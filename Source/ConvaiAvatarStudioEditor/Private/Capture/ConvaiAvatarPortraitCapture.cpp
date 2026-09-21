// Copyright Convai Inc. All Rights Reserved.
#include "Capture/ConvaiAvatarPortraitCapture.h"
#include "Workspace/ConvaiAvatarBlueprintSetup.h"
#include "ConvaiChatbotComponent.h"
#include "AssetCompilingManager.h"
#include "Animation/AnimInstance.h"
#include "CanvasTypes.h"
#include "Components/MeshComponent.h"
#include "Components/LODSyncComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ContentStreaming.h"
#include "Editor.h"
#include "Engine/AssetManager.h"
#include "Engine/Blueprint.h"
#include "Engine/LatentActionManager.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StreamableManager.h"
#include "Engine/StreamableRenderAsset.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineModule.h"
#include "EngineGlobals.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "ImageUtils.h"
#include "LegacyScreenPercentageDriver.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RendererInterface.h"
#include "RHI.h"
#include "ShaderCompiler.h"
#include "UnrealEngine.h"
#include "SceneView.h"
#include "ThumbnailHelpers.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

namespace
{
	// The cloud square crop requires a native 1024x2048 RGBA portrait. Unreal's
	// thumbnail view uses a square projection, so render at full height and take
	// the central portrait pixels without resizing or distorting the character.
	constexpr int32 PortraitPixelWidth = 1024, PortraitPixelHeight = 2048, RenderSide = PortraitPixelHeight;
	// Leave room below the avatar as well as above the head: the upward camera
	// aim previously pushed the feet outside a portrait filled to 92% height.
	constexpr float FieldOfView = 18.0f, HeightFill = 0.70f, WidthFill = 0.92f, AimUp = 0.28f;
	constexpr double WarmupSeconds = 3.0, MaximumSeconds = 120.0;

	FVector CameraOffsetForBounds(const FVector& Extent, float FOV)
	{
		// The fixed thumbnail camera looks along -X: Y spans the image and X is
		// depth. Fit the nearest box face to the central portrait, rather than
		// treating a prop extending toward the camera as extra screen width.
		const double VerticalFit = FMath::Max(Extent.Z, 1.0) / HeightFill;
		const double HorizontalFit = Extent.Y / (WidthFill * static_cast<double>(PortraitPixelWidth) / RenderSide);
		const double NearestFaceDistance = FMath::Max(VerticalFit, HorizontalFit) / FMath::Tan(FMath::DegreesToRadians(FOV) * 0.5f);
		return FVector(Extent.X + NearestFaceDistance, 0, Extent.Z * AimUp);
	}

