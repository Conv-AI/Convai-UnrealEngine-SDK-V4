// Copyright Convai Inc. All Rights Reserved.

#include "Diorama/ConvaiAvatarDiorama.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "ConvaiDefinitions.h"
#include "ConvaiObjectComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/Brush.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/LevelStreaming.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreMisc.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/EnumRange.h"
#include "Misc/PackageName.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "RenderUtils.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshCompiler.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "ConvaiAvatarDiorama"

bool FConvaiAvatarDioramaFacts::HasCustomLighting() const
{
    // Static lights are left out: they need a build the studio never runs, so a level lit only by
    // them arrives dark - and must not be the reason the studio's own lighting stands down.
    return DynamicLights.Num() > 0 || bSkyLight || bSkyAtmosphere || bHeightFog || bVolumetricCloud;
}

bool FConvaiAvatarDioramaFacts::HasCustomCamera() const
{
    return !CameraActorName.IsNone();
}

int32 FConvaiAvatarDioramaReport::Count(const EConvaiAvatarDioramaSeverity Severity) const
{
    int32 Total = 0;
    for (const FConvaiAvatarDioramaIssue& Issue : Issues)
    {
        if (Issue.Severity == Severity)
        {
            ++Total;
        }
    }
    return Total;
}

namespace ConvaiAvatarDiorama
{
namespace
{
    FConvaiAvatarDioramaIssue Issue(const EConvaiAvatarDioramaSeverity Severity, FString Reason, const FConvaiAvatarDioramaActor* Actor = nullptr)
    {
        FConvaiAvatarDioramaIssue Result;
        Result.Severity = Severity;
        Result.Reason = MoveTemp(Reason);
        if (Actor)
        {
            Result.Target = Actor->Path;
            Result.ActorGuid = Actor->Guid;
        }
        return Result;
    }

    FString Labels(const TArray<FConvaiAvatarDioramaActor>& Actors)
    {
        TArray<FString> Names;
        Names.Reserve(Actors.Num());
        for (const FConvaiAvatarDioramaActor& Actor : Actors)
        {
            Names.Add(Actor.Label);
        }
        return FString::Join(Names, TEXT(", "));
    }

    /** The one fact each limit is measured against. Walkthrough S4's limit-to-fact table. */
    double Measure(const FConvaiAvatarDioramaFacts& Facts, const EConvaiAvatarDioramaLimit Limit)
    {
        switch (Limit)
        {
        case EConvaiAvatarDioramaLimit::DynamicLights: return Facts.DynamicLights.Num();
        case EConvaiAvatarDioramaLimit::ShadowLights: return Facts.ShadowCastingLights.Num();
        // Components, not instances: an instanced mesh is one thing to draw with, however many it draws.
        case EConvaiAvatarDioramaLimit::StaticMeshes: return Facts.StaticMeshComponents;
        case EConvaiAvatarDioramaLimit::Triangles: return Facts.Triangles;
        case EConvaiAvatarDioramaLimit::Textures: return Facts.TexturesCount;
        case EConvaiAvatarDioramaLimit::TextureMaxSize: return Facts.TexturesMaxBuiltPx;
        case EConvaiAvatarDioramaLimit::TextureMemoryMb: return Facts.TexturesMemoryMb;
        case EConvaiAvatarDioramaLimit::ConvaiObjects: return Facts.ConvaiObjects;
        case EConvaiAvatarDioramaLimit::Actors: return Facts.Actors;
        default: return 0.0;
        }
    }

    FConvaiAvatarDioramaIssue LimitIssue(
        const FConvaiAvatarDioramaFacts& Facts, const EConvaiAvatarDioramaLimit Limit, const FConvaiAvatarDioramaLimitRule& Rule, const double Measured)
    {
        FConvaiAvatarDioramaIssue Result;
        Result.Severity = Rule.Severity;
        Result.Measured = Measured;
        Result.Limit = Rule.Max;

        switch (Limit)
        {
        case EConvaiAvatarDioramaLimit::TextureMaxSize:
            Result.Target = Facts.LargestTexture;
            Result.Reason = FString::Printf(TEXT("Texture too large: %s - %.0f px, max %.0f px"),
                *Facts.LargestTexture.GetAssetName(), Measured, Rule.Max);
            if (!Facts.LargestTextureContext.IsEmpty())
            {
                Result.Reason += FString::Printf(TEXT(" (%s)"), *Facts.LargestTextureContext);
            }
            break;

        case EConvaiAvatarDioramaLimit::DynamicLights:
            if (Facts.DynamicLights.Num() > 0)
            {
                Result.Target = Facts.DynamicLights[0].Path;
                Result.ActorGuid = Facts.DynamicLights[0].Guid;
            }
            Result.Reason = FString::Printf(TEXT("Too many dynamic lights: %s - %.0f, max %.0f"),
                *Labels(Facts.DynamicLights), Measured, Rule.Max);
            break;

        case EConvaiAvatarDioramaLimit::ShadowLights:
            if (Facts.ShadowCastingLights.Num() > 0)
            {
                Result.Target = Facts.ShadowCastingLights[0].Path;
                Result.ActorGuid = Facts.ShadowCastingLights[0].Guid;
            }
            Result.Reason = FString::Printf(TEXT("Shadow-casting light%s: %s - %.0f, max %.0f"),
                Facts.ShadowCastingLights.Num() == 1 ? TEXT("") : TEXT("s"),
                *Labels(Facts.ShadowCastingLights), Measured, Rule.Max);
            break;

        case EConvaiAvatarDioramaLimit::StaticMeshes:
            Result.Reason = FString::Printf(TEXT("Static meshes: %.0f components (%d instances), max %.0f"),
                Measured, Facts.StaticMeshInstances, Rule.Max);
            break;

        case EConvaiAvatarDioramaLimit::Triangles:
            Result.Reason = FString::Printf(TEXT("Triangles: %.0f, max %.0f"), Measured, Rule.Max);
            break;

        case EConvaiAvatarDioramaLimit::Textures:
            Result.Reason = FString::Printf(TEXT("Textures: %.0f including the avatar's, max %.0f"), Measured, Rule.Max);
            break;

        case EConvaiAvatarDioramaLimit::TextureMemoryMb:
            Result.Reason = FString::Printf(TEXT("Texture memory: %.0f MB, max %.0f MB"), Measured, Rule.Max);
            break;

        case EConvaiAvatarDioramaLimit::ConvaiObjects:
            Result.Reason = FString::Printf(TEXT("Convai objects: %.0f, max %.0f"), Measured, Rule.Max);
            break;

        case EConvaiAvatarDioramaLimit::Actors:
            Result.Reason = FString::Printf(TEXT("Actors: %.0f, max %.0f"), Measured, Rule.Max);
            break;

        default:
            break;
        }

        return Result;
    }

    /** {x,y,z} and {pitch,yaw,roll} both, because the studio's importer reads an object, not an array. */
    TSharedPtr<FJsonObject> Triple(
        const TCHAR* KeyA, const double A, const TCHAR* KeyB, const double B, const TCHAR* KeyC, const double C)
    {
        const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(KeyA, A);
        Object->SetNumberField(KeyB, B);
        Object->SetNumberField(KeyC, C);
        return Object;
    }

    FString Label(const AActor* Actor)
    {
        // The default argument writes a made-up label into an actor that has none, and a scan has to
        // leave the level exactly as it found it.
        const FString Authored = Actor->GetActorLabel(/*bCreateIfNone=*/false);
        return Authored.IsEmpty() ? Actor->GetName() : Authored;
    }

    FConvaiAvatarDioramaActor Handle(const AActor* Actor)
    {
        FConvaiAvatarDioramaActor Result;
        Result.Label = Label(Actor);
        Result.Path = FSoftObjectPath(Actor);
        Result.Guid = Actor->GetActorGuid();
        return Result;
    }

    /**
     * Whether one ancestry chain reaches a placed avatar. Child-actor parenting and attachment are
     * unrelated relationships and the diorama reads them apart: a child actor of the avatar was never
     * placed, an actor attached to it was and is only warned about.
     */
    bool ChainReachesAvatar(const AActor* Actor, const TSet<const AActor*>& Avatars, const bool bAttachment)
    {
        const AActor* Current = Actor;
        // A cycle cannot happen, but a scan is not where anyone wants to find out.
        for (int32 Step = 0; Current && Step < 64; ++Step)
        {
            Current = bAttachment ? Current->GetAttachParentActor() : Current->GetParentActor();
            if (Current && Avatars.Contains(Current))
            {
                return true;
            }
        }
        return false;
    }

    /** What the walk gathers, so the reads that stall on compilation all happen once, at the end. */
    struct FCollected
    {
        /** Unique, so the wait for compilation is one wait. */
        TArray<UStaticMesh*> Meshes;

        /** Per component with its instance count, so a mesh drawn twice is counted twice. */
        TArray<TPair<UStaticMesh*, int32>> MeshUses;

        TArray<USkeletalMesh*> SkeletalUses;

        TSet<UTexture*> Textures;

        /** Kept apart because only the count takes them in - see FConvaiAvatarDioramaFacts::TexturesMaxBuiltPx. */
        TSet<UTexture*> AvatarTextures;

        /** "M_Desk on SM_Desk", from the first use that found the texture. */
        TMap<UTexture*, FString> Context;
    };