	bool NormalPath(FString Path)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
		for (;;)
		{
			if (Files.IsSymlink(*Path) != ESymlinkResult::NonSymlink) return false;
			const FString Parent = FPaths::GetPath(Path);
			if (Parent.IsEmpty() || Parent == Path) break;
			Path = Parent;
		}
		return true;
	}

	bool VisibleGeometry(const UPrimitiveComponent* Component)
	{
		if (!Component || !Component->IsVisible() || Component->bHiddenInGame) return false;
		if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component)) return Mesh->GetStaticMesh() != nullptr;
		if (const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Component)) return Mesh->GetSkeletalMeshAsset() != nullptr;
		return false;
	}

	/** V0 portrait lighting/framing in a private, nontransactional editor preview world. */
	class FPortraitScene final : public FThumbnailPreviewScene
	{
	public:
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        FPortraitScene() : FThumbnailPreviewScene()
        {
            // Legacy thumbnail constructors always add a sky sphere and floor.
            // Remove only the constructor's meshes before an avatar is spawned.
            TArray<UStaticMeshComponent*> BackgroundComponents;
            for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
                if (It->GetWorld() == GetWorld()) BackgroundComponents.Add(*It);
            for (UStaticMeshComponent* Component : BackgroundComponents) RemoveComponent(Component);
        }
#else
        FPortraitScene() : FThumbnailPreviewScene(FConstructionValues().SetCreateSkySphere(false).SetCreateFloorPlane(false)) {}
#endif
		~FPortraitScene() override
		{
			for (const FResidency& Lease : Residency)
				if (UStreamableRenderAsset* Asset = Lease.Asset.Get(); Asset && !Lease.bAlreadyForced) Asset->SetForceMipLevelsToBeResident(-1.0f);
			if (CameraTarget.IsValid()) CameraTarget->Destroy();
			if (Avatar.IsValid()) Avatar->Destroy();
		}

		bool Initialize(UBlueprint* Blueprint, FString& Error)
		{
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Spawn.ObjectFlags = RF_Transient;
			Avatar = GetWorld()->SpawnActor<AActor>(Blueprint->GeneratedClass, Spawn);
			if (!Avatar.IsValid() || !Avatar->GetRootComponent())
			{ Error = TEXT("This Blueprint could not be placed in the thumbnail preview."); return false; }
			CameraTarget = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), Spawn);
			if (!CameraTarget.IsValid()) { Error = TEXT("The thumbnail camera target could not be created."); return false; }
			USceneComponent* TargetRoot = NewObject<USceneComponent>(CameraTarget.Get(), NAME_None, RF_Transient);
			CameraTarget->SetRootComponent(TargetRoot);
			TargetRoot->RegisterComponent();

			// No BeginPlay is dispatched. In addition to disabling auto-connect, clear
			// the preview instance's ID: BeginPlay's details request is a separate path.
			TInlineComponentArray<UConvaiChatbotComponent*> Chatbots(Avatar.Get());
			for (UConvaiChatbotComponent* Chatbot : Chatbots)
			{
				Chatbot->bAutoInitializeSession = false;
				Chatbot->CharacterID.Reset();
				Chatbot->bAutoActivate = false;
				Chatbot->SetComponentTickEnabled(false);
				Chatbot->LookAtTarget = CameraTarget.Get();
			}
			Avatar->SetActorTickEnabled(false);
			Avatar->SetActorEnableCollision(false);
			TInlineComponentArray<USkeletalMeshComponent*> AvatarSkeletalComponents(Avatar.Get());
			for (USkeletalMeshComponent* Mesh : AvatarSkeletalComponents)
			{
				if (!Mesh->GetSkeletalMeshAsset()) continue;
				Mesh->SetUpdateAnimationInEditor(true);
				Mesh->SetUpdateClothInEditor(false);
				Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
				Mesh->bEnableUpdateRateOptimizations = false;
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
				// Older engines retain their normal editor-preview notify behavior.
				Mesh->bSuppressNotifyEventDispatch = true;
#endif
				Mesh->SuspendClothingSimulation();
				Meshes.Add(Mesh);
			}
			// Leader/body first, then face and clothing followers, so CopyPose reads
			// the current body pose. These are the only components we tick ourselves.
			Meshes.StableSort([](const TWeakObjectPtr<USkeletalMeshComponent>& A, const TWeakObjectPtr<USkeletalMeshComponent>& B)
			{
				const bool ABody = A.IsValid() && A->GetName().Contains(TEXT("Body"), ESearchCase::IgnoreCase);
				const bool BBody = B.IsValid() && B->GetName().Contains(TEXT("Body"), ESearchCase::IgnoreCase);
				return ABody && !BBody;
			});
			if (!Measure()) { Error = TEXT("This Blueprint has no visible mesh to capture."); return false; }
			Avatar->SetActorLocation(Avatar->GetActorLocation() - Bounds.Origin);
			if (!Measure()) { Error = TEXT("The avatar has invalid preview bounds."); return false; }
			InitialBoundsDiagnostic = DescribeBounds(TEXT("initial"));
			RequestStreaming();
			return true;
		}

		bool ResourcesReady() const
		{
			// Mirrors the Auto Tagger's scoped readiness contract, plus each preview
			// primitive's IsCompiling (groom components report pending binding builds).
			for (const TWeakObjectPtr<UPrimitiveComponent>& Primitive : Primitives)
				if (Primitive.IsValid() && Primitive->IsCompiling()) return false;
#if UE_VERSION_OLDER_THAN(5, 4, 0)
            // UE 5.3 has no per-material readiness API. Wait conservatively.
            if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling()) return false;
#else
            for (const TWeakObjectPtr<UMaterialInterface>& Material : Materials)
                if (Material.IsValid() && Material->IsCompiling()) return false;