    void CollectTextures(const AActor* Actor, const UMeshComponent* Mesh, FCollected& Collected, const bool bAvatar)
    {
        TSet<UTexture*>& Found = bAvatar ? Collected.AvatarTextures : Collected.Textures;

        TArray<UMaterialInterface*> Materials;
        // Not every override resets the array, and UPrimitiveComponent's own version writes nothing.
        Mesh->GetUsedMaterials(Materials);

        for (UMaterialInterface* Material : Materials)
        {
            if (!Material)
            {
                continue;
            }

            TArray<UTexture*> Used;
            // The overload taking quality and feature levels is deprecated with an empty body: it still
            // compiles, returns nothing, and would read every diorama as textureless.
#if UE_VERSION_OLDER_THAN(5, 7, 0)
            Material->GetUsedTextures(Used, EMaterialQualityLevel::High, true, ERHIFeatureLevel::SM5, true);
#else
            Material->GetUsedTextures(Used);
#endif

            for (UTexture* Texture : Used)
            {
                if (!Texture)
                {
                    continue;
                }

                bool bSeen = false;
                Found.Add(Texture, &bSeen);
                if (!bSeen && !bAvatar)
                {
                    Collected.Context.Add(Texture,
                        FString::Printf(TEXT("%s on %s"), *Material->GetName(), *Label(Actor)));
                }
            }
        }
    }

    void CollectComponents(AActor* Actor, FConvaiAvatarDioramaFacts& Facts, FCollected& Collected)
    {
        TInlineComponentArray<UActorComponent*> Components(Actor);
        for (UActorComponent* Component : Components)
        {
            // Visualization components are the camera's proxy mesh and frustum, the billboards, the
            // arrows: they draw for the creator here and for nobody in the studio.
            if (!Component || Component->IsEditorOnly() || Component->IsVisualizationComponent())
            {
                continue;
            }

            if (const ULightComponent* Light = Cast<ULightComponent>(Component))
            {
                if (Light->bAffectsWorld)
                {
                    // Static only: a Stationary light still renders without a build, so it is not one of
                    // the lights that arrive dark.
                    (Light->HasStaticLighting() ? Facts.StaticLights : Facts.DynamicLights).Add(Handle(Actor));
                    if (Light->CastShadows && Light->CastDynamicShadows)
                    {
                        Facts.ShadowCastingLights.Add(Handle(Actor));
                    }
                }
                continue;
            }

            // A sky light hangs off ULightComponentBase rather than ULightComponent, so the cast above
            // never sees one and the light counts never include one.
            if (const USkyLightComponent* Sky = Cast<USkyLightComponent>(Component))
            {
                if (Sky->bAffectsWorld)
                {
                    Facts.bSkyLight = true;
                }
                continue;
            }

            UMeshComponent* Mesh = Cast<UMeshComponent>(Component);
            if (!Mesh)
            {
                continue;
            }

            if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Mesh))
            {
                ++Facts.StaticMeshComponents;

                const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Static);
                const int32 Instances = Instanced ? Instanced->GetInstanceCount() : 1;
                if (Instanced)
                {
                    Facts.StaticMeshInstances += Instances;
                }

                UStaticMesh* Asset = Static->GetStaticMesh();
                if (Asset)
                {
                    Collected.Meshes.AddUnique(Asset);
                    Collected.MeshUses.Emplace(Asset, Instances);
                }
            }
            else if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Mesh))
            {
                if (USkeletalMesh* Asset = Skeletal->GetSkeletalMeshAsset())
                {
                    Collected.SkeletalUses.Add(Asset);
                }
            }

            CollectTextures(Actor, Mesh, Collected, /*bAvatar=*/false);
        }
    }

    /** The camera choice needs the actors themselves, which the handles in the facts do not carry. */
    struct FCameraPick
    {
        TArray<const AActor*> Actors;
        const AActor* Tagged = nullptr;
    };

    void CollectPlacedActor(AActor* Actor, FConvaiAvatarDioramaFacts& Facts, FCameraPick& Cameras)
    {
        // Cine cameras derive from ACameraActor, so this one test is both.
        if (const ACameraActor* Camera = Cast<ACameraActor>(Actor))
        {
            Facts.Cameras.Add(Handle(Actor));
            Cameras.Actors.Add(Actor);

            // Disabled reads as INDEX_NONE; 0 is Player 0, which does take the view.
            if (Camera->GetAutoActivatePlayerIndex() != INDEX_NONE)
            {
                Facts.AutoActivatingCameras.Add(Handle(Actor));
            }

            TInlineComponentArray<UCameraComponent*> Lenses(Actor);
            for (const UCameraComponent* Lens : Lenses)
            {
                if (Lens && Lens->bConstrainAspectRatio != 0)
                {
                    Facts.AspectConstrainedCameras.Add(Handle(Actor));
                    break;
                }
            }

            if (Actor->ActorHasTag(FName(TEXT("Convai.Diorama.Camera"))))
            {
                Cameras.Tagged = Actor;
            }
        }

        if (Actor->IsA<ANavMeshBoundsVolume>())
        {
            Facts.bNavMeshBounds = true;
        }
        if (Actor->IsA<ASkyAtmosphere>())
        {
            Facts.bSkyAtmosphere = true;
        }
        if (Actor->IsA<AExponentialHeightFog>())
        {
            Facts.bHeightFog = true;
        }
        if (Actor->IsA<AVolumetricCloud>())
        {
            Facts.bVolumetricCloud = true;
        }

        // The avatar is already out of this walk, so every pawn left is one the studio has no player for.
        if (Actor->IsA<APlayerStart>() || Actor->IsA<APawn>())
        {
            Facts.Unsupported.Add(Label(Actor));
        }

        TInlineComponentArray<UConvaiObjectComponent*> Objects(Actor);
        for (const UConvaiObjectComponent* Object : Objects)
        {
            // Not HasValidObjectName: the runtime normalises the name before its own empty check, so a
            // whitespace-only name passes there and is refused at runtime.
            if (Object && Object->bConvaiEnabled &&
                !FConvaiObjectEntry::NormalizeMovementPointName(Object->ObjectEntry.Name).IsEmpty())
            {
                ++Facts.ConvaiObjects;
                break;
            }
        }
    }

    void FindAvatarReferencers(
        const TArray<AActor*>& Placed, AActor* LevelScript, const TSet<const AActor*>& Avatars, FConvaiAvatarDioramaFacts& Facts)
    {
        if (Avatars.IsEmpty())
        {
            return;
        }

        // Hard references only - a soft or weak one is invisible to the finder, which is the documented
        // V0 limitation rather than a gap here.
        TArray<AActor*> Candidates = Placed;
        if (LevelScript)
        {
            Candidates.Add(LevelScript);
        }

        for (AActor* Actor : Candidates)
        {
            // A finder per candidate: its dedup set is private and outlives any reset of the out-array,
            // so a shared finder reports the avatar to the first referencer and to nobody after it.
            // ponytail: one finder pass per actor and per component, linear in the diorama; fine for tens
            // of actors, batch it if a diorama of thousands ever shows up.
            TArray<UObject*> Found;
            FReferenceFinder Finder(Found, nullptr, false, false, false, false);
            Finder.FindReferences(Actor);

            TInlineComponentArray<UActorComponent*> Components(Actor);
            for (UActorComponent* Component : Components)
            {
                if (Component)
                {
                    Finder.FindReferences(Component);
                }
            }

            for (const UObject* Object : Found)
            {
                if (Avatars.Contains(Cast<AActor>(Object)))
                {
                    Facts.AvatarReferencers.Add(Handle(Actor));
                    break;
                }
            }
        }
    }

    bool HasAuthoredLevelBlueprint(ULevel* Level)
    {
        // bDontCreate: the default builds the blueprint and dirties the level, and a scan that dirtied
        // the level would make its own bDirty fact true.
        ULevelScriptBlueprint* Blueprint = Level->GetLevelScriptBlueprint(/*bDontCreate=*/true);
        if (!Blueprint)
        {
            return false;
        }

        TArray<TObjectPtr<UEdGraph>> Graphs = Blueprint->UbergraphPages;
        Graphs.Append(Blueprint->FunctionGraphs);

        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                // An untouched level blueprint still holds the events the editor placed in it, so what
                // counts is a node the creator put there.
                if (Node && !Node->IsAutomaticallyPlacedGhostNode())
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool HasUnsavedChanges(ULevel* Level, UPackage* WorldPackage)
    {
        // Only this level's own packages: another map left dirty in the editor is not this diorama's
        // business, and an OFPA level dirties its external actor packages rather than the map.
        TSet<UPackage*> Own;
        Own.Add(WorldPackage);
        if (Level->MapBuildData)
        {
            Own.Add(Level->MapBuildData->GetOutermost());
        }
        Own.Append(Level->GetLoadedExternalObjectPackages());

        TArray<UPackage*> Dirty;
        FEditorFileUtils::GetDirtyWorldPackages(Dirty);
        for (UPackage* Package : Dirty)
        {
            if (Own.Contains(Package))
            {
                return true;
            }
        }
        return false;
    }

    void MeasureGeometry(FCollected& Collected, FConvaiAvatarDioramaFacts& Facts)
    {
        // GetNumTriangles finishes the mesh's compilation itself, one mesh at a time; finishing the set
        // first makes that one wait. Never FinishAllCompilation - this runs on a tick.
        FStaticMeshCompilingManager::Get().FinishCompilation(Collected.Meshes);

        for (const TPair<UStaticMesh*, int32>& Use : Collected.MeshUses)
        {
            Facts.Triangles += Use.Key->GetNumTriangles(0) * Use.Value;
        }

        for (const USkeletalMesh* Mesh : Collected.SkeletalUses)
        {
            if (const FSkeletalMeshRenderData* Data = Mesh->GetResourceForRendering())
            {
                if (Data->LODRenderData.IsValidIndex(0))
                {
                    Facts.Triangles += Data->LODRenderData[0].GetTotalFaces();
                }
            }
        }
    }

    void MeasureTextures(const FCollected& Collected, FConvaiAvatarDioramaFacts& Facts)
    {
        TArray<UTexture*> Textures = Collected.Textures.Array();
        // A size or format read off a still-compiling texture is the placeholder's, not the texture's.
        // Only the diorama's are waited on: the avatar's are never measured, so compiling them would be
        // a stall bought for a number nothing reads.
        FTextureCompilingManager::Get().FinishCompilation(Textures);

        // Shared textures count once, which is what the Pak carries.
        Facts.TexturesCount = Collected.Textures.Union(Collected.AvatarTextures).Num();

        // The size the cook produces, not the one the artist authored: a 4K source under a 1024 maximum
        // ships as a 1024, and that is the number the limit is about.
        ITargetPlatformManagerModule* Platforms = GetTargetPlatformManager();
        const ITargetPlatform* Windows = Platforms ? Platforms->FindTargetPlatform(TEXT("Windows")) : nullptr;

        SIZE_T Bytes = 0;
        for (UTexture* Texture : Textures)
        {
            int32 SizeX = 0;
            int32 SizeY = 0;
            if (Windows)
            {
                Texture->GetBuiltTextureSize(Windows, SizeX, SizeY);
            }

            UTexture2D* Flat = Cast<UTexture2D>(Texture);
            // Zeros are how an unknown built size shows up, the call having nothing to return.
            if (Flat && (SizeX == 0 || SizeY == 0))
            {
                SizeX = Flat->GetSizeX();
                SizeY = Flat->GetSizeY();
            }

            const int32 Edge = FMath::Max(SizeX, SizeY);
            if (Edge > Facts.TexturesMaxBuiltPx)
            {
                Facts.TexturesMaxBuiltPx = Edge;
                Facts.LargestTexture = FSoftObjectPath(Texture);
                const FString* Context = Collected.Context.Find(Texture);
                Facts.LargestTextureContext = Context ? *Context : FString();
            }

            if (Flat)
            {
                Bytes += CalcTextureSize(SizeX, SizeY, Flat->GetPixelFormat(), Flat->GetNumMips());
            }
        }

        Facts.TexturesMemoryMb = static_cast<double>(Bytes) / (1024.0 * 1024.0);
    }
}