#endif
			auto& Streaming = IStreamingManager::Get().GetRenderAssetStreamingManager();
			for (const FResidency& Lease : Residency)
			{
				UStreamableRenderAsset* Asset = Lease.Asset.Get();
				if (!Asset) continue;
				if (Asset->HasPendingInitOrStreaming()) return false;
				if (const UTexture* Texture = Cast<UTexture>(Asset); Texture && Texture->IsCompiling()) return false;
				const FStreamableRenderResourceState& Resource = Asset->GetStreamableResourceState();
				if (Asset->IsStreamable() && (!Resource.IsValid() || (Resource.bSupportsStreaming && !UsesVirtualStreaming(Asset) && !Streaming.IsFullyStreamedIn(Asset)))) return false;
			}
			return true;
		}

		bool ReadyForAnimation() const
		{
			for (const TWeakObjectPtr<UPrimitiveComponent>& Item : Primitives)
				if (Item.IsValid() && Item->IsCompiling()) return false;
			for (const TWeakObjectPtr<USkeletalMeshComponent>& Item : Meshes)
				if (const USkeletalMeshComponent* Mesh = Item.Get(); Mesh && Mesh->GetSkeletalMeshAsset()->IsCompiling()) return false;
			return true;
		}

		void Advance(float DeltaTime)
		{
			// Advancing only this preview's clock makes AnimBP GetWorldDeltaSeconds
			// useful without World::Tick, gameplay timers, actor ticks or PIE.
			UWorld* World = GetWorld();
			World->DeltaTimeSeconds = DeltaTime;
			World->DeltaRealTimeSeconds = DeltaTime;
			World->TimeSeconds += DeltaTime;
			World->UnpausedTimeSeconds += DeltaTime;
			World->RealTimeSeconds += DeltaTime;
			World->GetLatentActionManager().BeginFrame();
			TInlineComponentArray<ULODSyncComponent*> LODSyncComponents(Avatar.Get());
			for (ULODSyncComponent* Sync : LODSyncComponents)
                if (Sync->IsRegistered())
                {
#if UE_VERSION_OLDER_THAN(5, 4, 0)
                    Sync->RefreshSyncComponents(); // Also updates LOD, without gameplay ticks.
#else
                    Sync->UpdateLOD();
#endif
                }
			for (const TWeakObjectPtr<USkeletalMeshComponent>& Item : Meshes)
			{
				USkeletalMeshComponent* Mesh = Item.Get();
				if (!Mesh || !Mesh->IsRegistered()) continue;
				// Match the native skinned-mesh pose path without dispatching a
				// Blueprint component's ReceiveTick or latent gameplay actions.
				const bool bLODChanged = Mesh->UpdateLODStatus();
				if (Mesh->ShouldTickPose()) Mesh->TickPose(DeltaTime, false);
				if (Mesh->ShouldUpdateTransform(bLODChanged))
				{
					if (Mesh->LeaderPoseComponent.IsValid()) Mesh->UpdateFollowerComponent();
					else Mesh->RefreshBoneTransforms();
				}
				Mesh->UpdateComponentToWorld();
				Mesh->UpdateBounds();
				Mesh->MarkRenderTransformDirty();
				Mesh->MarkRenderDynamicDataDirty();
			}
			// AnimBP Delay nodes normally resume in the world's latent-action pass.
			// Advance only this avatar's live animation instances; a whole-world pass
			// would also execute actor/component gameplay that this preview never starts.
			TSet<TWeakObjectPtr<UAnimInstance>> AnimationInstances;
			for (const TWeakObjectPtr<USkeletalMeshComponent>& Item : Meshes)
			{
				if (const USkeletalMeshComponent* Mesh = Item.Get(); Mesh && Mesh->IsRegistered())
				{
					AnimationInstances.Add(Mesh->GetAnimInstance());
					AnimationInstances.Add(Mesh->GetPostProcessInstance());
					for (UAnimInstance* Linked : Mesh->GetLinkedAnimInstances()) AnimationInstances.Add(Linked);
				}
			}
			for (const TWeakObjectPtr<UAnimInstance>& Item : AnimationInstances)
			{
				if (UAnimInstance* Instance = Item.Get(); Instance && Instance->GetWorld() == World
					&& World->GetLatentActionManager().GetNumActionsForObject(Instance) > 0)
					World->GetLatentActionManager().ProcessLatentActions(Instance, DeltaTime);
			}
			// Attached groom/other primitive transforms follow the refreshed bones.
			// No generic component gameplay tick or physics simulation is dispatched.
			for (const TWeakObjectPtr<UPrimitiveComponent>& Item : Primitives)
			{
				UPrimitiveComponent* Primitive = Item.Get();
				if (!Primitive || !Primitive->IsRegistered() || Primitive->IsCompiling()) continue;
				// UE swaps groom deformation buffers during native component ticks.
				// Exact-class dispatch avoids Blueprint ReceiveTick/latent actions and
				// lets projects without HairStrands keep using ordinary avatars.
				if (Primitive->GetClass()->GetPathName() == TEXT("/Script/HairStrandsCore.GroomComponent"))
					Primitive->TickComponent(DeltaTime, LEVELTICK_All, nullptr);
				Primitive->UpdateComponentToWorld();
				Primitive->UpdateBounds();
				Primitive->MarkRenderTransformDirty();
				Primitive->MarkRenderDynamicDataDirty();
			}
			UpdateCaptureContents();
		}

		bool Render(UTextureRenderTarget2D* Target, TArray<FColor>* Pixels, FString& Error)
		{
			FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
			if (!Resource) { Error = TEXT("The thumbnail render target is unavailable."); return false; }
			IConsoleVariable* Alpha = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PostProcessing.PropagateAlpha"));
			if (!Alpha) { Error = TEXT("This renderer does not provide transparent thumbnail output."); return false; }
			// Restore the renderer setting after each synchronous preview render.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
			const int32 PreviousAlpha = Alpha->GetInt();
			Alpha->SetWithCurrentPriority(1);
			ON_SCOPE_EXIT { Alpha->SetWithCurrentPriority(PreviousAlpha); };
#else
			// Tagged history preserves any existing override at this priority.
			const FName AlphaTag(*(TEXT("ConvaiPortrait_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			constexpr EConsoleVariableFlags AlphaPriority = ECVF_SetByPreview;
#else
			constexpr EConsoleVariableFlags AlphaPriority = ECVF_SetByTemp;
#endif
			Alpha->Set(1, AlphaPriority, AlphaTag);
			ON_SCOPE_EXIT { Alpha->Unset(AlphaPriority, AlphaTag); };
#endif
			if (!Alpha->GetBool())
			{ Error = TEXT("Transparent thumbnails are disabled by a renderer override. Enable alpha output and capture again."); return false; }

			FCanvas Canvas(Resource, nullptr, GetWorld()->GetTime(), GetScene()->GetFeatureLevel());
			FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Resource, GetScene(), FEngineShowFlags(ESFIM_Game)).SetTime(GetWorld()->GetTime()));
			Family.EngineShowFlags.DisableAdvancedFeatures();
			Family.EngineShowFlags.MotionBlur = 0;
			Family.EngineShowFlags.SetAtmosphere(false);
			Family.EngineShowFlags.SetFog(false);
			Family.EngineShowFlags.ScreenPercentage = false;
			Family.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(Family, 1.0f));
			FSceneView* View = CreateView(&Family, 0, 0, RenderSide, RenderSide);
			if (!View) { Error = TEXT("The avatar camera could not be framed."); return false; }
			// Thumbnail views initialize ViewMatrices, not the optional ViewLocation field.
			CameraTarget->SetActorLocation(View->ViewMatrices.GetViewOrigin());
			GetRendererModule().BeginRenderingViewFamily(&Canvas, &Family);
			FlushRenderingCommands();
			if (!Pixels) return true;
			if (!Resource->ReadPixels(*Pixels) || Pixels->Num() != RenderSide * RenderSide)
			{ Error = TEXT("The rendered avatar image could not be read."); return false; }
			for (FColor& Pixel : *Pixels) Pixel.A = 255 - Pixel.A;
			return true;
		}

		void LogRejectedImageBounds() const
		{
			UE_LOG(LogTemp, Display, TEXT("Convai portrait bounds: %s"), *InitialBoundsDiagnostic);
			UE_LOG(LogTemp, Display, TEXT("Convai portrait bounds: %s"), *DescribeBounds(TEXT("final")));
		}

	protected:
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
        float GetFOV() const override { return FieldOfView; }
        bool ShouldClampOrbitZoom() const override { return false; }