FConvaiAvatarDioramaFacts Inspect(UWorld* World, UClass* AvatarClass)
{
    FConvaiAvatarDioramaFacts Facts;
    if (!World || !World->PersistentLevel)
    {
        return Facts;
    }

    ULevel* Level = World->PersistentLevel;
    UPackage* WorldPackage = World->GetOutermost();

    Facts.SourceLevel = WorldPackage->GetName();
    Facts.bPartitioned = World->IsPartitionedWorld();
    Facts.Sublevels = World->GetStreamingLevels().Num();
    Facts.bDirty = HasUnsavedChanges(Level, WorldPackage);

    if (AvatarClass)
    {
        // The creator named the blueprint, not the class the compiler generated from it.
        FString Name = AvatarClass->GetName();
        Name.RemoveFromEnd(TEXT("_C"));
        Facts.AvatarName = MoveTemp(Name);
    }

    TSet<const AActor*> Avatars;
    TArray<AActor*> AvatarActors;
    for (AActor* Actor : Level->Actors)
    {
        if (IsValid(Actor) && !Actor->HasAnyFlags(RF_Transient) && AvatarClass && Actor->GetClass()->IsChildOf(AvatarClass))
        {
            Avatars.Add(Actor);
            AvatarActors.Add(Actor);
            Facts.AvatarActorNames.Add(Actor->GetFName());
        }
    }

    // One placed avatar is the only case with a spawn point to record; how many there should be is
    // Evaluate's to say.
    if (AvatarActors.Num() == 1)
    {
        const FTransform& Transform = AvatarActors[0]->GetActorTransform();
        Facts.AvatarLocation = Transform.GetLocation();
        Facts.AvatarRotation = Transform.Rotator();
        Facts.AvatarScale = Transform.GetScale3D();
    }

    FCollected Collected;
    FCameraPick Cameras;
    TArray<AActor*> Placed;
    const ABrush* DefaultBrush = Level->GetDefaultBrush();

    for (AActor* Actor : Level->Actors)
    {
        // The level script actor is read for references below and counted as an actor nowhere.
        if (!IsValid(Actor) || Avatars.Contains(Actor) || Actor->IsEditorOnly() || Actor == DefaultBrush ||
            (Actor->HasAnyFlags(RF_Transient) && !Actor->IsChildActor()) ||
            Actor->IsA<AWorldSettings>() || Actor->IsA<ALevelScriptActor>() ||
            ChainReachesAvatar(Actor, Avatars, /*bAttachment=*/false))
        {
            continue;
        }

        // A child actor was spawned by its parent's component rather than placed, so it is none of the
        // diorama's actors - but it renders, so its components are still the diorama's cost.
        if (!Actor->IsChildActor())
        {
            ++Facts.Actors;
            Placed.Add(Actor);
            CollectPlacedActor(Actor, Facts, Cameras);

            // Attached and still counted: it is placed in the level, and it is only its place in the
            // Outliner that the copy cannot keep.
            if (ChainReachesAvatar(Actor, Avatars, /*bAttachment=*/true))
            {
                Facts.AttachedToAvatar.Add(Handle(Actor));
            }
        }

        CollectComponents(Actor, Facts, Collected);
    }

    if (Cameras.Actors.Num() == 1)
    {
        Facts.CameraActorName = Cameras.Actors[0]->GetFName();
    }
    else if (Cameras.Tagged)
    {
        Facts.CameraActorName = Cameras.Tagged->GetFName();
    }

    // bChecked defaults to true and asserts; nothing a scan reads is worth a crash.
    if (const AWorldSettings* Settings = World->GetWorldSettings(/*bCheckStreamingPersistent=*/false, /*bChecked=*/false))
    {
        if (Settings->DefaultGameMode)
        {
            Facts.Unsupported.Add(FString::Printf(TEXT("Game mode %s"), *Settings->DefaultGameMode->GetName()));
        }
    }

    Facts.bHasLevelBlueprint = HasAuthoredLevelBlueprint(Level);
    FindAvatarReferencers(Placed, Level->GetLevelScriptActor(), Avatars, Facts);

    // The avatar's textures are the one fact it counts in: the studio spawns its own copy, but the Pak
    // still carries them.
    for (AActor* Avatar : AvatarActors)
    {
        TInlineComponentArray<UMeshComponent*> Meshes(Avatar);
        for (const UMeshComponent* Mesh : Meshes)
        {
            if (Mesh && !Mesh->IsEditorOnly() && !Mesh->IsVisualizationComponent())
            {
                CollectTextures(Avatar, Mesh, Collected, /*bAvatar=*/true);
            }
        }
    }

    MeasureGeometry(Collected, Facts);
    MeasureTextures(Collected, Facts);

    return Facts;
}

TArray<FConvaiAvatarDioramaIssue> Evaluate(const FConvaiAvatarDioramaFacts& Facts, const FConvaiAvatarDioramaLimits& Limits)
{
    TArray<FConvaiAvatarDioramaIssue> Issues;
    const FString Level = FPackageName::GetShortName(Facts.SourceLevel);

    // The placed avatar is the transform the studio spawns at, so exactly one is the whole question -
    // and the Info line is how the creator learns that the spot they see is the spot that ships.
    if (Facts.AvatarActorNames.Num() == 0)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Error,
            FString::Printf(TEXT("Avatar not placed: %s is not in %s"), *Facts.AvatarName, *Level)));
    }
    else if (Facts.AvatarActorNames.Num() > 1)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Error,
            FString::Printf(TEXT("More than one %s placed (%d) - keep one"),
                *Facts.AvatarName, Facts.AvatarActorNames.Num())));
    }
    else
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Info,
            FString::Printf(TEXT("%s found at (%.0f, %.0f, %.0f) - Avatar Studio will spawn it here."),
                *Facts.AvatarName, Facts.AvatarLocation.X, Facts.AvatarLocation.Y, Facts.AvatarLocation.Z)));
    }

    if (Facts.Cameras.Num() > 1 && Facts.CameraActorName.IsNone())
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Error,
            FString::Printf(TEXT("Which camera? %s - tag one Convai.Diorama.Camera"), *Labels(Facts.Cameras))));
    }

    // AutoActivateForPlayer is private and hard-cuts in BeginPlay, so the copy cannot clear it for
    // the creator: the level itself has to change.
    for (const FConvaiAvatarDioramaActor& Camera : Facts.AutoActivatingCameras)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Error,
            FString::Printf(TEXT("%s auto-activates for the player - set it to Disabled"), *Camera.Label), &Camera));
    }

    for (const FConvaiAvatarDioramaActor& Camera : Facts.AspectConstrainedCameras)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Warning,
            FString::Printf(TEXT("%s constrains aspect ratio - untick Constrain Aspect Ratio"), *Camera.Label), &Camera));
    }

    for (const FConvaiAvatarDioramaActor& Referencer : Facts.AvatarReferencers)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Error,
            FString::Printf(TEXT("%s references the placed avatar - reference it through Convai actions/objects instead"),
                *Referencer.Label), &Referencer));
    }

    if (!Facts.bNavMeshBounds)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Error,
            TEXT("No NavMesh: add a NavMeshBoundsVolume over the diorama floor - Move To and Follow need one")));
    }

    if (Facts.bDirty)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Warning,
            FString::Printf(TEXT("%s has unsaved changes - the upload copies what is on disk"), *Level)));
    }

    if (Facts.Sublevels > 0)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Error,
            FString::Printf(TEXT("%s has %d sublevel%s - Avatar Studio streams only the persistent level; ")
                TEXT("move their actors into it (Levels panel -> Move Selected Actors to Level) and remove them"),
                *Level, Facts.Sublevels, Facts.Sublevels == 1 ? TEXT("") : TEXT("s"))));
    }

    for (const FConvaiAvatarDioramaActor& Light : Facts.StaticLights)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Warning,
            FString::Printf(TEXT("Static light %s will not render in Avatar Studio - set Mobility to Movable"),
                *Light.Label), &Light));
    }

    for (const FConvaiAvatarDioramaActor& Attached : Facts.AttachedToAvatar)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Warning,
            FString::Printf(TEXT("%s is attached to the avatar and will be left in the diorama at its world position ")
                TEXT("- attach it inside the avatar blueprint instead"), *Attached.Label), &Attached));
    }

    for (const FString& Entry : Facts.Unsupported)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Info,
            FString::Printf(TEXT("%s will not be used by Avatar Studio"), *Entry)));
    }

    if (Facts.bHasLevelBlueprint)
    {
        Issues.Add(Issue(EConvaiAvatarDioramaSeverity::Info,
            FString::Printf(TEXT("%s has a Level Blueprint; it will run in Avatar Studio"), *Level)));
    }

    for (const EConvaiAvatarDioramaLimit Limit : TEnumRange<EConvaiAvatarDioramaLimit>())
    {
        const FConvaiAvatarDioramaLimitRule* Rule = Limits.Rules.Find(Limit);
        if (!Rule)
        {
            continue;
        }

        const double Measured = Measure(Facts, Limit);
        if (Measured > Rule->Max)
        {
            Issues.Add(LimitIssue(Facts, Limit, *Rule, Measured));
        }
    }

    return Issues;
}