#endif
		void GetViewMatrixParameters(float FOV, FVector& Origin, float& Pitch, float& Yaw, float& Zoom) const override
		{
			const FVector CameraOffset = CameraOffsetForBounds(Bounds.BoxExtent, FOV);
			Origin = -Bounds.Origin + FVector(0, 0, -CameraOffset.Z);
			Pitch = 0; Yaw = -90;
			Zoom = static_cast<float>(CameraOffset.X);
		}

    private:
        static bool UsesVirtualStreaming(const UStreamableRenderAsset* Asset)
        {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
            const UTexture* Texture = Cast<UTexture>(Asset);
            return Texture && Texture->VirtualTextureStreaming;
#else
            return Asset->GetStreamableResourceState().bSupportsVirtualStreaming;
#endif
        }
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        // Legacy thumbnail views hard-code a 30-degree FOV and minimum distance.
        // Keep the same 18-degree portrait framing and small-avatar support.
        FSceneView* CreateView(FSceneViewFamily* Family, int32 X, int32 Y, uint32 Width, uint32 Height) const
        {
            const FVector CameraOffset = CameraOffsetForBounds(Bounds.BoxExtent, FieldOfView);
            FSceneViewInitOptions Options;
            Options.ViewFamily = Family;
            Options.SetViewRectangle(FIntRect(X, Y, X + Width, Y + Height));
            Options.ViewOrigin = Bounds.Origin + CameraOffset;
            Options.ViewRotationMatrix = FInverseRotationMatrix(FRotator(0, 180, 0)) * FMatrix(
                FPlane(0,0,1,0), FPlane(1,0,0,0), FPlane(0,1,0,0), FPlane(0,0,0,1));
            Options.ProjectionMatrix = FReversedZPerspectiveMatrix(FMath::DegreesToRadians(FieldOfView) * 0.5f, 1.f, 1.f, 1.f);
            Options.BackgroundColor = FLinearColor::Black;
            FSceneView* View = new FSceneView(Options);
            Family->Views.Add(View);
            View->StartFinalPostprocessSettings(Options.ViewOrigin);
            View->EndFinalPostprocessSettings(Options);
            IStreamingManager::Get().AddViewInformation(Options.ViewOrigin, Width,
                Width / FMath::Tan(FMath::DegreesToRadians(FieldOfView) * 0.5f));
            return View;
        }