namespace
{
TSharedRef<FJsonObject> DioramaRecordObject(int32 Schema, const FString& LevelPackage, const FVector& Location,
    const FRotator& Rotation, const FVector& Scale, bool bHasCustomLighting, bool bHasCustomCamera)
{
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schema"), Schema);
    Root->SetStringField(TEXT("level"), LevelPackage);

    const TSharedPtr<FJsonObject> Transform = MakeShared<FJsonObject>();
    Transform->SetObjectField(TEXT("location"),
        Triple(TEXT("x"), Location.X, TEXT("y"), Location.Y, TEXT("z"), Location.Z));
    Transform->SetObjectField(TEXT("rotation"),
        Triple(TEXT("pitch"), Rotation.Pitch, TEXT("yaw"), Rotation.Yaw, TEXT("roll"), Rotation.Roll));
    Transform->SetObjectField(TEXT("scale"),
        Triple(TEXT("x"), Scale.X, TEXT("y"), Scale.Y, TEXT("z"), Scale.Z));
    Root->SetObjectField(TEXT("avatar_transform"), Transform);

    Root->SetBoolField(TEXT("has_custom_lighting"), bHasCustomLighting);
    Root->SetBoolField(TEXT("has_custom_camera"), bHasCustomCamera);

    return Root;
}
}

TSharedRef<FJsonObject> MakeRecord(const FConvaiAvatarDioramaFacts& Facts, const FString& LevelPackage)
{
    return DioramaRecordObject(1, LevelPackage, Facts.AvatarLocation, FRotator(0.0, Facts.AvatarRotation.Yaw, 0.0),
        Facts.AvatarScale, Facts.HasCustomLighting(), Facts.HasCustomCamera());
}

TSharedRef<FJsonObject> MakeRecord(const FConvaiAvatarDioramaRecord& Record)
{
    return DioramaRecordObject(Record.Schema, Record.Level, Record.AvatarTransform.GetLocation(), Record.AvatarTransform.Rotator(),
        Record.AvatarTransform.GetScale3D(), Record.bHasCustomLighting, Record.bHasCustomCamera);
}

void ApplyRecordTo(TSharedRef<FJsonObject> Metadata, const TOptional<FConvaiAvatarDioramaRecord>& Record)
{
    if (Record.IsSet()) Metadata->SetObjectField(TEXT("stage"), MakeRecord(Record.GetValue()));
    else Metadata->RemoveField(TEXT("stage"));
}

namespace
{
    bool ReadRecordNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double& Value)
    {
        const auto Field = Object->TryGetField(Key);
        return Field && Field->Type == EJson::Number && Object->TryGetNumberField(Key, Value) && FMath::IsFinite(Value);
    }

    bool ReadRecordTriple(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key,
        const TCHAR* KeyA, double& A, const TCHAR* KeyB, double& B, const TCHAR* KeyC, double& C)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        return Parent->TryGetObjectField(Key, Object) &&
            ReadRecordNumber(*Object, KeyA, A) && ReadRecordNumber(*Object, KeyB, B) && ReadRecordNumber(*Object, KeyC, C);
    }
}

bool ReadRecord(const TSharedPtr<FJsonObject>& Json, FConvaiAvatarDioramaRecord& OutRecord)
{
    if (!Json) return false;
    FConvaiAvatarDioramaRecord Parsed;
    double Schema = 0.0;
    const TSharedPtr<FJsonObject>* Transform = nullptr;
    const auto Level = Json->TryGetField(TEXT("level"));
    const auto Lighting = Json->TryGetField(TEXT("has_custom_lighting"));
    const auto Camera = Json->TryGetField(TEXT("has_custom_camera"));
    if (!ReadRecordNumber(Json, TEXT("schema"), Schema) || Schema != 1.0 ||
        !Level || Level->Type != EJson::String || !Json->TryGetStringField(TEXT("level"), Parsed.Level) ||
        !FPackageName::IsValidTextForLongPackageName(Parsed.Level) ||
        !Lighting || Lighting->Type != EJson::Boolean || !Json->TryGetBoolField(TEXT("has_custom_lighting"), Parsed.bHasCustomLighting) ||
        !Camera || Camera->Type != EJson::Boolean || !Json->TryGetBoolField(TEXT("has_custom_camera"), Parsed.bHasCustomCamera) ||
        !Json->TryGetObjectField(TEXT("avatar_transform"), Transform)) return false;

    FVector Location, Scale;
    FRotator Rotation;
    if (!ReadRecordTriple(*Transform, TEXT("location"), TEXT("x"), Location.X, TEXT("y"), Location.Y, TEXT("z"), Location.Z) ||
        !ReadRecordTriple(*Transform, TEXT("rotation"), TEXT("pitch"), Rotation.Pitch, TEXT("yaw"), Rotation.Yaw, TEXT("roll"), Rotation.Roll) ||
        !ReadRecordTriple(*Transform, TEXT("scale"), TEXT("x"), Scale.X, TEXT("y"), Scale.Y, TEXT("z"), Scale.Z)) return false;
    Parsed.AvatarTransform = FTransform(Rotation, Location, Scale);
    OutRecord = MoveTemp(Parsed);
    return true;
}

bool HasErrors(const FConvaiAvatarDioramaReport& Report)
{
    return Report.bLimitsRead && Report.Count(EConvaiAvatarDioramaSeverity::Error) > 0;
}

FText StatusLine(const FConvaiAvatarDioramaReport& Report)
{
    if (!Report.Refusal.IsEmpty()) return FText::FromString(Report.Refusal);
    if (!Report.bLimitsRead) return LOCTEXT("DioramaLimitsNotRead", "Limits not read - Re-read policy");
    const int32 Errors = Report.Count(EConvaiAvatarDioramaSeverity::Error);
    if (Errors > 0)
    {
        return Errors == 1
            ? LOCTEXT("DioramaOneIssue", "1 issue needs attention.")
            : FText::Format(LOCTEXT("DioramaIssues", "{0} issues need attention."), Errors);
    }
    const int32 Warnings = Report.Count(EConvaiAvatarDioramaSeverity::Warning);
    if (Warnings == 1) return LOCTEXT("DioramaReadyOneWarning", "READY TO UPLOAD (1 warning)");
    if (Warnings > 1) return FText::Format(LOCTEXT("DioramaReadyWarnings", "READY TO UPLOAD ({0} warnings)"), Warnings);
    return LOCTEXT("DioramaReady", "READY TO UPLOAD");
}
}

#undef LOCTEXT_NAMESPACE