#endif
        FString DescribeBounds(const TCHAR* Phase) const
		{
			FString Result = FString::Printf(TEXT("phase=%s framingOrigin=%s framingExtent=%s camera=%s"), Phase,
				*Bounds.Origin.ToString(), *Bounds.BoxExtent.ToString(),
				CameraTarget.IsValid() ? *CameraTarget->GetActorLocation().ToString() : TEXT("unavailable"));
			TInlineComponentArray<UPrimitiveComponent*> DiagnosticPrimitives(Avatar.Get());
			const int32 Count = FMath::Min(DiagnosticPrimitives.Num(), 128);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const UPrimitiveComponent* Component = DiagnosticPrimitives[Index];
				const UObject* MeshAsset = nullptr;
				if (const UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(Component)) MeshAsset = StaticMesh->GetStaticMesh();
				else if (const USkeletalMeshComponent* SkeletalMesh = Cast<USkeletalMeshComponent>(Component)) MeshAsset = SkeletalMesh->GetSkeletalMeshAsset();
				Result += FString::Printf(TEXT("\n  component=%s class=%s included=%d registered=%d visible=%d hidden=%d origin=%s extent=%s location=%s scale=%s boundsScale=%.3f attachBounds=%d mesh=%s"),
					*Component->GetName().Left(256), *Component->GetClass()->GetName().Left(128), VisibleGeometry(Component),
					Component->IsRegistered(), Component->IsVisible(), Component->bHiddenInGame,
					*Component->Bounds.Origin.ToString(), *Component->Bounds.BoxExtent.ToString(),
					*Component->GetComponentLocation().ToString(), *Component->GetComponentScale().ToString(),
					Component->BoundsScale, Component->bUseAttachParentBound, *GetPathNameSafe(MeshAsset).Left(512));
			}
			if (DiagnosticPrimitives.Num() > Count) Result += TEXT("\n  additional components omitted");
			return Result;
		}

		struct FResidency { TWeakObjectPtr<UStreamableRenderAsset> Asset; bool bAlreadyForced = false; };
		void AddStreamingAsset(UStreamableRenderAsset* Asset)
		{
			if (!Asset || Residency.ContainsByPredicate([Asset](const FResidency& Lease) { return Lease.Asset.Get() == Asset; })) return;
			FResidency& Lease = Residency.AddDefaulted_GetRef();
			Lease.Asset = Asset; Lease.bAlreadyForced = Asset->ShouldMipLevelsBeForcedResident();
			// Leave an existing residency request's duration unchanged. Our own
			// requests are released on completion/cancel and never edit source assets.
			if (!Lease.bAlreadyForced) Asset->SetForceMipLevelsToBeResident(static_cast<float>(MaximumSeconds), 0);
			if (Asset->IsStreamable())
			{
				const FStreamableRenderResourceState& Resource = Asset->GetStreamableResourceState();
				if (!Resource.IsValid() || !UsesVirtualStreaming(Asset))
					IStreamingManager::Get().GetRenderAssetStreamingManager().UpdateIndividualRenderAsset(Asset);
			}
		}
		void RequestStreaming()
		{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
            check(GetCachedScalabilityCVars().bInitialized);
            const auto MaterialQuality = GetCachedScalabilityCVars().MaterialQualityLevel;
#else
            const auto MaterialQuality = GetCurrentMaterialQualityLevelChecked();
#endif
            TInlineComponentArray<UPrimitiveComponent*> StreamingPrimitives(Avatar.Get());
			for (UPrimitiveComponent* Component : StreamingPrimitives)
			{
				Primitives.Add(Component);
				if (UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(Component)) AddStreamingAsset(StaticMesh->GetStaticMesh());
				else if (USkeletalMeshComponent* SkeletalMesh = Cast<USkeletalMeshComponent>(Component)) AddStreamingAsset(SkeletalMesh->GetSkeletalMeshAsset());
				for (int32 Index = 0; Index < Component->GetNumMaterials(); ++Index)
					if (UMaterialInterface* Material = Component->GetMaterial(Index)) Materials.AddUnique(Material);
				TArray<UTexture*> Textures;
				Component->GetUsedTextures(Textures, MaterialQuality);
				for (UTexture* Texture : Textures) if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture)) AddStreamingAsset(Texture2D);
			}
		}
		bool Measure()
		{
			FBoxSphereBounds::Builder Builder;
			bool bAny = false;
			TInlineComponentArray<UPrimitiveComponent*> BoundsPrimitives(Avatar.Get());
			for (UPrimitiveComponent* Component : BoundsPrimitives)
			{
				if (VisibleGeometry(Component)) { Component->UpdateBounds(); Builder += Component->Bounds; bAny = true; }
			}
			if (!bAny) return false;
			Bounds = Builder;
			return !Bounds.Origin.ContainsNaN() && !Bounds.BoxExtent.ContainsNaN() && Bounds.SphereRadius > 0;
		}
		TWeakObjectPtr<AActor> Avatar, CameraTarget;
		TArray<TWeakObjectPtr<USkeletalMeshComponent>> Meshes;
		TArray<FResidency> Residency;
		TArray<TWeakObjectPtr<UPrimitiveComponent>> Primitives;
		TArray<TWeakObjectPtr<UMaterialInterface>> Materials;
		FBoxSphereBounds Bounds;
		FString InitialBoundsDiagnostic;
	};
}

struct FConvaiAvatarPortraitCapture::FState
{
	FSoftObjectPath SourcePath;
	TSharedPtr<FStreamableHandle> Load;
	TStrongObjectPtr<UBlueprint> Source, Preview;
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UTextureRenderTarget2D> Target;
	TUniquePtr<FPortraitScene> Scene;
	bool bMetaHuman = false;
	double Started = FPlatformTime::Seconds(), Warmed = 0, WarmupStarted = 0, LastRender = 0;
	uint64 LastFrame = MAX_uint64;
	int32 RenderedFrames = 0;
	~FState()
	{
		if (Load) Load->CancelHandle();
		Scene.Reset(); // Release preview world before its class and render target.
	}

	bool Prepare(FString& Error)
	{
		Source.Reset(Cast<UBlueprint>(SourcePath.ResolveObject()));
		if (!Source.IsValid() || !Source->GeneratedClass || Source->bBeingCompiled || Source->Status == BS_Error || !Source->GeneratedClass->IsChildOf(AActor::StaticClass()))
		{ Error = TEXT("Choose a compiled Actor Blueprint to capture an avatar thumbnail."); return false; }
		Package.Reset(CreatePackage(*(TEXT("/Temp/ConvaiAvatarPortrait_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		Package->SetFlags(RF_Transient);
		Preview.Reset(DuplicateObject<UBlueprint>(Source.Get(), Package.Get(), TEXT("PreviewAvatar")));
		if (!Preview.IsValid() || !Preview->GeneratedClass || Preview->GeneratedClass == Source->GeneratedClass || Preview->GeneratedClass->GetOutermost() != Package.Get())
		{ Error = TEXT("The avatar Blueprint could not be isolated for a thumbnail preview."); return false; }
		Preview->SetFlags(RF_Transient);
		TArray<FString> Changes;
		if (!ConvaiAvatarStudio::BlueprintSetup::PrepareAvatarBlueprint(Preview.Get(), bMetaHuman, Error, Changes)) return false;
		if (Preview->Status == BS_Error) { Error = TEXT("The avatar preview Blueprint did not compile. Check its Blueprint errors and try again."); return false; }
		Scene = MakeUnique<FPortraitScene>();
		if (!Scene->Initialize(Preview.Get(), Error)) return false;
		Target.Reset(NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient));
		Target->RenderTargetFormat = RTF_RGBA8_SRGB;
		Target->ClearColor = FLinearColor::Transparent;
		Target->InitAutoFormat(RenderSide, RenderSide);
		Target->UpdateResourceImmediate(true);
		// Establish the actual camera target before the first animation update.
		if (!Scene->Render(Target.Get(), nullptr, Error)) return false;
		WarmupStarted = FPlatformTime::Seconds();
		return true;
	}

	bool WriteImage(FString& Path, FString& Error)
	{
		TArray<FColor> Rendered;
		if (!Scene->Render(Target.Get(), &Rendered, Error)) return false;
		TArray<FColor> Portrait;
		Portrait.SetNumUninitialized(PortraitPixelWidth * PortraitPixelHeight);
		for (int32 Row = 0; Row < PortraitPixelHeight; ++Row)
			FMemory::Memcpy(Portrait.GetData() + Row * PortraitPixelWidth, Rendered.GetData() + Row * RenderSide + (RenderSide - PortraitPixelWidth) / 2, PortraitPixelWidth * sizeof(FColor));
		int32 Covered = 0, Transparent = 0, Left = PortraitPixelWidth, Top = PortraitPixelHeight, Right = -1, Bottom = -1;
		uint8 MinimumAlpha = 255, MaximumAlpha = 0;
		for (int32 Index = 0; Index < Portrait.Num(); ++Index)
		{
			const FColor& Pixel = Portrait[Index];
			if (Pixel.A > 0 && (Pixel.R > 5 || Pixel.G > 5 || Pixel.B > 5)) ++Covered;
			if (Pixel.A == 0) ++Transparent;
			else
			{
				Left = FMath::Min(Left, Index % PortraitPixelWidth); Right = FMath::Max(Right, Index % PortraitPixelWidth);
				Top = FMath::Min(Top, Index / PortraitPixelWidth); Bottom = FMath::Max(Bottom, Index / PortraitPixelWidth);
			}
			MinimumAlpha = FMath::Min(MinimumAlpha, Pixel.A); MaximumAlpha = FMath::Max(MaximumAlpha, Pixel.A);
		}
		if (Covered < Portrait.Num() / 100 || Transparent < Portrait.Num() / 100)
		{
			UE_LOG(LogTemp, Display, TEXT("Convai portrait rejected: size=%dx%d covered=%d transparent=%d total=%d alphaRange=%d..%d alphaBounds=(%d,%d)-(%d,%d)"),
				PortraitPixelWidth, PortraitPixelHeight, Covered, Transparent, Portrait.Num(), MinimumAlpha, MaximumAlpha, Left, Top, Right, Bottom);
			Scene->LogRejectedImageBounds();
#if WITH_DEV_AUTOMATION_TESTS
			// Rejected pixels are diagnostic evidence only. They never become a
			// thumbnail result or enter the normal upload folder.
			if (FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioTestPortraitCapture")))
			{
				const FString DiagnosticDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/ConvaiAvatarPortrait"));
				if (NormalPath(DiagnosticDirectory) && IFileManager::Get().MakeDirectory(*DiagnosticDirectory, true))
				{
					const FString DiagnosticStem = DiagnosticDirectory / (TEXT("Rejected-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
					auto SaveDiagnostic = [&DiagnosticStem](const TCHAR* Suffix, int32 ImageWidth, int32 ImageHeight, const TArray<FColor>& Image)
					{
						TArray64<uint8> Bytes;
						FImageUtils::PNGCompressImageArray(ImageWidth, ImageHeight, TArrayView64<const FColor>(Image.GetData(), Image.Num()), Bytes);
						const FString Filename = DiagnosticStem + Suffix;
						if (!Bytes.IsEmpty() && FFileHelper::SaveArrayToFile(Bytes, *Filename))
						{
							UE_LOG(LogTemp, Display, TEXT("Convai rejected portrait diagnostic: %s"), *Filename);
						}
						else IFileManager::Get().Delete(*Filename, false, true);
					};
					SaveDiagnostic(TEXT("-portrait.png"), PortraitPixelWidth, PortraitPixelHeight, Portrait);
					SaveDiagnostic(TEXT("-render.png"), RenderSide, RenderSide, Rendered);
				}
			}
#endif
			Error = TEXT("The thumbnail is blank or its background is not transparent. Check the avatar's visible meshes and try again."); return false;
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(PortraitPixelWidth, PortraitPixelHeight, TArrayView64<const FColor>(Portrait.GetData(), Portrait.Num()), Png);
		if (Png.IsEmpty()) { Error = TEXT("The avatar thumbnail could not be encoded as PNG."); return false; }
		const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/Thumbnails"));
		if (!NormalPath(Directory) || !IFileManager::Get().MakeDirectory(*Directory, true))
		{ Error = TEXT("The thumbnail folder is unavailable or redirects outside this project."); return false; }
		Path = Directory / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".png"));
		if (!FFileHelper::SaveArrayToFile(Png, *Path))
		{ IFileManager::Get().Delete(*Path, false, true); Path.Reset(); Error = TEXT("The captured thumbnail could not be saved. Check free space and folder access."); return false; }
		return true;
	}
};

FConvaiAvatarPortraitCapture::FConvaiAvatarPortraitCapture() = default;
FConvaiAvatarPortraitCapture::~FConvaiAvatarPortraitCapture() { Cancel(); }

bool FConvaiAvatarPortraitCapture::IsAvailable(FString& Reason)
{
	Reason.Reset();
	if (!GEditor || !GIsRHIInitialized || GUsingNullRHI) Reason = TEXT("Thumbnail capture needs the editor's graphics renderer. Open the project normally and try again.");
	else if (GEditor->PlayWorld) Reason = TEXT("Stop Play or Simulate before capturing an avatar thumbnail.");
	return Reason.IsEmpty();
}

void FConvaiAvatarPortraitCapture::Start(const FSoftObjectPath& Blueprint, bool bIsMetaHuman, FCompletion Completion)
{
	check(IsInGameThread());
	FString Error;
	if (State) Error = TEXT("Another avatar thumbnail is still being prepared.");
	else if (!Blueprint.IsValid()) Error = TEXT("Choose an avatar Blueprint before capturing a thumbnail.");
	else IsAvailable(Error);
	if (!Error.IsEmpty()) { if (Completion) Completion(FString(), Error); return; }
	OnComplete = MoveTemp(Completion);
	State = MakeUnique<FState>(); State->SourcePath = Blueprint; State->bMetaHuman = bIsMetaHuman;
	State->Load = UAssetManager::GetStreamableManager().RequestAsyncLoad(Blueprint);
	if (!State->Load) { Finish(FString(), TEXT("The selected avatar Blueprint could not be loaded.")); return; }
	TWeakPtr<FConvaiAvatarPortraitCapture> Weak = AsShared();
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float DeltaTime)
	{
		const TSharedPtr<FConvaiAvatarPortraitCapture> Self = Weak.Pin();
		return Self && Self->Tick(DeltaTime);
	}), 0.001f);
}

bool FConvaiAvatarPortraitCapture::Tick(float DeltaTime)
{
	check(IsInGameThread());
	if (!State) return false;
	FString Error;
	if (!IsAvailable(Error)) { Finish(FString(), Error); return false; }
	const double Now = FPlatformTime::Seconds();
	if (Now - State->Started > MaximumSeconds)
	{ Finish(FString(), TEXT("The avatar preview is taking too long to load or compile. Wait for the editor to finish preparing assets, then capture again.")); return false; }
	if (!State->Load->HasLoadCompleted()) return true;
	if (!State->Scene && !State->Prepare(Error)) { Finish(FString(), Error); return false; }
	FAssetCompilingManager::Get().ProcessAsyncTasks(true);
	if (!State->Scene->ReadyForAnimation() || State->LastFrame == GFrameCounter) return true;
	State->LastFrame = GFrameCounter;
	const float Step = FMath::Clamp(DeltaTime, 0.0f, 1.0f / 15.0f);
	State->Scene->Advance(Step); State->Warmed += Step;
	if (Now - State->LastRender >= 0.25)
	{
		if (!State->Scene->Render(State->Target.Get(), nullptr, Error)) { Finish(FString(), Error); return false; }
		State->LastRender = Now;
		++State->RenderedFrames;
	}
	if (State->Warmed < WarmupSeconds || Now - State->WarmupStarted < WarmupSeconds || State->RenderedFrames < 5 || !State->Scene->ResourcesReady()) return true;
	FString Output;
	State->WriteImage(Output, Error);
	Finish(Output, Error);
	return false;
}

void FConvaiAvatarPortraitCapture::Cancel()
{
	check(IsInGameThread());
	OnComplete = {};
	if (TickHandle.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(TickHandle); TickHandle.Reset(); }
	State.Reset();
}

void FConvaiAvatarPortraitCapture::Finish(const FString& Path, const FString& Error)
{
	FCompletion Completion = MoveTemp(OnComplete);
	Cancel();
	if (Completion) Completion(Path, Error);
}

bool FConvaiAvatarPortraitCapture::IsBusy() const { return State.IsValid(); }
#if WITH_DEV_AUTOMATION_TESTS
FVector FConvaiAvatarPortraitCapture::GetCameraOffsetForTests(const FVector& BoundsExtent) { return CameraOffsetForBounds(BoundsExtent, FieldOfView); }
UWorld* FConvaiAvatarPortraitCapture::GetPreviewWorldForTests() const { return State && State->Scene ? State->Scene->GetWorld() : nullptr; }
UBlueprint* FConvaiAvatarPortraitCapture::GetPreviewBlueprintForTests() const { return State ? State->Preview.Get() : nullptr; }
#endif
