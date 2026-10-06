// Copyright Convai. All Rights Reserved.

#include "SceneAutoTaggerController.h"

#include "ConvaiSceneAutoTaggerSettings.h"
#include "ConvaiObjectComponent.h"
#include "Convai/Convai.h"
#include "ConvaiUtils.h"
#include "ConvaiVisionService.h"
#include "SceneAutoTaggerApply.h"
#include "SceneAutoTaggerCache.h"
#include "SceneAutoTaggerCapture.h"
#include "SceneAutoTaggerCaptureWarmup.h"
#include "SceneAutoTaggerDuplicateClustering.h"
#include "SceneAutoTaggerDiscovery.h"
#include "SceneAutoTaggerNativeCoreAdapter.h"
#include "SceneAutoTaggerViewSelection.h"
#include "SceneAutoTaggerVisionProtocol.h"

#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "ContentStreaming.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "Engine/StreamableRenderAsset.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineGlobals.h"
#include "UnrealEngine.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "ImageCore.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "ConvaiSceneAutoTaggerController"

DEFINE_LOG_CATEGORY_STATIC(LogConvaiSceneAutoTaggerController, Log, All);

namespace
{
using namespace ConvaiSceneAutoTagger::CaptureWarmup;

// A zero-delay ticker added from inside another ticker can run again in the same
// FTSTicker::Tick call. Keep this positive so every probe/object yields back to Slate.
constexpr float CaptureYieldDelaySeconds = 0.001f;
constexpr const TCHAR* NativeCoreUnavailableMessage =
	TEXT("Scene analysis isn't available in this Convai installation. Reinstall or update the Convai plugin, then restart Unreal Editor.");

void AddCaptureStreamingAsset(
	TArray<FCaptureStreamingLease>& Assets,
	UStreamableRenderAsset* Asset)
{
	if (!IsValid(Asset))
	{
		return;
	}
	for (const FCaptureStreamingLease& Lease : Assets)
	{
		if (Lease.Asset.Get() == Asset)
		{
			return;
		}
	}

	FCaptureStreamingLease& Lease = Assets.AddDefaulted_GetRef();
	Lease.Asset = Asset;
	Lease.bWasForcedResident = Asset->ShouldMipLevelsBeForcedResident();
	Lease.bOriginalIgnoreStreamingMipBias = Asset->bIgnoreStreamingMipBias;
}

void ReleaseCaptureStreaming(
	TArray<FCaptureStreamingLease>& Assets)
{
	for (const FCaptureStreamingLease& Lease : Assets)
	{
		if (UStreamableRenderAsset* Asset = Lease.Asset.Get())
		{
			// Requests are intentionally short-lived, but a fast batch can otherwise
			// keep several recently captured actors resident at once. Expire this
			// actor's request as soon as its retained pixels have been read. Preserve
			// any force-resident state that existed before the Auto Tagger touched it.
			if (!Lease.bWasForcedResident)
			{
				Asset->SetForceMipLevelsToBeResident(-1.0f);
			}
			Asset->bIgnoreStreamingMipBias = Lease.bOriginalIgnoreStreamingMipBias;
		}
	}
	Assets.Reset();
}

void RequestCaptureStreaming(
	const TArray<UPrimitiveComponent*>& Components,
	const float DurationSeconds,
	const bool bFastTextureResponse,
	TArray<FCaptureStreamingLease>& OutAssets,
	TArray<TWeakObjectPtr<UMaterialInterface>>& OutMaterials)
{
	for (UPrimitiveComponent* Component : Components)
	{
		if (!IsValid(Component))
		{
			continue;
		}

		if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
		{
			AddCaptureStreamingAsset(OutAssets, StaticMeshComponent->GetStaticMesh());
		}
		else if (USkinnedMeshComponent* SkinnedMeshComponent = Cast<USkinnedMeshComponent>(Component))
		{
			AddCaptureStreamingAsset(OutAssets, SkinnedMeshComponent->GetSkinnedAsset());
		}
		if (UMeshComponent* MeshComponent = Cast<UMeshComponent>(Component))
		{
			MeshComponent->PrestreamMeshLODs(DurationSeconds);
		}
		for (int32 MaterialIndex = 0; MaterialIndex < Component->GetNumMaterials(); ++MaterialIndex)
		{
			if (UMaterialInterface* Material = Component->GetMaterial(MaterialIndex))
			{
				OutMaterials.AddUnique(TWeakObjectPtr<UMaterialInterface>(Material));
			}
		}

		TArray<UTexture*> UsedTextures;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 8
		check(GetCachedScalabilityCVars().bInitialized);
		Component->GetUsedTextures(UsedTextures, GetCachedScalabilityCVars().MaterialQualityLevel);
#else
		Component->GetUsedTextures(UsedTextures, GetCurrentMaterialQualityLevelChecked());
#endif
		for (UTexture* Texture : UsedTextures)
		{
			if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
			{
				AddCaptureStreamingAsset(OutAssets, Texture2D);
				// Call the render asset directly. The actor/material convenience APIs
				// synchronously finish texture compilation in Editor builds, which can
				// freeze Slate before their otherwise-asynchronous residency request.
				Texture2D->SetForceMipLevelsToBeResident(DurationSeconds, 0);
			}
		}
	}

	// Force-resident flags are consumed asynchronously by the render-asset
	// streaming manager. Prioritize only this actor's conventional assets now so
	// the readiness gate below observes the new request instead of the previous
	// low-mip target. This is the non-blocking, per-asset counterpart to the
	// global StreamAllResources/WaitForStreaming calls that would freeze Slate.
	auto& RenderAssetStreamingManager =
		IStreamingManager::Get().GetRenderAssetStreamingManager();
	for (const FCaptureStreamingLease& Lease : OutAssets)
	{
		UStreamableRenderAsset* Asset = Lease.Asset.Get();
		if (!Asset || !Asset->IsStreamable())
		{
			continue;
		}

		const FStreamableRenderResourceState& State = Asset->GetStreamableResourceState();
		bool bFastRequestAccepted = false;
		if (bFastTextureResponse)
		{
			if (UTexture2D* Texture2D = Cast<UTexture2D>(Asset))
			{
				static IConsoleVariable* CVarAllowFastForceResident =
					IConsoleManager::Get().FindConsoleVariable(
						TEXT("r.Streaming.AllowFastForceResident"));
				Texture2D->bIgnoreStreamingMipBias = CVarAllowFastForceResident
					&& CVarAllowFastForceResident->GetInt() != 0;
				bFastRequestAccepted =
					RenderAssetStreamingManager.FastForceFullyResident(Texture2D);
			}
		}
		if (!bFastRequestAccepted
			&& (!State.IsValid() || !State.bSupportsVirtualStreaming))
		{
			RenderAssetStreamingManager.UpdateIndividualRenderAsset(Asset);
		}
	}
}

FCaptureStreamingReadiness InspectCaptureStreamingReadiness(
	const TArray<FCaptureStreamingLease>& Assets,
	const TArray<TWeakObjectPtr<UMaterialInterface>>& Materials)
{
	FCaptureStreamingReadiness Result;
	auto& RenderAssetStreamingManager =
		IStreamingManager::Get().GetRenderAssetStreamingManager();
	for (const FCaptureStreamingLease& Lease : Assets)
	{
		UStreamableRenderAsset* Asset = Lease.Asset.Get();
		if (!Asset)
		{
			++Result.UnknownResources;
			continue;
		}

		++Result.TrackedAssets;
		const FStreamableRenderResourceState& State = Asset->GetStreamableResourceState();
		if (State.IsValid() && State.bSupportsVirtualStreaming)
		{
			// Virtual textures and Nanite have no per-view completion callback. The
			// bounded real-frame allowance below is their readiness contract.
			++Result.VirtualAssets;
		}

		const bool bHasPendingUpdate = Asset->HasPendingInitOrStreaming();
		bool bFullyResident = true;
		bool bPending = bHasPendingUpdate;
		if (Asset->IsStreamable()
			&& State.IsValid()
			&& State.bSupportsStreaming
			&& !State.bSupportsVirtualStreaming)
		{
			++Result.ConventionalAssets;
			bFullyResident = RenderAssetStreamingManager.IsFullyStreamedIn(Asset);
			bPending = !ConvaiSceneAutoTagger::IsConventionalCaptureAssetReady(
				bHasPendingUpdate,
				bFullyResident);
			Result.FullyResidentAssets += bPending ? 0 : 1;
		}
		else if (Asset->IsStreamable() && !State.IsValid())
		{
			// A streamable asset without a published state cannot yet prove readiness.
			++Result.ConventionalAssets;
			bFullyResident = false;
			bPending = true;
		}
		Result.PendingAssets += bPending ? 1 : 0;

		UE_LOG(
			LogConvaiSceneAutoTaggerController,
			VeryVerbose,
			TEXT("Capture residency '%s' (%s): resident=%d requested=%d max=%d nonOptional=%d pendingUpdate=%s fullyResident=%s virtual=%s."),
			*Asset->GetName(),
			*Asset->GetClass()->GetName(),
			State.IsValid() ? int32(State.NumResidentLODs) : -1,
			State.IsValid() ? int32(State.NumRequestedLODs) : -1,
			State.IsValid() ? int32(State.MaxNumLODs) : -1,
			State.IsValid() ? int32(State.NumNonOptionalLODs) : -1,
			bHasPendingUpdate ? TEXT("yes") : TEXT("no"),
			bFullyResident ? TEXT("yes") : TEXT("no"),
			State.IsValid() && State.bSupportsVirtualStreaming ? TEXT("yes") : TEXT("no"));
	}
	for (const TWeakObjectPtr<UMaterialInterface>& WeakMaterial : Materials)
	{
		if (UMaterialInterface* Material = WeakMaterial.Get())
		{
			++Result.TrackedMaterials;
			Result.CompilingMaterials += Material->IsCompiling() ? 1 : 0;
		}
		else
		{
			++Result.UnknownResources;
		}
	}
	return Result;
}

void AppendWarning(FString& Destination, const FString& Warning)
{
	if (Warning.IsEmpty())
	{
		return;
	}
	const FString FramedDestination = TEXT("\n") + Destination + TEXT("\n");
	const FString FramedWarning = TEXT("\n") + Warning + TEXT("\n");
	if (FramedDestination.Contains(FramedWarning, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (!Destination.IsEmpty())
	{
		Destination += TEXT("\n");
	}
	Destination += Warning;
}

FString Trimmed(FString Value, int32 MaximumLength)
{
	Value.TrimStartAndEndInline();
	return Value.Left(MaximumLength);
}

FString BuildBoundedCollisionName(const FString& BaseName, const int32 Suffix)
{
	TArray<FString> Words;
	BaseName.ParseIntoArrayWS(Words);
	while (Words.Num() > 2)
	{
		// Preserve the semantic head of a three-word name, e.g. "marble bust 2".
		Words.RemoveAt(0);
	}
	FString Stem = FString::Join(Words, TEXT(" "));
	const FString SuffixText = FString::Printf(TEXT(" %d"), Suffix);
	Stem = Stem.Left(FMath::Max(1, 64 - SuffixText.Len())).TrimEnd();
	return Stem + SuffixText;
}

TArray<UPrimitiveComponent*> ResolvePrimitiveComponents(const FSceneAutoTaggerCandidate& Candidate)
{
	TArray<UPrimitiveComponent*> Components;
	Components.Reserve(Candidate.PrimitiveComponents.Num());
	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakComponent : Candidate.PrimitiveComponents)
	{
		if (UPrimitiveComponent* Component = WeakComponent.Get())
		{
			Components.Add(Component);
		}
	}
	return Components;
}

FVector ResolveDuplicateClusteringLocation(
	const FSceneAutoTaggerCandidate& Candidate,
	bool& bOutValid)
{
	bOutValid = false;
	AActor* Actor = Candidate.Actor.Get();
	if (!IsValid(Actor))
	{
		return FVector::ZeroVector;
	}

	const auto IsFiniteVector = [](const FVector& Value)
	{
		return FMath::IsFinite(Value.X)
			&& FMath::IsFinite(Value.Y)
			&& FMath::IsFinite(Value.Z);
	};
	FBox CurrentBounds(ForceInit);
	int32 CurrentComponentCount = 0;
	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakComponent : Candidate.PrimitiveComponents)
	{
		const UPrimitiveComponent* Component = WeakComponent.Get();
		if (!IsValid(Component) || !Component->IsRegistered()
			|| Component->GetWorld() != Actor->GetWorld())
		{
			continue;
		}
		const FBox ComponentBounds = Component->Bounds.GetBox();
		if (!ComponentBounds.IsValid || !IsFiniteVector(ComponentBounds.Min)
			|| !IsFiniteVector(ComponentBounds.Max))
		{
			CurrentBounds = FBox(ForceInit);
			CurrentComponentCount = 0;
			break;
		}
		CurrentBounds += ComponentBounds;
		++CurrentComponentCount;
	}

	const FBox& Bounds = CurrentComponentCount > 0 && CurrentBounds.IsValid
		? CurrentBounds
		: Candidate.WorldBounds;
	if (Bounds.IsValid && IsFiniteVector(Bounds.Min) && IsFiniteVector(Bounds.Max))
	{
		const FVector Center = Bounds.GetCenter();
		if (IsFiniteVector(Center))
		{
			bOutValid = true;
			return Center;
		}
	}

	const FVector ActorLocation = Actor->GetActorLocation();
	bOutValid = IsFiniteVector(ActorLocation);
	return bOutValid ? ActorLocation : FVector::ZeroVector;
}

struct FContextBatchSelection
{
	TArray<int32> TargetIndices;
	TArray<int32> ContextIndices;
};

bool BuildContextBatchPlan(
	const TArray<TSharedPtr<FSceneAutoTaggerCandidate>>& Candidates,
	const TArray<int32>& TargetIndices,
	const TArray<int32>& ContextIndices,
	const int32 CellCapacity,
	const bool bFillUnusedCells,
	TArray<FContextBatchSelection>& OutBatches,
	FString& OutError)
{
	OutBatches.Reset();
	OutError.Reset();
	if (TargetIndices.IsEmpty() || CellCapacity <= 0)
	{
		OutError = TEXT("spatial batch planning received an invalid target set");
		return false;
	}

	for (int32 Start = 0; Start < TargetIndices.Num(); Start += CellCapacity)
	{
		FContextBatchSelection& Batch = OutBatches.AddDefaulted_GetRef();
		const int32 End = FMath::Min(Start + CellCapacity, TargetIndices.Num());
		for (int32 Index = Start; Index < End; ++Index)
		{
			Batch.TargetIndices.Add(TargetIndices[Index]);
		}
		if (!bFillUnusedCells || Batch.TargetIndices.Num() >= CellCapacity)
		{
			continue;
		}

		FVector TargetCenter = FVector::ZeroVector;
		int32 ValidTargetCenters = 0;
		for (const int32 CandidateIndex : Batch.TargetIndices)
		{
			if (!Candidates.IsValidIndex(CandidateIndex) || !Candidates[CandidateIndex].IsValid())
			{
				continue;
			}
			bool bValid = false;
			const FVector Location = ResolveDuplicateClusteringLocation(
				*Candidates[CandidateIndex], bValid);
			if (bValid)
			{
				TargetCenter += Location;
				++ValidTargetCenters;
			}
		}
		if (ValidTargetCenters > 0)
		{
			TargetCenter /= static_cast<double>(ValidTargetCenters);
		}

		TArray<int32> RankedContext = ContextIndices;
		RankedContext.StableSort([&Candidates, TargetCenter, ValidTargetCenters](
			const int32 LeftIndex,
			const int32 RightIndex)
		{
			bool bLeftValid = false;
			bool bRightValid = false;
			const FVector Left = Candidates.IsValidIndex(LeftIndex) && Candidates[LeftIndex].IsValid()
				? ResolveDuplicateClusteringLocation(*Candidates[LeftIndex], bLeftValid)
				: FVector::ZeroVector;
			const FVector Right = Candidates.IsValidIndex(RightIndex) && Candidates[RightIndex].IsValid()
				? ResolveDuplicateClusteringLocation(*Candidates[RightIndex], bRightValid)
				: FVector::ZeroVector;
			const double LeftDistance = ValidTargetCenters > 0 && bLeftValid
				? FVector::DistSquared(Left, TargetCenter)
				: TNumericLimits<double>::Max();
			const double RightDistance = ValidTargetCenters > 0 && bRightValid
				? FVector::DistSquared(Right, TargetCenter)
				: TNumericLimits<double>::Max();
			return LeftDistance == RightDistance
				? LeftIndex < RightIndex
				: LeftDistance < RightDistance;
		});

		const int32 ContextCapacity = CellCapacity - Batch.TargetIndices.Num();
		for (int32 Index = 0; Index < FMath::Min(ContextCapacity, RankedContext.Num()); ++Index)
		{
			Batch.ContextIndices.Add(RankedContext[Index]);
		}
	}
	return !OutBatches.IsEmpty();
}

bool HasApplicableCandidateName(const FSceneAutoTaggerCandidate& Candidate)
{
	return !FConvaiObjectEntry::NormalizeMovementPointName(
		Trimmed(Candidate.SuggestedName, 96)).IsEmpty();
}

bool IsReadyToApplyCandidate(const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate)
{
	return Candidate.IsValid()
		&& !Candidate->bDismissedFromReview
		&& Candidate->Decision == ESceneAutoTaggerDecision::Accepted
		&& Candidate->bTagComplete
		&& !Candidate->bApplied
		&& Candidate->Actor.IsValid()
		&& !Candidate->bAnalysisCancelled
		&& !Candidate->bUserCapturePendingAnalysis
		&& ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
			Candidate->RefinementNote).IsEmpty()
		&& HasApplicableCandidateName(*Candidate);
}

ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan BuildDuplicateClusteringPlanForCandidates(
	const TArray<TSharedPtr<FSceneAutoTaggerCandidate>>& Candidates,
	const UConvaiSceneAutoTaggerSettings& Settings)
{
	TArray<ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringItem> Items;
	Items.Reserve(Candidates.Num());
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (!Candidate.IsValid() || Candidate->bDismissedFromReview)
		{
			continue;
		}

		ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringItem& Item =
			Items.AddDefaulted_GetRef();
		Item.CandidateIndex = CandidateIndex;
		Item.ProofGroup = Candidate->DuplicateRepresentative != INDEX_NONE
			&& Candidate->DuplicateGroupSize > 1
			? Candidate->DuplicateRepresentative
			: INDEX_NONE;
		Item.ActorPath = Candidate->ActorPath;
		Item.Location = ResolveDuplicateClusteringLocation(*Candidate, Item.bHasValidLocation);
	}

	return ConvaiSceneAutoTagger::BuildDuplicateClusteringPlan(
		Items,
		Settings.GetEffectiveDuplicateGroupingMode(),
		Settings.DuplicateClusterMaxHorizontalSpanCm,
		Settings.DuplicateClusterMaxVerticalSeparationCm);
}

FString Sha1Prefix(const FString& Value, int32 CharacterCount)
{
	const FTCHARToUTF8 Utf8(*Value);
	return FSHA1::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()))
		.ToString().Left(CharacterCount).ToLower();
}

TArray<FColor> DownsampleSquareImage(
	const TArray<FColor>& Source,
	const int32 SourceResolution,
	const int32 TargetResolution)
{
	if (SourceResolution <= 0 || TargetResolution <= 0
		|| Source.Num() != SourceResolution * SourceResolution)
	{
		return {};
	}
	if (SourceResolution == TargetResolution)
	{
		return Source;
	}

	FImage SourceImage(
		SourceResolution,
		SourceResolution,
		ERawImageFormat::BGRA8,
		EGammaSpace::sRGB);
	if (SourceImage.RawData.Num()
		!= static_cast<int64>(Source.Num()) * static_cast<int64>(sizeof(FColor)))
	{
		return {};
	}
	FMemory::Memcpy(
		SourceImage.RawData.GetData(),
		Source.GetData(),
		SourceImage.RawData.Num());

	FImage OutputImage(
		TargetResolution,
		TargetResolution,
		ERawImageFormat::BGRA8,
		EGammaSpace::sRGB);
	FImageCore::ResizeImage(
		SourceImage,
		OutputImage,
		FImageCore::EResizeImageFilter::AdaptiveSmooth);
	TArray<FColor> Result;
	Result.SetNumUninitialized(TargetResolution * TargetResolution);
	if (OutputImage.RawData.Num()
		!= static_cast<int64>(Result.Num()) * static_cast<int64>(sizeof(FColor)))
	{
		return {};
	}
	FMemory::Memcpy(
		Result.GetData(),
		OutputImage.RawData.GetData(),
		OutputImage.RawData.Num());
	return Result;
}

// Discovery writes a human-readable reason list; the native core consumes the
// same facts as ABI v4 significance-reason bits. Phrases must stay in sync
// with SceneAutoTaggerDiscovery.
uint32 BuildNativeSignificanceReasonFlags(const FString& SignificanceReason)
{
	const auto HasReason = [&SignificanceReason](const TCHAR* Phrase)
	{
		return SignificanceReason.Contains(Phrase, ESearchCase::IgnoreCase);
	};
	uint32 Flags = 0;
	Flags |= HasReason(TEXT("object-scale geometry")) ? 1u << 0 : 0u;
	Flags |= HasReason(TEXT("large geometry")) ? 1u << 1 : 0u;
	Flags |= HasReason(TEXT("very small")) ? 1u << 2 : 0u;
	Flags |= HasReason(TEXT("multi-part")) ? 1u << 3 : 0u;
	Flags |= HasReason(TEXT("material detail")) ? 1u << 4 : 0u;
	Flags |= HasReason(TEXT("tour keyword")) ? 1u << 5 : 0u;
	Flags |= HasReason(TEXT("structural keyword")) ? 1u << 6 : 0u;
	Flags |= HasReason(TEXT("authored label")) ? 1u << 7 : 0u;
	Flags |= HasReason(TEXT("plain engine primitive")) ? 1u << 8 : 0u;
	Flags |= HasReason(TEXT("authored material on panel-like engine primitive")) ? 1u << 9 : 0u;
	// "low-information geometry" is a prefix of the authored-surface phrase, so
	// the two mutually exclusive discovery branches must be tested in order.
	if (HasReason(TEXT("low-information geometry with authored surface")))
	{
		Flags |= 1u << 10;
	}
	else if (HasReason(TEXT("low-information geometry")))
	{
		Flags |= 1u << 11;
	}
	return Flags;
}
}

struct FSceneAutoTaggerController::FActiveBatch
{
	FGuid BatchId = FGuid::NewGuid();
	/** Representatives originally assigned to this batch, including any capture failures. */
	TArray<int32> RequestedRepresentativeIndices;
	/** Nearby retained-evidence actors appended after captured targets for model context only. */
	TArray<int32> PlannedContextIndices;
	/** Representatives whose cells captured successfully, in contact-sheet order. */
	TArray<int32> CapturedRepresentativeIndices;
	/** True for cells whose returned proposal may update review state; false for context-only cells. */
	TArray<bool> CapturedTargetFlags;
	/** Preview revision paired with every captured cell to reject stale asynchronous results. */
	TArray<uint32> CapturedPreviewRevisions;
	TArray<TArray<FColor>> CapturedCells;
	TArray<FString> EchoIds;
	/** Frozen per-cell prompt data, aligned with CapturedCells across retries and recovery batches. */
	TArray<FSceneAutoTaggerObjectPromptContext> ObjectPromptContexts;
	TArray<uint32> CapturedRefinementNoteRevisions;
	bool IsTargetCell(const int32 CellIndex) const
	{
		return !CapturedTargetFlags.IsValidIndex(CellIndex) || CapturedTargetFlags[CellIndex];
	}
	int32 CountTargetCells() const
	{
		int32 Count = 0;
		for (int32 CellIndex = 0; CellIndex < CapturedRepresentativeIndices.Num(); ++CellIndex)
		{
			Count += IsTargetCell(CellIndex) ? 1 : 0;
		}
		return Count;
	}
	TArray64<uint8> GridPngBytes;
	TArray<FVector> CurrentProbeLookDirections;
	ConvaiSceneAutoTagger::FSceneAutoTaggerViewPlan CurrentProbeViewPlan;
	/** Plan-aligned probe captures; a failed capture keeps its slot absent. */
	TArray<ConvaiSceneAutoTagger::FSceneAutoTaggerProbeImage> CurrentProbeImages;
	/** Plan-aligned probe metrics returned by SelectCapturedView after the probe loop. */
	TArray<ConvaiSceneAutoTagger::FSceneAutoTaggerViewMetrics> CurrentProbeMetrics;
	TArray<FCaptureStreamingLease> CurrentStreamingAssets;
	TArray<TWeakObjectPtr<UMaterialInterface>> CurrentStreamingMaterials;
	FString CurrentProbeError;
	/** High-resolution winning view kept alive briefly so texture/VT pages can settle. */
	TUniquePtr<ConvaiSceneAutoTagger::FLiveObjectCaptureSession> CurrentFinalCaptureSession;
	int32 CaptureCursor = 0;
	int32 CurrentProbeCursor = 0;
	/** First visits every proposed direction without scoring so view-dependent pages can load. */
	bool bCurrentProbeSeedingPass = true;
	bool bCurrentProbeViewPrimed = false;
	bool bCurrentProbePrimeSucceeded = false;
	TArray<bool> CurrentProbeSeededViews;
	/** A locality hint only; each new actor must prove its own current residency. */
	FVector LastCompletedCaptureCenter = FVector::ZeroVector;
	double LastCompletedCaptureSeconds = -1.0;
	int32 CurrentProbeWarmupTicks = 0;
	uint64 CurrentProbeLastObservedFrame = MAX_uint64;
	double CurrentProbeWarmupStartedAtSeconds = 0.0;
	bool bCurrentProbeWarmupComplete = false;
	int32 SelectedProbeIndex = INDEX_NONE;
	int32 CurrentFinalWarmupTicks = 0;
	uint64 CurrentFinalLastObservedFrame = MAX_uint64;
	double CurrentFinalWarmupStartedAtSeconds = 0.0;
	bool bCurrentFinalCaptureCalibrated = false;
	int32 AlignmentRetryCount = 0;
	int32 ProviderRetryCount = 0;
	double CaptureStartedAtSeconds = 0.0;
	double ProviderStartedAtSeconds = 0.0;
	TSharedPtr<ISceneAutoTaggerRequest> Request;
};

namespace
{
FSceneAutoTaggerObjectPromptContext BuildObjectPromptContext(
	const FSceneAutoTaggerCandidate& Candidate,
	const FString& EchoId,
	const bool bIncludeRefinement = true,
	const bool bContextOnly = false,
	const bool bIncludePreviousProposal = false)
{
	FSceneAutoTaggerObjectPromptContext Context;
	Context.EchoId = EchoId;
	Context.bContextOnly = bContextOnly;
	Context.bIncludePreviousProposal = bIncludePreviousProposal
		&& (!Candidate.SuggestedName.IsEmpty() || !Candidate.Description.IsEmpty());
	if (bIncludeRefinement)
	{
		Context.RefinementNote = ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
			Candidate.RefinementNote);
	}
	if (Context.bIncludePreviousProposal || !Context.RefinementNote.IsEmpty())
	{
		Context.PreviousName = Candidate.SuggestedName;
		Context.PreviousDescription = Candidate.Description;
	}
	return Context;
}

bool ContainsRefinementGuidance(
	const TArray<FSceneAutoTaggerObjectPromptContext>& ObjectPromptContexts)
{
	for (const FSceneAutoTaggerObjectPromptContext& Context : ObjectPromptContexts)
	{
		if (!Context.RefinementNote.IsEmpty())
		{
			return true;
		}
	}
	return false;
}
}

FSceneAutoTaggerController::FSceneAutoTaggerController()
{
	if (GEditor)
	{
		GEditor->RegisterForUndo(this);
	}
}

FSceneAutoTaggerController::~FSceneAutoTaggerController()
{
	if (GEditor)
	{
		GEditor->UnregisterForUndo(this);
	}
	bCancelRequested = true;
	ActiveRunId = FGuid::NewGuid();
	for (const TPair<FGuid, TSharedPtr<FActiveBatch>>& Pair : InFlightAnalysisBatches)
	{
		if (Pair.Value.IsValid() && Pair.Value->Request.IsValid())
		{
			Pair.Value->Request->Cancel();
			Pair.Value->Request.Reset();
		}
	}
	InFlightAnalysisBatches.Reset();
	PendingAnalysisBatches.Reset();
	ConvaiSceneAutoTagger::CleanupCaptureResources();
}

void FSceneAutoTaggerController::PostUndo(const bool bSuccess)
{
	if (bSuccess)
	{
		ReconcileAppliedComponentsFromEditor();
	}
}

void FSceneAutoTaggerController::PostRedo(const bool bSuccess)
{
	if (bSuccess)
	{
		ReconcileAppliedComponentsFromEditor();
	}
}

bool FSceneAutoTaggerController::HasMatchingAppliedComponent(
	const FSceneAutoTaggerCandidate& Candidate,
	const int32 CandidateIndex) const
{
	AActor* Actor = Candidate.Actor.Get();
	if (!Candidate.bTagComplete || !IsValid(Actor))
	{
		return false;
	}

	const FString ExpectedName =
		FConvaiObjectEntry::NormalizeMovementPointName(Candidate.SuggestedName);
	const FString ExpectedDescription = Candidate.Description.TrimStartAndEnd();
	const bool bExpectedMerge = FrozenDuplicateClusteringPlan.IsValid()
		&& FrozenDuplicateClusteringPlan->ShouldMergeCandidate(CandidateIndex);
	const int32 ExpectedMergeGroupIndex = bExpectedMerge
		? FrozenDuplicateClusteringPlan->GetMergeGroupIndexForCandidate(CandidateIndex)
		: 0;
	TArray<UConvaiObjectComponent*> Components;
	Actor->GetComponents<UConvaiObjectComponent>(Components);
	for (const UConvaiObjectComponent* Component : Components)
	{
		if (IsValid(Component)
			&& FSceneAutoTaggerApply::HasAutoTaggerOwnershipMarker(*Component)
			&& Component->ObjectEntry.ObjectReference == EConvaiObjectReference::WholeActor
			&& Component->ObjectEntry.Name == ExpectedName
			&& Component->ObjectEntry.Description == ExpectedDescription
			&& Component->bMergeWithSameNamedObjects == bExpectedMerge
			&& Component->MergeGroupIndex == ExpectedMergeGroupIndex)
		{
			return true;
		}
	}
	return false;
}

void FSceneAutoTaggerController::ReconcileAppliedComponentsFromEditor()
{
	bool bChanged = false;
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (!Candidate.IsValid()
			|| !UndoTrackedAppliedActorPaths.Contains(Candidate->ActorPath)
			|| Candidate->bUserCapturePendingAnalysis)
		{
			continue;
		}

		const bool bHasMatchingManagedComponent =
			HasMatchingAppliedComponent(*Candidate, CandidateIndex);

		if (Candidate->bApplied == bHasMatchingManagedComponent)
		{
			continue;
		}

		const bool bWasApplied = Candidate->bApplied;
		Candidate->bApplied = bHasMatchingManagedComponent;
		if (bHasMatchingManagedComponent)
		{
			if (Candidate->Decision == ESceneAutoTaggerDecision::Pending)
			{
				Candidate->Decision = ESceneAutoTaggerDecision::Accepted;
			}
		}
		else if (bWasApplied
			&& Candidate->Decision == ESceneAutoTaggerDecision::Accepted)
		{
			Candidate->Decision = ESceneAutoTaggerDecision::Pending;
		}
		bChanged = true;
	}

	if (bChanged)
	{
		LastApplySummary = TEXT("Applied scene metadata changed through Undo/Redo; review state was refreshed.");
		BroadcastChanged();
	}
}

void FSceneAutoTaggerController::StartExploration(
	UWorld* World,
	const FSceneAutoTaggerRunOptions& InOptions)
{
	FString NativeCoreError;
	if (!EnsureNativeCoreReady(NativeCoreError))
	{
		LastError = NativeCoreError;
		SetState(ESceneAutoTaggerState::Failed, FText::FromString(NativeCoreError));
		return;
	}

	if (!InFlightAnalysisBatches.IsEmpty())
	{
		ActiveRunId = FGuid::NewGuid();
		for (const TPair<FGuid, TSharedPtr<FActiveBatch>>& Pair : InFlightAnalysisBatches)
		{
			if (Pair.Value.IsValid() && Pair.Value->Request.IsValid())
			{
				Pair.Value->Request->Cancel();
				Pair.Value->Request.Reset();
			}
		}
		InFlightAnalysisBatches.Reset();
	}

	Candidates.Reset();
	PendingRepresentatives.Reset();
	VisuallyRejectedActorPaths.Reset();
	VisuallyRejectedReasonsByActorPath.Reset();
	EvidenceDescriptorsByActorPath.Reset();
	UndoTrackedAppliedActorPaths.Reset();
	FrozenDuplicateClusteringPlan.Reset();
	ActiveBatch.Reset();
	PendingAnalysisBatches.Reset();
	InFlightAnalysisBatches.Reset();
	Cache.Reset();
	ActiveWorld = World;
	RunOptions = InOptions;
	RunOptions.VisionCharacterID = RunOptions.VisionCharacterID.TrimStartAndEnd();
	RunOptions.VisionCharacterName = RunOptions.VisionCharacterName.TrimStartAndEnd();
	const UConvaiSceneAutoTaggerSettings* Settings = GetDefault<UConvaiSceneAutoTaggerSettings>();
	if (RunOptions.AnalysisDetail != EConvaiSceneAutoTaggerAnalysisDetail::Standard
		&& RunOptions.AnalysisDetail != EConvaiSceneAutoTaggerAnalysisDetail::Detailed)
	{
		RunOptions.AnalysisDetail = Settings->AnalysisDetail
			== EConvaiSceneAutoTaggerAnalysisDetail::Detailed
			? EConvaiSceneAutoTaggerAnalysisDetail::Detailed
			: EConvaiSceneAutoTaggerAnalysisDetail::Standard;
	}
	const bool bAccurateAnalysis = RunOptions.AnalysisDetail
		== EConvaiSceneAutoTaggerAnalysisDetail::Detailed;
	RunOptions.AnalysisGridDimension = RunOptions.AnalysisGridDimension > 0
		? FMath::Clamp(RunOptions.AnalysisGridDimension, 2, 5)
		: (bAccurateAnalysis ? 3 : 4);
	RunOptions.AnalysisCellResolution = RunOptions.AnalysisCellResolution > 0
		? FMath::Clamp(RunOptions.AnalysisCellResolution, 128, 512)
		: (bAccurateAnalysis
			? 512
			: UConvaiSceneAutoTaggerSettings::DefaultCellResolution);
	const int32 ConfiguredEvidenceResolution =
		FMath::Clamp(Settings->EvidencePreviewResolution, 256, 1024);
	RunOptions.EvidencePreviewResolution = RunOptions.EvidencePreviewResolution > 0
		? FMath::Clamp(RunOptions.EvidencePreviewResolution, 256, 1024)
		: FMath::Max(
			bAccurateAnalysis
				? UConvaiSceneAutoTaggerSettings::MinimumAccurateEvidencePreviewResolution
				: UConvaiSceneAutoTaggerSettings::MinimumFastEvidencePreviewResolution,
			ConfiguredEvidenceResolution);
	Progress = FSceneAutoTaggerProgress();
	LastError.Reset();
	LastApplySummary.Reset();
	NextRepresentative = 0;
	CompletedRepresentatives = 0;
	bCancelRequested = false;
	bCaptureComplete = false;
	bGenerateMovementPointsOnApply = false;
	bCompletionSummaryAcknowledged = false;
	bSingleCandidateRequest = false;
	SingleCandidateIndex = INDEX_NONE;
	SingleCandidateRecaptureSnapshot.Reset();
	ResetReviewBatchState();
	ActiveRunId = FGuid::NewGuid();
	ConvaiSceneAutoTagger::CleanupCaptureResources();

	if (!IsValid(World) || World->WorldType != EWorldType::Editor)
	{
		LastError = TEXT("Open an editor level before exploring the scene.");
		SetState(ESceneAutoTaggerState::Failed, LOCTEXT("InvalidWorld", "No editable level is available."));
		return;
	}

	FString CharacterReadiness;
	if (!EnsureVisionCharacterReady(CharacterReadiness))
	{
		LastError = CharacterReadiness;
		SetState(ESceneAutoTaggerState::Failed, FText::FromString(CharacterReadiness));
		return;
	}
	SetState(
		ESceneAutoTaggerState::Discovering,
		RunOptions.ScopedActors.IsEmpty()
			? LOCTEXT("Discovering", "Filtering significant scene objects...")
			: LOCTEXT("PreparingSelectedActors", "Preparing selected actors..."));
	FSceneAutoTaggerDiscoveryStats DiscoveryStats;
	FSceneAutoTaggerDiscovery::Discover(World, RunOptions, *Settings, Candidates, DiscoveryStats);
	if (Candidates.IsEmpty())
	{
		Progress.Total = 0;
		Progress.Completed = 0;
		const bool bExplicitSelection = RunOptions.bForceAnalyzeScopedActors
			&& !RunOptions.ScopedActors.IsEmpty();
		if (bExplicitSelection)
		{
			LastError = TEXT("None of the selected actors can be scanned. Select actors with visible renderable geometry; actors excluded by project settings remain unavailable.");
		}
		SetState(
			ESceneAutoTaggerState::ReviewReady,
			bExplicitSelection
				? LOCTEXT("NoSelectedCandidates", "The selected actors do not contain eligible renderable geometry.")
				: LOCTEXT("NoCandidates", "No objects passed the current local-relevance filter."));
		bCompletionSummaryAcknowledged = true;
		return;
	}

	BuildFingerprintGroupsAndLoadCache();
}

void FSceneAutoTaggerController::Cancel()
{
	if (!IsBusy())
	{
		return;
	}

	bCancelRequested = true;
	const bool bWasSingleCandidateRequest = bSingleCandidateRequest;
	const bool bWasSingleCandidateRecapture = SingleCandidateRecaptureSnapshot.IsValid();
	const bool bWasReviewBatchRequest = IsReviewBatchRequest();
	const int32 CompletedReviewBatchCount = ReviewBatchSucceededCount;
	const int32 CancelledSingleCandidateIndex = SingleCandidateIndex;
	ActiveRunId = FGuid::NewGuid();
	ActiveBatch.Reset();
	PendingAnalysisBatches.Reset();
	ConvaiSceneAutoTagger::CleanupCaptureResources();
	TArray<TSharedPtr<ISceneAutoTaggerRequest>> RequestsToCancel;
	RequestsToCancel.Reserve(InFlightAnalysisBatches.Num());
	for (const TPair<FGuid, TSharedPtr<FActiveBatch>>& Pair : InFlightAnalysisBatches)
	{
		if (Pair.Value.IsValid() && Pair.Value->Request.IsValid())
		{
			RequestsToCancel.Add(MoveTemp(Pair.Value->Request));
		}
	}
	InFlightAnalysisBatches.Reset();
	for (const TSharedPtr<ISceneAutoTaggerRequest>& Request : RequestsToCancel)
	{
		if (Request.IsValid())
		{
			Request->Cancel();
		}
	}
	RestoreSingleCandidateRecaptureSnapshot();
	if (bWasReviewBatchRequest)
	{
		RestoreAllReviewBatchCandidates();
	}
	bSingleCandidateRequest = false;
	SingleCandidateIndex = INDEX_NONE;
	const auto MarkUnfinishedCandidateCancelled = [](const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate)
	{
		if (Candidate.IsValid() && !Candidate->bTagComplete && Candidate->Error.IsEmpty())
		{
			Candidate->bAnalysisCancelled = true;
		}
	};
	if (bWasReviewBatchRequest)
	{
		ResetReviewBatchState();
		SetState(
			ESceneAutoTaggerState::ReviewReady,
			CompletedReviewBatchCount > 0
				? FText::Format(
					LOCTEXT(
						"ReviewBatchPartiallyCancelled",
						"Batch analysis cancelled. {0} completed object(s) were kept; unfinished objects were restored."),
					FText::AsNumber(CompletedReviewBatchCount))
				: LOCTEXT(
					"ReviewBatchCancelled",
					"Batch analysis cancelled. Unfinished objects were restored to their previous review."));
	}
	else if (bWasSingleCandidateRequest)
	{
		MarkUnfinishedCandidateCancelled(
			Candidates.IsValidIndex(CancelledSingleCandidateIndex)
				? Candidates[CancelledSingleCandidateIndex]
				: nullptr);
		SetState(
			ESceneAutoTaggerState::ReviewReady,
			bWasSingleCandidateRecapture
				? LOCTEXT("SingleRecaptureCancelled", "Recapture cancelled. The previous image, suggestion, and review state were kept.")
				: LOCTEXT("SingleAnalysisCancelled", "Analysis cancelled. The captured image remains available to retry."));
	}
	else
	{
		for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
		{
			MarkUnfinishedCandidateCancelled(Candidate);
		}
		RemoveVisuallyRejectedCandidates();
		if (CountAttentionCandidates() == 0
			&& CountLowInformationFilteredCandidates() == 0)
		{
			bCompletionSummaryAcknowledged = true;
		}
		SetState(
			ESceneAutoTaggerState::Cancelled,
			LOCTEXT("Cancelled", "Exploration cancelled. Completed suggestions remain available for review."));
	}
}

void FSceneAutoTaggerController::ClearResults()
{
	if (IsBusy())
	{
		Cancel();
	}
	Candidates.Reset();
	PendingRepresentatives.Reset();
	VisuallyRejectedActorPaths.Reset();
	VisuallyRejectedReasonsByActorPath.Reset();
	EvidenceDescriptorsByActorPath.Reset();
	UndoTrackedAppliedActorPaths.Reset();
	FrozenDuplicateClusteringPlan.Reset();
	ActiveBatch.Reset();
	PendingAnalysisBatches.Reset();
	InFlightAnalysisBatches.Reset();
	Cache.Reset();
	ActiveWorld.Reset();
	RunOptions.VisionCharacterID.Reset();
	RunOptions.VisionCharacterName.Reset();
	ConvaiSceneAutoTagger::CleanupCaptureResources();
	LastError.Reset();
	LastApplySummary.Reset();
	Progress = FSceneAutoTaggerProgress();
	NextRepresentative = 0;
	CompletedRepresentatives = 0;
	bCancelRequested = false;
	bCaptureComplete = false;
	bGenerateMovementPointsOnApply = false;
	bCompletionSummaryAcknowledged = false;
	bSingleCandidateRequest = false;
	SingleCandidateIndex = INDEX_NONE;
	SingleCandidateRecaptureSnapshot.Reset();
	ResetReviewBatchState();
	ActiveRunId = FGuid::NewGuid();
	BroadcastChanged();
}

void FSceneAutoTaggerController::AbortRun(const FString& Error, const FText& Message)
{
	bCancelRequested = true;
	const bool bWasReviewBatchRequest = IsReviewBatchRequest();
	RestoreSingleCandidateRecaptureSnapshot();
	if (bWasReviewBatchRequest)
	{
		RestoreAllReviewBatchCandidates();
	}
	bSingleCandidateRequest = false;
	SingleCandidateIndex = INDEX_NONE;
	ActiveRunId = FGuid::NewGuid();
	ActiveBatch.Reset();
	PendingAnalysisBatches.Reset();
	TArray<TSharedPtr<ISceneAutoTaggerRequest>> RequestsToCancel;
	RequestsToCancel.Reserve(InFlightAnalysisBatches.Num());
	for (const TPair<FGuid, TSharedPtr<FActiveBatch>>& Pair : InFlightAnalysisBatches)
	{
		if (Pair.Value.IsValid() && Pair.Value->Request.IsValid())
		{
			RequestsToCancel.Add(MoveTemp(Pair.Value->Request));
		}
	}
	InFlightAnalysisBatches.Reset();
	ConvaiSceneAutoTagger::CleanupCaptureResources();
	for (const TSharedPtr<ISceneAutoTaggerRequest>& Request : RequestsToCancel)
	{
		if (Request.IsValid())
		{
			Request->Cancel();
		}
	}
	Progress.Completed = FMath::Min(CompletedRepresentatives, Progress.Total);
	LastError = Error;
	if (bWasReviewBatchRequest)
	{
		ResetReviewBatchState();
		SetState(
			ESceneAutoTaggerState::ReviewReady,
			LOCTEXT("ReviewBatchInterrupted", "Batch analysis stopped. Previous review data was kept for unfinished objects."));
	}
	else
	{
		SetState(ESceneAutoTaggerState::Failed, Message);
	}
}

bool FSceneAutoTaggerController::RestoreSingleCandidateRecaptureSnapshot()
{
	if (!SingleCandidateRecaptureSnapshot.IsValid())
	{
		return false;
	}

	bool bRestored = false;
	if (Candidates.IsValidIndex(SingleCandidateIndex) && Candidates[SingleCandidateIndex].IsValid()
		&& Candidates[SingleCandidateIndex]->ActorPath == SingleCandidateRecaptureSnapshot->ActorPath)
	{
		*Candidates[SingleCandidateIndex] = *SingleCandidateRecaptureSnapshot;
		bRestored = true;
	}
	SingleCandidateRecaptureSnapshot.Reset();
	return bRestored;
}

bool FSceneAutoTaggerController::IsActiveEditorWorldCurrent() const
{
	return ActiveWorld.IsValid()
		&& GEditor
		&& GEditor->GetEditorWorldContext().World() == ActiveWorld.Get();
}

bool FSceneAutoTaggerController::IsNativeCoreReady() const
{
	return FSceneAutoTaggerNativeCoreAdapter::Get().IsAvailable();
}

FString FSceneAutoTaggerController::GetNativeCoreDiagnostic() const
{
	return IsNativeCoreReady() ? FString() : FString(NativeCoreUnavailableMessage);
}

bool FSceneAutoTaggerController::EnsureNativeCoreReady(FString& OutError) const
{
	if (IsNativeCoreReady())
	{
		return true;
	}

	OutError = GetNativeCoreDiagnostic();
	return false;
}

bool FSceneAutoTaggerController::EnsureVisionCharacterReady(FString& OutError) const
{
	if (!RunOptions.VisionCharacterID.IsEmpty())
	{
		return true;
	}

	OutError = TEXT("Choose a Convai character in Scene Auto Tagger setup before analyzing.");
	return false;
}

void FSceneAutoTaggerController::ValidateRetainedReviewState(UWorld* CurrentWorld)
{
	if (Candidates.IsEmpty() || !IsValid(CurrentWorld)
		|| CurrentWorld->WorldType != EWorldType::Editor)
	{
		return;
	}

	if (!ActiveWorld.IsValid() || ActiveWorld.Get() != CurrentWorld)
	{
		ClearResults();
		return;
	}
	if (IsBusy() || IsReviewBatchRequest())
	{
		return;
	}

	bool bChanged = false;
	bool bHasLiveCandidate = false;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (!Candidate.IsValid())
		{
			continue;
		}

		AActor* Actor = Candidate->Actor.Get();
		const FString PreviousActorPath = Candidate->ActorPath;
		if ((!IsValid(Actor) || Actor->GetWorld() != CurrentWorld)
			&& !Candidate->ActorPath.IsEmpty())
		{
			AActor* ResolvedActor = FindObject<AActor>(nullptr, *Candidate->ActorPath);
			if (IsValid(ResolvedActor) && ResolvedActor->GetWorld() == CurrentWorld)
			{
				Candidate->Actor = ResolvedActor;
				Actor = ResolvedActor;
				bChanged = true;
			}
		}
		if (IsValid(Actor) && Actor->GetWorld() == CurrentWorld
			&& FSceneAutoTaggerDiscovery::RefreshCandidateGeometry(*Actor, *Candidate))
		{
			if (!PreviousActorPath.IsEmpty() && PreviousActorPath != Candidate->ActorPath)
			{
				if (VisuallyRejectedActorPaths.Remove(PreviousActorPath) > 0)
				{
					VisuallyRejectedActorPaths.Add(Candidate->ActorPath);
				}
				ESceneAutoTaggerLowInformationReason PreviousReason;
				if (VisuallyRejectedReasonsByActorPath.RemoveAndCopyValue(
					PreviousActorPath,
					PreviousReason))
				{
					VisuallyRejectedReasonsByActorPath.Add(Candidate->ActorPath, PreviousReason);
				}
				TPair<uint32, ConvaiSceneAutoTagger::FSceneAutoTaggerEvidenceDescriptor> PreviousDescriptor;
				if (EvidenceDescriptorsByActorPath.RemoveAndCopyValue(
					PreviousActorPath,
					PreviousDescriptor))
				{
					EvidenceDescriptorsByActorPath.Add(Candidate->ActorPath, MoveTemp(PreviousDescriptor));
				}
				if (UndoTrackedAppliedActorPaths.Remove(PreviousActorPath) > 0)
				{
					UndoTrackedAppliedActorPaths.Add(Candidate->ActorPath);
				}
			}
			bHasLiveCandidate = true;
			bChanged = true;
			continue;
		}

		bChanged |= !Candidate->bDismissedFromReview
			|| Candidate->bFilteredForLowInformation
			|| Candidate->LowInformationReason != ESceneAutoTaggerLowInformationReason::None;
		Candidate->Decision = ESceneAutoTaggerDecision::Pending;
		Candidate->bApplied = false;
		Candidate->bDismissedFromReview = true;
		Candidate->bFilteredForLowInformation = false;
		Candidate->LowInformationReason = ESceneAutoTaggerLowInformationReason::None;
		VisuallyRejectedActorPaths.Remove(Candidate->ActorPath);
		VisuallyRejectedReasonsByActorPath.Remove(Candidate->ActorPath);
		EvidenceDescriptorsByActorPath.Remove(Candidate->ActorPath);
		UndoTrackedAppliedActorPaths.Remove(Candidate->ActorPath);
	}
	if (!bHasLiveCandidate)
	{
		ClearResults();
		return;
	}

	if (CountLowInformationFilteredCandidates() == 0
		&& CountAttentionCandidates() == 0)
	{
		bCompletionSummaryAcknowledged = true;
	}
	if (bChanged)
	{
		BroadcastChanged();
	}
}

bool FSceneAutoTaggerController::CanIncludeCandidate(
	const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate) const
{
	return Candidate.IsValid() && Candidates.Contains(Candidate)
		&& !Candidate->bApplied && !Candidate->bDismissedFromReview
		&& Candidate->Actor.IsValid()
		&& Candidate->bTagComplete && !Candidate->bAnalysisCancelled
		&& !Candidate->bUserCapturePendingAnalysis && Candidate->Error.IsEmpty()
		&& ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
			Candidate->RefinementNote).IsEmpty()
		&& HasApplicableCandidateName(*Candidate);
}

int32 FSceneAutoTaggerController::SetDecisionsByActorPath(
	const TSet<FString>& CandidateActorPaths,
	const ESceneAutoTaggerDecision Decision)
{
	if (CandidateActorPaths.IsEmpty()
		|| (Decision != ESceneAutoTaggerDecision::Pending
			&& Decision != ESceneAutoTaggerDecision::Accepted
			&& Decision != ESceneAutoTaggerDecision::Rejected))
	{
		return 0;
	}

	int32 ChangedCount = 0;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		const bool bCanChangeReviewState = Candidate.IsValid()
			&& !Candidate->ActorPath.IsEmpty()
			&& CandidateActorPaths.Contains(Candidate->ActorPath)
			&& !Candidate->bApplied
			&& !Candidate->bDismissedFromReview;
		const bool bCanInclude = bCanChangeReviewState
			&& Candidate->Actor.IsValid()
			&& Candidate->bTagComplete
			&& !Candidate->bAnalysisCancelled
			&& !Candidate->bUserCapturePendingAnalysis
			&& Candidate->Error.IsEmpty()
			&& ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
				Candidate->RefinementNote).IsEmpty()
			&& HasApplicableCandidateName(*Candidate);
		if (!bCanChangeReviewState
			|| (Decision == ESceneAutoTaggerDecision::Accepted && !bCanInclude)
			|| Candidate->Decision == Decision)
		{
			continue;
		}

		Candidate->Decision = Decision;
		++ChangedCount;
	}
	if (ChangedCount > 0)
	{
		BroadcastChanged();
	}
	return ChangedCount;
}

int32 FSceneAutoTaggerController::DismissCandidatesFromReviewByActorPath(
	const TSet<FString>& CandidateActorPaths)
{
	if (IsBusy() || IsReviewBatchRequest() || CandidateActorPaths.IsEmpty())
	{
		return 0;
	}

	int32 DismissedCount = 0;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (!Candidate.IsValid()
			|| Candidate->ActorPath.IsEmpty()
			|| !CandidateActorPaths.Contains(Candidate->ActorPath)
			|| Candidate->bApplied
			|| Candidate->bDismissedFromReview)
		{
			continue;
		}

		Candidate->Decision = ESceneAutoTaggerDecision::Pending;
		if (!Candidate->RefinementNote.IsEmpty())
		{
			Candidate->RefinementNote.Reset();
			++Candidate->RefinementNoteRevision;
		}
		Candidate->bDismissedFromReview = true;
		Candidate->bFilteredForLowInformation = false;
		Candidate->LowInformationReason = ESceneAutoTaggerLowInformationReason::None;
		++DismissedCount;
	}
	if (DismissedCount > 0)
	{
		BroadcastChanged();
	}
	return DismissedCount;
}

void FSceneAutoTaggerController::AcceptAtOrAbove(float MinimumConfidence)
{
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (Candidate.IsValid() && Candidate->Actor.IsValid() && !Candidate->bApplied
			&& !Candidate->bDismissedFromReview
			&& Candidate->bTagComplete && !Candidate->bAnalysisCancelled
			&& !Candidate->bUserCapturePendingAnalysis && Candidate->Error.IsEmpty()
			&& ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
				Candidate->RefinementNote).IsEmpty()
			&& HasApplicableCandidateName(*Candidate)
			&& Candidate->Confidence >= MinimumConfidence)
		{
			Candidate->Decision = ESceneAutoTaggerDecision::Accepted;
		}
	}
	BroadcastChanged();
}

void FSceneAutoTaggerController::SetAllDecisions(ESceneAutoTaggerDecision Decision)
{
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (Candidate.IsValid() && !Candidate->bApplied && !Candidate->bDismissedFromReview
			&& Candidate->bTagComplete)
		{
			Candidate->Decision = Decision;
		}
	}
	BroadcastChanged();
}

void FSceneAutoTaggerController::NotifyAppliedComponentsRemoved(
	const TSet<FString>& ActorPaths)
{
	if (ActorPaths.IsEmpty())
	{
		return;
	}

	bool bChanged = false;
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (!Candidate.IsValid() || !Candidate->bApplied
			|| !ActorPaths.Contains(Candidate->ActorPath)
			|| HasMatchingAppliedComponent(*Candidate, CandidateIndex))
		{
			continue;
		}
		UndoTrackedAppliedActorPaths.Add(Candidate->ActorPath);
		Candidate->bApplied = false;
		if (Candidate->Decision == ESceneAutoTaggerDecision::Accepted)
		{
			Candidate->Decision = ESceneAutoTaggerDecision::Pending;
		}
		bChanged = true;
	}

	if (bChanged)
	{
		LastApplySummary = TEXT("Removed applied scene metadata; affected review rows returned to review.");
		BroadcastChanged();
	}
}

void FSceneAutoTaggerController::NotifyCandidateEdited(
	const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate)
{
	const int32 CandidateIndex = Candidates.IndexOfByKey(Candidate);
	if (Candidate.IsValid() && CandidateIndex != INDEX_NONE)
	{
		TUniquePtr<ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan> LivePlan;
		const ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan* Plan =
			FrozenDuplicateClusteringPlan.Get();
		if (!Plan)
		{
			const UConvaiSceneAutoTaggerSettings* Settings =
				GetDefault<UConvaiSceneAutoTaggerSettings>();
			LivePlan = MakeUnique<ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan>(
				BuildDuplicateClusteringPlanForCandidates(Candidates, *Settings));
			Plan = LivePlan.Get();
		}

		TArray<int32> EditedIndices = { CandidateIndex };
		if (const ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateCluster* Cluster =
			Plan->FindClusterForCandidate(CandidateIndex);
			Cluster && Cluster->IsMerged())
		{
			EditedIndices = Cluster->CandidateIndices;
		}
		for (const int32 EditedIndex : EditedIndices)
		{
			if (!Candidates.IsValidIndex(EditedIndex) || !Candidates[EditedIndex].IsValid())
			{
				continue;
			}
			TSharedPtr<FSceneAutoTaggerCandidate>& EditedCandidate = Candidates[EditedIndex];
			if (EditedIndex != CandidateIndex)
			{
				EditedCandidate->SuggestedName = Candidate->SuggestedName;
				EditedCandidate->Description = Candidate->Description;
			}
			EditedCandidate->bApplied = false;
			if (EditedCandidate->Decision == ESceneAutoTaggerDecision::Accepted
				&& !HasApplicableCandidateName(*EditedCandidate))
			{
				EditedCandidate->Decision = ESceneAutoTaggerDecision::Pending;
			}
		}
		if (EditedIndices.Num() > 1)
		{
			LastApplySummary = FString::Printf(
				TEXT("Updated metadata across %d copies in this logical object."),
				EditedIndices.Num());
		}
	}
	BroadcastChanged();
}

bool FSceneAutoTaggerController::ReplaceCandidateEvidence(
	const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate,
	const TArray<FColor>& Pixels,
	const int32 Resolution,
	const FVector& ViewDirection,
	const float DistanceScale,
	const FVector2D& TargetOffset,
	FString& OutError)
{
	OutError.Reset();
	if (IsBusy() || !Candidate.IsValid() || !Candidates.Contains(Candidate)
		|| Candidate->bDismissedFromReview)
	{
		OutError = TEXT("The selected object is not available for evidence editing.");
		return false;
	}
	if (Resolution <= 0 || Pixels.Num() != Resolution * Resolution)
	{
		OutError = TEXT("The adjusted view did not produce a valid square capture.");
		return false;
	}
	BuildPreview(*Candidate, Pixels, Resolution);
	Candidate->EvidenceViewDirectionActorLocal = Candidate->Actor.IsValid()
		? Candidate->Actor->GetActorQuat().UnrotateVector(ViewDirection.GetSafeNormal()).GetSafeNormal()
		: ViewDirection.GetSafeNormal();
	Candidate->EvidenceDistanceScale = FMath::Clamp(DistanceScale, 0.55f, 2.5f);
	Candidate->EvidenceTargetOffset = FVector2D(
		FMath::Clamp(TargetOffset.X, -0.75f, 0.75f),
		FMath::Clamp(TargetOffset.Y, -0.75f, 0.75f));
	Candidate->bHasUserCapture = true;
	Candidate->bUserCapturePendingAnalysis = true;
	Candidate->bAnalysisCancelled = false;
	Candidate->bApplied = false;
	Candidate->Decision = ESceneAutoTaggerDecision::Pending;
	Candidate->Error.Reset();
	BroadcastChanged();
	return true;
}

bool FSceneAutoTaggerController::RedescribeCandidate(
	const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate,
	FString& OutError)
{
	OutError.Reset();
	if (!EnsureNativeCoreReady(OutError))
	{
		return false;
	}
	if (IsBusy() || !Candidate.IsValid() || !Candidates.Contains(Candidate)
		|| Candidate->bDismissedFromReview)
	{
		OutError = TEXT("The selected object is not available for analysis.");
		return false;
	}
	const int32 CandidateIndex = Candidates.IndexOfByKey(Candidate);
	if (CandidateIndex == INDEX_NONE || !Candidate->Actor.IsValid())
	{
		OutError = TEXT("The selected actor no longer exists in the current level.");
		return false;
	}
	const int32 Resolution = Candidate->PreviewSize.X;
	if (Resolution <= 0 || Candidate->PreviewSize.Y != Resolution
		|| Candidate->PreviewBGRA.Num() != Resolution * Resolution * static_cast<int32>(sizeof(FColor)))
	{
		OutError = TEXT("A valid captured image is required before analyzing this object again.");
		return false;
	}

	if (!EnsureVisionCharacterReady(OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FActiveBatch>> PreparedBatches;
	TArray<int32> PreparedCandidateIndices;
	if (!BuildRetainedAnalysisBatches(
		{ CandidateIndex },
		PreparedBatches,
		PreparedCandidateIndices,
		OutError)
		|| PreparedBatches.Num() != 1
		|| PreparedCandidateIndices.Num() != 1
		|| PreparedCandidateIndices[0] != CandidateIndex)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("The retained image could not be prepared for analysis.");
		}
		return false;
	}
	ActiveBatch = MoveTemp(PreparedBatches[0]);
	// The request is now fully validated and will start. Retain the row-level
	// recovery error until success/cancellation resolves it, but do not let a
	// global banner from an older batch survive this new attempt.
	LastError.Reset();
	ActiveWorld = Candidate->Actor->GetWorld();
	ActiveRunId = FGuid::NewGuid();
	SingleCandidateRecaptureSnapshot.Reset();
	bCancelRequested = false;
	bCaptureComplete = true;
	bSingleCandidateRequest = true;
	SingleCandidateIndex = CandidateIndex;
	Progress.Total = 1;
	Progress.Completed = 0;
	Candidate->Decision = ESceneAutoTaggerDecision::Pending;
	Candidate->bApplied = false;
	Candidate->bAnalysisCancelled = false;
	// A failed ordinary analysis can reuse its retained evidence without being
	// misrepresented as a user-adjusted view. Keep the previous error until the
	// new request succeeds so cancellation never erases the recovery context.
	Candidate->bUserCapturePendingAnalysis = Candidate->bHasUserCapture;
	PendingAnalysisBatches.Add(ActiveBatch);
	ActiveBatch.Reset();
	PumpAnalysisQueue();
	return true;
}

bool FSceneAutoTaggerController::AnalyzeCandidatesByActorPath(
	const TSet<FString>& CandidateActorPaths,
	FString& OutError)
{
	return StartReviewBatch(
		CandidateActorPaths,
		EReviewBatchMode::RetainedEvidence,
		true,
		OutError);
}

bool FSceneAutoTaggerController::AnalyzeLowInformationCandidatesByActorPath(
	const TSet<FString>& CandidateActorPaths,
	FString& OutError)
{
	OutError.Reset();
	if (!EnsureNativeCoreReady(OutError))
	{
		return false;
	}
	if (IsBusy() || IsReviewBatchRequest())
	{
		OutError = TEXT("Another Scene Auto Tagger operation is already running.");
		return false;
	}
	if (CandidateActorPaths.IsEmpty())
	{
		OutError = TEXT("Select at least one skipped object to analyze.");
		return false;
	}

	TMap<int32, FSceneAutoTaggerCandidate> OriginalCandidates;
	TSet<FString> RestoredActorPaths;
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (!Candidate.IsValid()
			|| !Candidate->bFilteredForLowInformation
			|| !Candidate->bDismissedFromReview
			|| Candidate->bApplied
			|| Candidate->ActorPath.IsEmpty()
			|| !CandidateActorPaths.Contains(Candidate->ActorPath)
			|| !Candidate->Actor.IsValid())
		{
			continue;
		}

		const int32 Resolution = Candidate->PreviewSize.X;
		if (Resolution <= 0 || Candidate->PreviewSize.Y != Resolution
			|| Candidate->PreviewBGRA.Num()
				!= Resolution * Resolution * static_cast<int32>(sizeof(FColor)))
		{
			continue;
		}

		OriginalCandidates.Add(CandidateIndex, *Candidate);
		Candidate->Decision = ESceneAutoTaggerDecision::Pending;
		Candidate->Error.Reset();
		Candidate->bTagComplete = false;
		Candidate->bAnalysisCancelled = true;
		Candidate->bUserCapturePendingAnalysis = false;
		Candidate->bDismissedFromReview = false;
		Candidate->bFilteredForLowInformation = false;
		Candidate->LowInformationReason = ESceneAutoTaggerLowInformationReason::None;
		RestoredActorPaths.Add(Candidate->ActorPath);
	}

	if (RestoredActorPaths.IsEmpty())
	{
		OutError = TEXT("None of the selected skipped objects has a live actor and valid retained image.");
		return false;
	}

	if (StartReviewBatch(
		RestoredActorPaths,
		EReviewBatchMode::RetainedEvidence,
		true,
		OutError))
	{
		return true;
	}

	for (const TPair<int32, FSceneAutoTaggerCandidate>& Pair : OriginalCandidates)
	{
		if (Candidates.IsValidIndex(Pair.Key) && Candidates[Pair.Key].IsValid())
		{
			*Candidates[Pair.Key] = Pair.Value;
		}
	}
	return false;
}

bool FSceneAutoTaggerController::RecaptureAndAnalyzeCandidatesByActorPath(
	const TSet<FString>& CandidateActorPaths,
	FString& OutError)
{
	return StartReviewBatch(
		CandidateActorPaths,
		EReviewBatchMode::FreshCapture,
		true,
		OutError);
}

void FSceneAutoTaggerController::MergeDiscoveredCandidatesIntoReview(
	const TArray<TSharedPtr<FSceneAutoTaggerCandidate>>& DiscoveredCandidates,
	TSet<FString>& OutAnalysisActorPaths,
	TArray<int32>& OutAddedCandidateIndices,
	TMap<int32, FSceneAutoTaggerCandidate>& OutRestoredSnapshots,
	FSceneAutoTaggerAddToReviewResult& OutResult)
{
	OutAnalysisActorPaths.Reset();
	OutAddedCandidateIndices.Reset();
	OutRestoredSnapshots.Reset();
	OutResult.Added = 0;
	OutResult.Restored = 0;

	TMap<FString, int32> ExistingIndexByActorPath;
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (Candidate.IsValid() && !Candidate->ActorPath.IsEmpty())
		{
			ExistingIndexByActorPath.FindOrAdd(Candidate->ActorPath, CandidateIndex);
		}
	}

	TSet<FString> SeenDiscoveryPaths;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Discovered : DiscoveredCandidates)
	{
		if (!Discovered.IsValid() || Discovered->ActorPath.IsEmpty()
			|| SeenDiscoveryPaths.Contains(Discovered->ActorPath))
		{
			continue;
		}
		SeenDiscoveryPaths.Add(Discovered->ActorPath);

		if (const int32* ExistingIndex = ExistingIndexByActorPath.Find(Discovered->ActorPath))
		{
			if (!Candidates.IsValidIndex(*ExistingIndex) || !Candidates[*ExistingIndex].IsValid())
			{
				continue;
			}
			FSceneAutoTaggerCandidate& Existing = *Candidates[*ExistingIndex];
			if (!Existing.bDismissedFromReview || Existing.bApplied)
			{
				continue;
			}

			OutRestoredSnapshots.Add(*ExistingIndex, Existing);
			// Refresh only discovery-owned state. Retained evidence and authored edits
			// remain the rollback snapshot until the fresh analysis succeeds.
			Existing.Actor = Discovered->Actor;
			Existing.PrimitiveComponents = Discovered->PrimitiveComponents;
			Existing.WorldBounds = Discovered->WorldBounds;
			Existing.ActorLabel = Discovered->ActorLabel;
			Existing.AssetSummary = Discovered->AssetSummary;
			Existing.SignificanceReason = Discovered->SignificanceReason;
			Existing.SignificanceScore = Discovered->SignificanceScore;
			Existing.SignificanceReasonFlags = Discovered->SignificanceReasonFlags;
			Existing.bHasAuthoredLabel = Discovered->bHasAuthoredLabel;
			Existing.bHadConvaiComponent = Discovered->bHadConvaiComponent;
			Existing.bDismissedFromReview = false;
			Existing.bFilteredForLowInformation = false;
			Existing.LowInformationReason = ESceneAutoTaggerLowInformationReason::None;
			Existing.Decision = ESceneAutoTaggerDecision::Pending;
			OutAnalysisActorPaths.Add(Existing.ActorPath);
			++OutResult.Restored;
			continue;
		}

		const int32 NewCandidateIndex = Candidates.Num();
		Discovered->DuplicateRepresentative = NewCandidateIndex;
		Discovered->DuplicateGroupSize = 1;
		// This is the state restored if a provider/startup failure or cancellation
		// interrupts the fresh-capture batch. It must read as retryable, not Processing.
		Discovered->bAnalysisCancelled = true;
		Candidates.Add(Discovered);
		ExistingIndexByActorPath.Add(Discovered->ActorPath, NewCandidateIndex);
		OutAddedCandidateIndices.Add(NewCandidateIndex);
		OutAnalysisActorPaths.Add(Discovered->ActorPath);
		++OutResult.Added;
	}
}

bool FSceneAutoTaggerController::AddActorsToCurrentReview(
	const TArray<TWeakObjectPtr<AActor>>& Actors,
	FSceneAutoTaggerAddToReviewResult& OutResult,
	FString& OutError)
{
	OutResult = FSceneAutoTaggerAddToReviewResult();
	OutError.Reset();
	if (!EnsureNativeCoreReady(OutError))
	{
		return false;
	}
	if (!IsInGameThread())
	{
		OutError = TEXT("Selected actors can only be added from the editor game thread.");
		return false;
	}
	if (IsBusy() || IsReviewBatchRequest())
	{
		OutError = TEXT("Another Scene Auto Tagger operation is already running.");
		return false;
	}
	if (Candidates.IsEmpty() || !ActiveWorld.IsValid() || !IsActiveEditorWorldCurrent())
	{
		OutError = TEXT("Open an existing review in the current level before adding actors.");
		return false;
	}

	TArray<TWeakObjectPtr<AActor>> UniqueActors;
	TSet<FString> SelectedActorPaths;
	for (const TWeakObjectPtr<AActor>& WeakActor : Actors)
	{
		AActor* Actor = WeakActor.Get();
		if (!IsValid(Actor) || Actor->GetWorld() != ActiveWorld.Get())
		{
			continue;
		}
		const FString ActorPath = Actor->GetPathName();
		if (!ActorPath.IsEmpty() && !SelectedActorPaths.Contains(ActorPath))
		{
			SelectedActorPaths.Add(ActorPath);
			UniqueActors.Add(Actor);
		}
	}
	if (UniqueActors.IsEmpty())
	{
		OutError = TEXT("Select one or more actors in the current Level Editor first.");
		return false;
	}

	FSceneAutoTaggerRunOptions AddOptions = RunOptions;
	AddOptions.ScopedActors = UniqueActors;
	AddOptions.bForceAnalyzeScopedActors = true;
	TArray<TSharedPtr<FSceneAutoTaggerCandidate>> DiscoveredCandidates;
	FSceneAutoTaggerDiscoveryStats AddStats;
	FSceneAutoTaggerDiscovery::Discover(
		ActiveWorld.Get(),
		AddOptions,
		*GetDefault<UConvaiSceneAutoTaggerSettings>(),
		DiscoveredCandidates,
		AddStats);

	const int32 OriginalCandidateCount = Candidates.Num();
	TSet<FString> AnalysisActorPaths;
	TArray<int32> AddedCandidateIndices;
	TMap<int32, FSceneAutoTaggerCandidate> RestoredSnapshots;
	MergeDiscoveredCandidatesIntoReview(
		DiscoveredCandidates,
		AnalysisActorPaths,
		AddedCandidateIndices,
		RestoredSnapshots,
		OutResult);
	OutResult.Skipped = FMath::Max(
		0,
		UniqueActors.Num() - OutResult.Added - OutResult.Restored);

	if (AnalysisActorPaths.IsEmpty())
	{
		OutError = TEXT("The selected actors are already in this review or are not eligible for scene analysis.");
		return false;
	}

	if (StartReviewBatch(
		AnalysisActorPaths,
		EReviewBatchMode::FreshCapture,
		false,
		OutError))
	{
		return true;
	}

	// StartReviewBatch validates the provider and live editor world before it
	// snapshots anything. Roll back this append atomically when startup fails.
	Candidates.SetNum(OriginalCandidateCount);
	for (const TPair<int32, FSceneAutoTaggerCandidate>& Pair : RestoredSnapshots)
	{
		if (Candidates.IsValidIndex(Pair.Key) && Candidates[Pair.Key].IsValid())
		{
			*Candidates[Pair.Key] = Pair.Value;
		}
	}
	OutResult.Added = 0;
	OutResult.Restored = 0;
	return false;
}

bool FSceneAutoTaggerController::StartReviewBatch(
	const TSet<FString>& CandidateActorPaths,
	const EReviewBatchMode Mode,
	const bool bReconcileAllVisualDuplicates,
	FString& OutError)
{
	OutError.Reset();
	if (!EnsureNativeCoreReady(OutError))
	{
		return false;
	}
	if (IsBusy() || IsReviewBatchRequest())
	{
		OutError = TEXT("Another Scene Auto Tagger operation is already running.");
		return false;
	}
	if (CandidateActorPaths.IsEmpty()
		|| (Mode != EReviewBatchMode::RetainedEvidence && Mode != EReviewBatchMode::FreshCapture))
	{
		OutError = TEXT("Select at least one review row to analyze.");
		return false;
	}

	if (!EnsureVisionCharacterReady(OutError))
	{
		return false;
	}

	UWorld* ReviewWorld = nullptr;
	TArray<int32> EligibleCandidateIndices;
	EligibleCandidateIndices.Reserve(CandidateActorPaths.Num());
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (!Candidate.IsValid()
			|| Candidate->ActorPath.IsEmpty()
			|| !CandidateActorPaths.Contains(Candidate->ActorPath)
			|| Candidate->bApplied
			|| Candidate->bDismissedFromReview
			|| !Candidate->Actor.IsValid())
		{
			continue;
		}
		AActor* Actor = Candidate->Actor.Get();
		UWorld* CandidateWorld = Actor ? Actor->GetWorld() : nullptr;
		if (!IsValid(CandidateWorld) || CandidateWorld->WorldType != EWorldType::Editor)
		{
			continue;
		}
		if (!ReviewWorld)
		{
			ReviewWorld = CandidateWorld;
		}
		if (CandidateWorld != ReviewWorld)
		{
			continue;
		}
		if (Mode == EReviewBatchMode::RetainedEvidence)
		{
			const int32 Resolution = Candidate->PreviewSize.X;
			if (Resolution <= 0
				|| Candidate->PreviewSize.Y != Resolution
				|| Candidate->PreviewBGRA.Num()
					!= Resolution * Resolution * static_cast<int32>(sizeof(FColor)))
			{
				continue;
			}
		}
		EligibleCandidateIndices.Add(CandidateIndex);
	}

	if (EligibleCandidateIndices.IsEmpty() || !ReviewWorld
		|| !GEditor || GEditor->GetEditorWorldContext().World() != ReviewWorld)
	{
		OutError = Mode == EReviewBatchMode::RetainedEvidence
			? TEXT("None of the selected, unapplied rows has valid retained evidence in the current level.")
			: TEXT("None of the selected, unapplied rows has a live actor in the current level.");
		return false;
	}

	TArray<TSharedPtr<FActiveBatch>> PreparedRetainedBatches;
	TArray<int32> PreparedCandidateIndices = EligibleCandidateIndices;
	FString PreparationWarning;
	if (Mode == EReviewBatchMode::RetainedEvidence)
	{
		PreparedCandidateIndices.Reset();
		if (!BuildRetainedAnalysisBatches(
			EligibleCandidateIndices,
			PreparedRetainedBatches,
			PreparedCandidateIndices,
			PreparationWarning)
			|| PreparedCandidateIndices.IsEmpty())
		{
			OutError = PreparationWarning.IsEmpty()
				? TEXT("The selected retained images could not be prepared for batch analysis.")
				: PreparationWarning;
			return false;
		}
	}

	ResetReviewBatchState();
	ReviewBatchMode = Mode;
	bReviewBatchReconcileAllVisualDuplicates = bReconcileAllVisualDuplicates;
	for (const int32 CandidateIndex : PreparedCandidateIndices)
	{
		if (Candidates.IsValidIndex(CandidateIndex) && Candidates[CandidateIndex].IsValid()
			&& !ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
				Candidates[CandidateIndex]->RefinementNote).IsEmpty())
		{
			// A note is actor-specific editorial guidance. Global reconciliation could
			// overwrite it with an older duplicate or propagate it to other actors.
			bReviewBatchReconcileAllVisualDuplicates = false;
			break;
		}
	}
	ActiveBatch.Reset();
	PendingAnalysisBatches.Reset();
	InFlightAnalysisBatches.Reset();
	PendingRepresentatives.Reset();
	ActiveWorld = ReviewWorld;
	ActiveRunId = FGuid::NewGuid();
	bCancelRequested = false;
	bCaptureComplete = Mode == EReviewBatchMode::RetainedEvidence;
	bSingleCandidateRequest = false;
	SingleCandidateIndex = INDEX_NONE;
	SingleCandidateRecaptureSnapshot.Reset();
	NextRepresentative = 0;
	CompletedRepresentatives = 0;
	Progress.Total = PreparedCandidateIndices.Num();
	Progress.Completed = 0;
	LastError = MoveTemp(PreparationWarning);
	OutError.Reset();
	SnapshotReviewBatchCandidates(PreparedCandidateIndices);
	for (const int32 CandidateIndex : PreparedCandidateIndices)
	{
		if (Candidates.IsValidIndex(CandidateIndex) && Candidates[CandidateIndex].IsValid())
		{
			Candidates[CandidateIndex]->bAnalysisCancelled = false;
		}
	}

	if (Mode == EReviewBatchMode::RetainedEvidence)
	{
		PendingAnalysisBatches = MoveTemp(PreparedRetainedBatches);
		SetState(
			ESceneAutoTaggerState::Tagging,
			LOCTEXT("AnalyzingReviewBatch", "Analyzing selected objects in parallel..."));
		PumpAnalysisQueue();
	}
	else
	{
		PendingRepresentatives = MoveTemp(PreparedCandidateIndices);
		ConvaiSceneAutoTagger::CleanupCaptureResources();
		ProcessNextBatch();
	}
	return true;
}

bool FSceneAutoTaggerController::BuildRetainedAnalysisBatches(
	const TArray<int32>& CandidateIndices,
	TArray<TSharedPtr<FActiveBatch>>& OutBatches,
	TArray<int32>& OutPreparedCandidateIndices,
	FString& OutError) const
{
	OutBatches.Reset();
	OutPreparedCandidateIndices.Reset();
	OutError.Reset();
	const int32 GridDimension = GetRunAnalysisGridDimension();
	const int32 CellResolution = GetRunAnalysisCellResolution();
	const int32 Capacity = FMath::Square(GridDimension);
	TArray<int32> TargetIndices;
	TSet<int32> TargetSet;
	UWorld* TargetWorld = nullptr;
	for (const int32 CandidateIndex : CandidateIndices)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate> Candidate = Candidates.IsValidIndex(CandidateIndex)
			? Candidates[CandidateIndex]
			: nullptr;
		const int32 Resolution = Candidate.IsValid() ? Candidate->PreviewSize.X : 0;
		UWorld* CandidateWorld = Candidate.IsValid() && Candidate->Actor.IsValid()
			? Candidate->Actor->GetWorld()
			: nullptr;
		if (TargetSet.Contains(CandidateIndex)
			|| !Candidate.IsValid()
			|| !CandidateWorld
			|| (TargetWorld && CandidateWorld != TargetWorld)
			|| Resolution <= 0
			|| Candidate->PreviewSize.Y != Resolution
			|| Candidate->PreviewBGRA.Num()
				!= Resolution * Resolution * static_cast<int32>(sizeof(FColor)))
		{
			continue;
		}
		TargetWorld = CandidateWorld;
		TargetSet.Add(CandidateIndex);
		TargetIndices.Add(CandidateIndex);
	}
	if (TargetIndices.IsEmpty())
	{
		OutError = TEXT("No selected object has valid retained evidence in one editor level.");
		return false;
	}

	TArray<int32> ContextIndices;
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		const int32 Resolution = Candidate.IsValid() ? Candidate->PreviewSize.X : 0;
		if (TargetSet.Contains(CandidateIndex)
			|| !Candidate.IsValid()
			|| Candidate->bDismissedFromReview
			|| !Candidate->Actor.IsValid()
			|| Candidate->Actor->GetWorld() != TargetWorld
			|| Resolution <= 0
			|| Candidate->PreviewSize.Y != Resolution
			|| Candidate->PreviewBGRA.Num()
				!= Resolution * Resolution * static_cast<int32>(sizeof(FColor)))
		{
			continue;
		}
		ContextIndices.Add(CandidateIndex);
	}

	TArray<FContextBatchSelection> PlannedBatches;
	FString PlanError;
	if (!BuildContextBatchPlan(
		Candidates,
		TargetIndices,
		ContextIndices,
		Capacity,
		true,
		PlannedBatches,
		PlanError))
	{
		OutError = FString::Printf(TEXT("Spatial batch planning failed: %s"), *PlanError);
		return false;
	}

	const auto AppendRetainedCell = [this, CellResolution](
		FActiveBatch& Batch,
		const int32 CandidateIndex,
		const bool bIsTarget)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate> Candidate = Candidates.IsValidIndex(CandidateIndex)
			? Candidates[CandidateIndex]
			: nullptr;
		const int32 Resolution = Candidate.IsValid() ? Candidate->PreviewSize.X : 0;
		if (!Candidate.IsValid()
			|| Resolution <= 0
			|| Candidate->PreviewSize.Y != Resolution
			|| Candidate->PreviewBGRA.Num()
				!= Resolution * Resolution * static_cast<int32>(sizeof(FColor)))
		{
			return false;
		}
		TArray<FColor> FullPixels;
		FullPixels.SetNumUninitialized(Resolution * Resolution);
		FMemory::Memcpy(
			FullPixels.GetData(),
			Candidate->PreviewBGRA.GetData(),
			Candidate->PreviewBGRA.Num());
		TArray<FColor> CellPixels = DownsampleSquareImage(
			FullPixels,
			Resolution,
			CellResolution);
		if (CellPixels.Num() != CellResolution * CellResolution)
		{
			return false;
		}
		Batch.CapturedRepresentativeIndices.Add(CandidateIndex);
		Batch.CapturedTargetFlags.Add(bIsTarget);
		Batch.CapturedPreviewRevisions.Add(Candidate->PreviewRevision);
		Batch.CapturedCells.Add(MoveTemp(CellPixels));
		const FString EchoId = BuildEchoId(*Candidate, Batch.CapturedCells.Num());
		Batch.EchoIds.Add(EchoId);
		Batch.ObjectPromptContexts.Add(BuildObjectPromptContext(
			*Candidate,
			EchoId,
			bIsTarget,
			!bIsTarget,
			true));
		Batch.CapturedRefinementNoteRevisions.Add(
			bIsTarget ? Candidate->RefinementNoteRevision : 0u);
		return true;
	};

	for (const FContextBatchSelection& PlannedBatch : PlannedBatches)
	{
		TSharedPtr<FActiveBatch> Batch = MakeShared<FActiveBatch>();
		const int32 PreparedStart = OutPreparedCandidateIndices.Num();
		for (const int32 CandidateIndex : PlannedBatch.TargetIndices)
		{
			if (AppendRetainedCell(*Batch, CandidateIndex, true))
			{
				Batch->RequestedRepresentativeIndices.Add(CandidateIndex);
				OutPreparedCandidateIndices.Add(CandidateIndex);
			}
		}
		if (Batch->RequestedRepresentativeIndices.IsEmpty())
		{
			continue;
		}
		for (const int32 CandidateIndex : PlannedBatch.ContextIndices)
		{
			AppendRetainedCell(*Batch, CandidateIndex, false);
		}

		const int32 RetainedGridDimension = FMath::Clamp(
			FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Batch->CapturedCells.Num()))),
			1,
			GridDimension);
		FString CompositeError;
		if (!ConvaiSceneAutoTagger::CompositeGridPng(
			Batch->CapturedCells,
			RetainedGridDimension,
			CellResolution,
			RetainedGridDimension * CellResolution,
			Batch->GridPngBytes,
			CompositeError)
			|| Batch->GridPngBytes.IsEmpty())
		{
			AppendWarning(
				OutError,
				FString::Printf(TEXT("One retained-image contact sheet could not be encoded: %s"), *CompositeError));
			OutPreparedCandidateIndices.SetNum(PreparedStart, EAllowShrinking::No);
			continue;
		}
		OutBatches.Add(MoveTemp(Batch));
	}
	if (OutBatches.IsEmpty() && OutError.IsEmpty())
	{
		OutError = TEXT("The selected retained images could not be prepared for analysis.");
	}
	return !OutBatches.IsEmpty();
}

void FSceneAutoTaggerController::SnapshotReviewBatchCandidates(
	const TArray<int32>& CandidateIndices)
{
	ReviewBatchSnapshots.Reset();
	ReviewBatchExpectedPreviewRevisions.Reset();
	CompletedReviewBatchActorPaths.Reset();
	ReviewBatchSucceededCount = 0;
	ReviewBatchPreservedCount = 0;
	for (const int32 CandidateIndex : CandidateIndices)
	{
		if (!Candidates.IsValidIndex(CandidateIndex) || !Candidates[CandidateIndex].IsValid())
		{
			continue;
		}
		const FSceneAutoTaggerCandidate& Candidate = *Candidates[CandidateIndex];
		if (Candidate.ActorPath.IsEmpty())
		{
			continue;
		}
		ReviewBatchSnapshots.Add(
			Candidate.ActorPath,
			MakeUnique<FSceneAutoTaggerCandidate>(Candidate));
		ReviewBatchExpectedPreviewRevisions.Add(
			Candidate.ActorPath,
			Candidate.PreviewRevision);
	}
}

bool FSceneAutoTaggerController::RestoreReviewBatchCandidate(const int32 CandidateIndex)
{
	if (!Candidates.IsValidIndex(CandidateIndex) || !Candidates[CandidateIndex].IsValid())
	{
		return false;
	}
	FSceneAutoTaggerCandidate& Candidate = *Candidates[CandidateIndex];
	TUniquePtr<FSceneAutoTaggerCandidate>* Snapshot = ReviewBatchSnapshots.Find(Candidate.ActorPath);
	const uint32* ExpectedRevision = ReviewBatchExpectedPreviewRevisions.Find(Candidate.ActorPath);
	const bool bCanRestore = Snapshot && Snapshot->IsValid() && ExpectedRevision
		&& Candidate.PreviewRevision == *ExpectedRevision;
	if (bCanRestore)
	{
		Candidate = **Snapshot;
	}
	ReviewBatchSnapshots.Remove(Candidate.ActorPath);
	ReviewBatchExpectedPreviewRevisions.Remove(Candidate.ActorPath);
	return bCanRestore;
}

void FSceneAutoTaggerController::RestoreAllReviewBatchCandidates()
{
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		if (Candidates[CandidateIndex].IsValid()
			&& ReviewBatchSnapshots.Contains(Candidates[CandidateIndex]->ActorPath))
		{
			RestoreReviewBatchCandidate(CandidateIndex);
		}
	}
	ReviewBatchSnapshots.Reset();
	ReviewBatchExpectedPreviewRevisions.Reset();
}

bool FSceneAutoTaggerController::CommitReviewBatchResult(
	const int32 CandidateIndex,
	const uint32 ExpectedPreviewRevision,
	const FSceneAutoTaggerVisionItem& Item)
{
	if (!Candidates.IsValidIndex(CandidateIndex) || !Candidates[CandidateIndex].IsValid())
	{
		return false;
	}
	FSceneAutoTaggerCandidate& Candidate = *Candidates[CandidateIndex];
	const uint32* SubmittedRevision = ReviewBatchExpectedPreviewRevisions.Find(Candidate.ActorPath);
	if (!SubmittedRevision
		|| *SubmittedRevision != ExpectedPreviewRevision
		|| Candidate.PreviewRevision != ExpectedPreviewRevision)
	{
		PreserveReviewBatchCandidate(
			CandidateIndex,
			TEXT("A selected object's image changed while analysis was running; its newer local review was kept."));
		return false;
	}

	FString SubmittedNote;
	uint32 SubmittedNoteRevision = MAX_uint32;
	if (const TUniquePtr<FSceneAutoTaggerCandidate>* Snapshot =
		ReviewBatchSnapshots.Find(Candidate.ActorPath))
	{
		SubmittedNote = ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
			(*Snapshot)->RefinementNote);
		SubmittedNoteRevision = (*Snapshot)->RefinementNoteRevision;
	}
	ApplyResultToRepresentative(CandidateIndex, Item);
	const bool bConsumedRefinement = !SubmittedNote.IsEmpty()
		&& Candidate.RefinementNoteRevision == SubmittedNoteRevision
		&& ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
			Candidate.RefinementNote) == SubmittedNote;
	if (bConsumedRefinement)
	{
		Candidate.RefinementNote.Reset();
		++Candidate.RefinementNoteRevision;
		if (Cache && Cache->Remove(Candidate.Fingerprint))
		{
			FString CacheSaveError;
			if (!Cache->Save(CacheSaveError))
			{
				AppendWarning(LastError, CacheSaveError);
			}
		}
	}
	else if (!SubmittedNote.IsEmpty())
	{
		AppendWarning(
			LastError,
			TEXT("A suggestion was updated, but a newer refinement note is still saved and needs analysis."));
	}
	Candidate.Decision = ESceneAutoTaggerDecision::Pending;
	Candidate.bApplied = false;
	Candidate.bUserCapturePendingAnalysis = false;
	if (ReviewBatchMode == EReviewBatchMode::FreshCapture)
	{
		Candidate.bHasUserCapture = false;
	}
	ReviewBatchSnapshots.Remove(Candidate.ActorPath);
	ReviewBatchExpectedPreviewRevisions.Remove(Candidate.ActorPath);
	if (!CompletedReviewBatchActorPaths.Contains(Candidate.ActorPath))
	{
		CompletedReviewBatchActorPaths.Add(Candidate.ActorPath);
		++ReviewBatchSucceededCount;
		++CompletedRepresentatives;
	}
	return true;
}

void FSceneAutoTaggerController::PreserveReviewBatchCandidate(
	const int32 CandidateIndex,
	const FString& Error)
{
	AppendWarning(LastError, Error);
	if (!Candidates.IsValidIndex(CandidateIndex) || !Candidates[CandidateIndex].IsValid())
	{
		return;
	}
	const FString ActorPath = Candidates[CandidateIndex]->ActorPath;
	RestoreReviewBatchCandidate(CandidateIndex);
	if (!CompletedReviewBatchActorPaths.Contains(ActorPath))
	{
		CompletedReviewBatchActorPaths.Add(ActorPath);
		++ReviewBatchPreservedCount;
		++CompletedRepresentatives;
	}
}

void FSceneAutoTaggerController::FinishReviewBatch()
{
	TArray<int32> UnfinishedCandidateIndices;
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		if (Candidates[CandidateIndex].IsValid()
			&& ReviewBatchSnapshots.Contains(Candidates[CandidateIndex]->ActorPath))
		{
			UnfinishedCandidateIndices.Add(CandidateIndex);
		}
	}
	for (const int32 CandidateIndex : UnfinishedCandidateIndices)
	{
		PreserveReviewBatchCandidate(
			CandidateIndex,
			TEXT("One selected object did not finish analysis; its previous review was kept."));
	}
	if (ReviewBatchSucceededCount > 0 && bReviewBatchReconcileAllVisualDuplicates)
	{
		ReconcileVisualDuplicates();
	}

	const int32 SucceededCount = ReviewBatchSucceededCount;
	const int32 PreservedCount = ReviewBatchPreservedCount;
	ActiveBatch.Reset();
	PendingRepresentatives.Reset();
	PendingAnalysisBatches.Reset();
	InFlightAnalysisBatches.Reset();
	ConvaiSceneAutoTagger::CleanupCaptureResources();
	Progress.Completed = Progress.Total;
	bCaptureComplete = true;
	ResetReviewBatchState();
	SetState(
		ESceneAutoTaggerState::ReviewReady,
		PreservedCount > 0
			? FText::Format(
				LOCTEXT("ReviewBatchPartiallyReady", "{0} selected object(s) updated; {1} kept their previous review."),
				FText::AsNumber(SucceededCount),
				FText::AsNumber(PreservedCount))
			: FText::Format(
				LOCTEXT("ReviewBatchReady", "{0} selected object(s) are ready for review."),
				FText::AsNumber(SucceededCount)));
}

void FSceneAutoTaggerController::ResetReviewBatchState()
{
	ReviewBatchMode = EReviewBatchMode::None;
	ReviewBatchSnapshots.Reset();
	ReviewBatchExpectedPreviewRevisions.Reset();
	CompletedReviewBatchActorPaths.Reset();
	ReviewBatchSucceededCount = 0;
	ReviewBatchPreservedCount = 0;
	bReviewBatchReconcileAllVisualDuplicates = true;
}

int32 FSceneAutoTaggerController::GetRunAnalysisGridDimension() const
{
	if (RunOptions.AnalysisGridDimension > 0)
	{
		return FMath::Clamp(RunOptions.AnalysisGridDimension, 2, 5);
	}
	return GetDefault<UConvaiSceneAutoTaggerSettings>()->GetEffectiveGridDimension();
}

int32 FSceneAutoTaggerController::GetRunAnalysisCellResolution() const
{
	if (RunOptions.AnalysisCellResolution > 0)
	{
		return FMath::Clamp(RunOptions.AnalysisCellResolution, 128, 512);
	}
	return GetDefault<UConvaiSceneAutoTaggerSettings>()->GetEffectiveCellResolution();
}

int32 FSceneAutoTaggerController::GetRunEvidencePreviewResolution() const
{
	if (RunOptions.EvidencePreviewResolution > 0)
	{
		return FMath::Clamp(RunOptions.EvidencePreviewResolution, 256, 1024);
	}
	return GetDefault<UConvaiSceneAutoTaggerSettings>()->GetEffectiveEvidencePreviewResolution();
}

void FSceneAutoTaggerController::BuildFingerprintGroupsAndLoadCache()
{
	// The native core DLL owns the tagging prompt, so the cache identity pins
	// both the client request shape and the DLL's opaque vision protocol id; a
	// prompt or encoding change in the core invalidates cached descriptions.
	const FString ProviderId = TEXT("convai-vision-autotag");
	const FString PromptIdentity = FString::Printf(
		TEXT("%s+%s"),
		ConvaiSceneAutoTagger::VisionProtocol::GetPromptVersion(),
		*FSceneAutoTaggerNativeCoreAdapter::Get().GetVisionProtocolId());
	Cache = MakeUnique<ConvaiSceneAutoTagger::FSceneAutoTaggerCache>(
		ConvaiSceneAutoTagger::FSceneAutoTaggerCacheIdentity(
			ProviderId,
			PromptIdentity,
			ConvaiSceneAutoTagger::NativeCorePolicyId(),
			ConvaiSceneAutoTagger::BuildCapturePolicyId(
				GetRunEvidencePreviewResolution(),
				GetRunAnalysisCellResolution(),
				GetRunAnalysisGridDimension()),
			RunOptions.VisionCharacterID,
			RunOptions.SceneContext,
			RunOptions.DescriptionFocus));

	const UConvaiSceneAutoTaggerSettings* Settings = GetDefault<UConvaiSceneAutoTaggerSettings>();
	if (Settings->bUseGeometryCache)
	{
		FString CacheError;
		bool bInvalidated = false;
		if (!Cache->Load(CacheError, &bInvalidated))
		{
			AppendWarning(LastError, CacheError);
		}
	}
	else
	{
		Cache->Reset();
	}

	TMap<FString, int32> FingerprintRepresentatives;
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (!Candidate.IsValid())
		{
			continue;
		}

		TArray<UPrimitiveComponent*> Components = ResolvePrimitiveComponents(*Candidate);
		if (AActor* Actor = Candidate->Actor.Get(); Actor && !Components.IsEmpty())
		{
			const ConvaiSceneAutoTagger::FSceneAutoTaggerViewPlan ViewPlan =
				ConvaiSceneAutoTagger::BuildActorLocalViewPlan(*Actor, Components);
			// Cache hits skip the capture loop, but their later Edit View still needs
			// the same geometry and lighting policy that a fresh probe would have selected.
			Candidate->bOpposedBroadFacePresentation = ViewPlan.bOpposedBroadFacePresentation;
			Candidate->CaptureLightingPolicy = ViewPlan.CaptureLightingPolicy;
		}
		FString FingerprintWarning;
		Candidate->Fingerprint = ConvaiSceneAutoTagger::ComputeObjectFingerprint(Components, &FingerprintWarning);

		if (!Candidate->Fingerprint.IsEmpty())
		{
			if (const int32* ExistingRepresentative = FingerprintRepresentatives.Find(Candidate->Fingerprint))
			{
				Candidate->DuplicateRepresentative = *ExistingRepresentative;
				++Candidates[*ExistingRepresentative]->DuplicateGroupSize;
				continue;
			}
			FingerprintRepresentatives.Add(Candidate->Fingerprint, CandidateIndex);
		}

		Candidate->DuplicateRepresentative = CandidateIndex;
		PendingRepresentatives.Add(CandidateIndex);
	}

	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (Candidate.IsValid()
			&& Candidate->DuplicateRepresentative != INDEX_NONE
			&& Candidate->DuplicateRepresentative != CandidateIndex
			&& Candidates.IsValidIndex(Candidate->DuplicateRepresentative))
		{
			Candidate->DuplicateGroupSize = Candidates[Candidate->DuplicateRepresentative]->DuplicateGroupSize;
		}
	}

	Progress.Total = PendingRepresentatives.Num();
	Progress.Completed = 0;
	TArray<int32> UncachedRepresentatives;
	UncachedRepresentatives.Reserve(PendingRepresentatives.Num());
	for (int32 RepresentativeIndex : PendingRepresentatives)
	{
		TSharedPtr<FSceneAutoTaggerCandidate> Candidate = Candidates.IsValidIndex(RepresentativeIndex)
			? Candidates[RepresentativeIndex]
			: nullptr;
		const ConvaiSceneAutoTagger::FSceneAutoTaggerCachedTag* Cached =
			Settings->bUseGeometryCache && Candidate.IsValid() && !Candidate->Fingerprint.IsEmpty()
				? Cache->Find(Candidate->Fingerprint)
				: nullptr;
		if (!Cached || Cached->Name.IsEmpty() || Cached->Description.IsEmpty())
		{
			UncachedRepresentatives.Add(RepresentativeIndex);
			continue;
		}

		Candidate->SuggestedName = Cached->Name;
		Candidate->Description = Cached->Description;
		Candidate->Confidence = FMath::Clamp(Cached->Confidence, 0.0f, 1.0f);
		Candidate->Source = ESceneAutoTaggerSource::GeometryCache;
		Candidate->bTagComplete = true;
		Candidate->Error.Reset();
		FanOutRepresentative(RepresentativeIndex, ESceneAutoTaggerSource::DuplicateGeometry);
		++CompletedRepresentatives;
	}
	PendingRepresentatives = MoveTemp(UncachedRepresentatives);
	Progress.Completed = CompletedRepresentatives;
	BroadcastChanged();

	NextRepresentative = 0;
	ProcessNextBatch();
}

void FSceneAutoTaggerController::ProcessNextBatch()
{
	if (bCancelRequested)
	{
		return;
	}
	if (!IsActiveEditorWorldCurrent())
	{
		AbortRun(
			TEXT("The editor level was closed or replaced during exploration."),
			LOCTEXT("WorldClosed", "Exploration stopped because the level is no longer available."));
		return;
	}
	if (NextRepresentative >= PendingRepresentatives.Num())
	{
		bCaptureComplete = true;
		ActiveBatch.Reset();
		ConvaiSceneAutoTagger::CleanupCaptureResources();
		if (!PendingAnalysisBatches.IsEmpty())
		{
			SetState(
				ESceneAutoTaggerState::Tagging,
				LOCTEXT("TaggingPreparedBatches", "Analyzing scene objects in parallel..."));
		}
		PumpAnalysisQueue();
		TryFinishExploration();
		return;
	}

	const int32 Capacity = FMath::Square(GetRunAnalysisGridDimension());
	TArray<int32> RemainingTargets;
	for (int32 Index = NextRepresentative; Index < PendingRepresentatives.Num(); ++Index)
	{
		RemainingTargets.Add(PendingRepresentatives[Index]);
	}
	TArray<int32> ContextIndices;
	if (ReviewBatchMode == EReviewBatchMode::FreshCapture)
	{
		TSet<int32> Targets;
		Targets.Reserve(PendingRepresentatives.Num());
		for (const int32 CandidateIndex : PendingRepresentatives)
		{
			Targets.Add(CandidateIndex);
		}
		for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
		{
			const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
			const int32 Resolution = Candidate.IsValid() ? Candidate->PreviewSize.X : 0;
			if (!Targets.Contains(CandidateIndex) && Candidate.IsValid()
				&& !Candidate->bDismissedFromReview && Candidate->Actor.IsValid()
				&& Candidate->Actor->GetWorld() == ActiveWorld.Get() && Resolution > 0
				&& Candidate->PreviewSize.Y == Resolution
				&& Candidate->PreviewBGRA.Num()
					== Resolution * Resolution * static_cast<int32>(sizeof(FColor)))
			{
				ContextIndices.Add(CandidateIndex);
			}
		}
	}
	TArray<FContextBatchSelection> PlannedBatches;
	FString PlanError;
	if (!BuildContextBatchPlan(Candidates, RemainingTargets, ContextIndices, Capacity,
		ReviewBatchMode == EReviewBatchMode::FreshCapture, PlannedBatches, PlanError)
		|| PlannedBatches.IsEmpty())
	{
		AbortRun(
			FString::Printf(TEXT("Spatial batch planning failed: %s"), *PlanError),
			LOCTEXT("SpatialBatchPlanFailed", "Exploration stopped because spatial batching could not be verified."));
		return;
	}
	FContextBatchSelection Batch = MoveTemp(PlannedBatches[0]);
	NextRepresentative += Batch.TargetIndices.Num();
	CaptureBatch(Batch.TargetIndices, Batch.ContextIndices);
}

void FSceneAutoTaggerController::ScheduleNextCaptureBatch()
{
	const FGuid CaptureRunId = ActiveRunId;
	const TWeakPtr<FSceneAutoTaggerController> WeakController = AsShared();
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakController, CaptureRunId](float)
		{
			if (const TSharedPtr<FSceneAutoTaggerController> Self = WeakController.Pin())
			{
				if (Self->ActiveRunId == CaptureRunId && !Self->bCancelRequested)
				{
					Self->ProcessNextBatch();
				}
			}
			return false;
		}),
		CaptureYieldDelaySeconds);
}

void FSceneAutoTaggerController::CaptureBatch(
	const TArray<int32>& RepresentativeIndices,
	const TArray<int32>& ContextIndices)
{
	ActiveBatch = MakeShared<FActiveBatch>();
	ActiveBatch->RequestedRepresentativeIndices = RepresentativeIndices;
	ActiveBatch->PlannedContextIndices = ContextIndices;
	ActiveBatch->CaptureStartedAtSeconds = FPlatformTime::Seconds();
	SetState(
		ESceneAutoTaggerState::Capturing,
		LOCTEXT("CaptureBatch", "Preparing object views..."));
	CaptureNextCell(ActiveRunId);
}

void FSceneAutoTaggerController::CaptureNextCell(FGuid CaptureRunId)
{
	if (CaptureRunId != ActiveRunId || bCancelRequested || !ActiveBatch)
	{
		return;
	}
	if (!IsActiveEditorWorldCurrent())
	{
		AbortRun(
			TEXT("The editor level was closed or replaced during capture."),
			LOCTEXT("WorldClosedDuringCapture", "Capture stopped because the level is no longer available."));
		return;
	}
	if (!ActiveBatch->RequestedRepresentativeIndices.IsValidIndex(ActiveBatch->CaptureCursor))
	{
		FinishCaptureBatch();
		return;
	}

	const int32 RepresentativeIndex =
		ActiveBatch->RequestedRepresentativeIndices[ActiveBatch->CaptureCursor];
	const int32 InitiallyCompleted = FMath::Max(0, Progress.Total - PendingRepresentatives.Num());
	const int32 CurrentBatchStart = FMath::Max(
		0,
		NextRepresentative - ActiveBatch->RequestedRepresentativeIndices.Num());
	const int32 PreparationOrdinal = FMath::Clamp(
		InitiallyCompleted + CurrentBatchStart + ActiveBatch->CaptureCursor + 1,
		1,
		FMath::Max(1, Progress.Total));
	SetState(
		ESceneAutoTaggerState::Capturing,
		FText::Format(
			LOCTEXT("PreparingViewProgress", "Preparing view {0} of {1}..."),
			FText::AsNumber(PreparationOrdinal),
			FText::AsNumber(Progress.Total)));
	TSharedPtr<FSceneAutoTaggerCandidate> Candidate = Candidates.IsValidIndex(RepresentativeIndex)
		? Candidates[RepresentativeIndex]
		: nullptr;
	if (Candidate.IsValid())
	{
		TArray<UPrimitiveComponent*> Components = ResolvePrimitiveComponents(*Candidate);
		const FBox& ObjectBounds = Candidate->WorldBounds;

		const int32 CellResolution = GetRunAnalysisCellResolution();
		const int32 CaptureResolution = FMath::Max(
			CellResolution,
			GetRunEvidencePreviewResolution());
		if (ActiveBatch->CurrentProbeLookDirections.IsEmpty())
		{
			// Prime only the actor currently being captured. Texture and mesh requests
			// are asynchronous; the winning review view below remains alive while their
			// conventional render-resource state settles.
			constexpr float CapturePrestreamSeconds = 2.0f;
			if (AActor* Actor = Candidate->Actor.Get())
			{
				RequestCaptureStreaming(
					Components,
					CapturePrestreamSeconds,
					false,
					ActiveBatch->CurrentStreamingAssets,
					ActiveBatch->CurrentStreamingMaterials);
				ActiveBatch->CurrentProbeViewPlan =
					ConvaiSceneAutoTagger::BuildActorLocalViewPlan(*Actor, Components);
				const ConvaiSceneAutoTagger::FSceneAutoTaggerViewPlan& ViewPlan =
					ActiveBatch->CurrentProbeViewPlan;
				Candidate->bOpposedBroadFacePresentation =
					ViewPlan.bOpposedBroadFacePresentation;
				Candidate->CaptureLightingPolicy = ViewPlan.CaptureLightingPolicy;
				for (int32 ViewIndex = 0; ViewIndex < ViewPlan.CameraOffsetsActorLocal.Num(); ++ViewIndex)
				{
					const FVector& CameraOffsetActorLocal = ViewPlan.CameraOffsetsActorLocal[ViewIndex];
					const FVector CameraOffsetWorld = Actor->GetActorQuat().RotateVector(CameraOffsetActorLocal).GetSafeNormal();
					// A degenerate direction keeps its plan-aligned slot; it is never
					// captured, so its probe stays absent for SelectCapturedView.
					ActiveBatch->CurrentProbeLookDirections.Add(
						!CameraOffsetWorld.IsNearlyZero() && !CameraOffsetWorld.ContainsNaN()
							? -CameraOffsetWorld
							: FVector::ZeroVector);
				}
			}
			bool bHasUsableProbeDirection = false;
			for (const FVector& LookDirection : ActiveBatch->CurrentProbeLookDirections)
			{
				bHasUsableProbeDirection |= !LookDirection.IsNearlyZero();
			}
			if (!bHasUsableProbeDirection)
			{
				const FVector FallbackLook = FVector(-1.0, -1.0, -0.65).GetSafeNormal();
				AActor* Actor = Candidate->Actor.Get();
				ActiveBatch->CurrentProbeLookDirections.Reset();
				ActiveBatch->CurrentProbeLookDirections.Add(FallbackLook);
				ActiveBatch->CurrentProbeViewPlan.CameraOffsetsActorLocal.Reset();
				ActiveBatch->CurrentProbeViewPlan.FrontPreferences.Reset();
				ActiveBatch->CurrentProbeViewPlan.ShapeProfileCandidates.Reset();
				ActiveBatch->CurrentProbeViewPlan.CameraOffsetsActorLocal.Add(
					Actor ? Actor->GetActorQuat().UnrotateVector(-FallbackLook) : -FallbackLook);
				ActiveBatch->CurrentProbeViewPlan.FrontPreferences.Add(0.0f);
				ActiveBatch->CurrentProbeViewPlan.ShapeProfileCandidates.Add(false);
			}
			ActiveBatch->CurrentProbeImages.Reserve(ActiveBatch->CurrentProbeLookDirections.Num());
			ActiveBatch->CurrentProbeSeededViews.Init(false, ActiveBatch->CurrentProbeLookDirections.Num());
			ActiveBatch->CurrentProbeWarmupTicks = 0;
			ActiveBatch->CurrentProbeLastObservedFrame = GFrameCounter;
			ActiveBatch->CurrentProbeWarmupStartedAtSeconds = FPlatformTime::Seconds();
			ActiveBatch->bCurrentProbeWarmupComplete = false;
		}

		const auto ScheduleCaptureTick = [this, CaptureRunId]()
		{
			const TWeakPtr<FSceneAutoTaggerController> WeakController = AsShared();
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([WeakController, CaptureRunId](float)
				{
					if (const TSharedPtr<FSceneAutoTaggerController> Self = WeakController.Pin())
					{
						Self->CaptureNextCell(CaptureRunId);
					}
					return false;
				}),
				CaptureYieldDelaySeconds);
		};
		const auto ResetProbeState = [this]()
		{
			ActiveBatch->CurrentProbeLookDirections.Reset();
			ActiveBatch->CurrentProbeViewPlan = ConvaiSceneAutoTagger::FSceneAutoTaggerViewPlan();
			ActiveBatch->CurrentProbeImages.Reset();
			ActiveBatch->CurrentProbeMetrics.Reset();
			ReleaseCaptureStreaming(ActiveBatch->CurrentStreamingAssets);
			ActiveBatch->CurrentStreamingMaterials.Reset();
			ActiveBatch->CurrentProbeError.Reset();
			if (ActiveBatch->CurrentFinalCaptureSession)
			{
				ActiveBatch->CurrentFinalCaptureSession->End();
				ActiveBatch->CurrentFinalCaptureSession.Reset();
			}
			ActiveBatch->CurrentProbeCursor = 0;
			ActiveBatch->bCurrentProbeSeedingPass = true;
			ActiveBatch->bCurrentProbeViewPrimed = false;
			ActiveBatch->bCurrentProbePrimeSucceeded = false;
			ActiveBatch->CurrentProbeSeededViews.Reset();
			ActiveBatch->CurrentProbeWarmupTicks = 0;
			ActiveBatch->CurrentProbeLastObservedFrame = MAX_uint64;
			ActiveBatch->CurrentProbeWarmupStartedAtSeconds = 0.0;
			ActiveBatch->bCurrentProbeWarmupComplete = false;
			ActiveBatch->SelectedProbeIndex = INDEX_NONE;
			ActiveBatch->CurrentFinalWarmupTicks = 0;
			ActiveBatch->CurrentFinalLastObservedFrame = MAX_uint64;
			ActiveBatch->CurrentFinalWarmupStartedAtSeconds = 0.0;
			ActiveBatch->bCurrentFinalCaptureCalibrated = false;
		};

		if (ActiveBatch->SelectedProbeIndex == INDEX_NONE
			&& !ActiveBatch->bCurrentProbeWarmupComplete)
		{
			if (ActiveBatch->CurrentProbeLastObservedFrame == GFrameCounter)
			{
				ScheduleCaptureTick();
				return;
			}
			ActiveBatch->CurrentProbeLastObservedFrame = GFrameCounter;
			++ActiveBatch->CurrentProbeWarmupTicks;
			const FCaptureStreamingReadiness Readiness = InspectCaptureStreamingReadiness(
				ActiveBatch->CurrentStreamingAssets,
				ActiveBatch->CurrentStreamingMaterials);
			const int32 PendingRenderWork = Readiness.PendingAssets
				+ Readiness.CompilingMaterials;
			const double ElapsedSeconds = FPlatformTime::Seconds()
				- ActiveBatch->CurrentProbeWarmupStartedAtSeconds;
			const bool bRepeatedPrimedView = !ActiveBatch->bCurrentProbeSeedingPass
				&& ActiveBatch->bCurrentProbeViewPrimed
				&& ActiveBatch->bCurrentProbePrimeSucceeded
				&& ActiveBatch->CurrentProbeSeededViews.IsValidIndex(ActiveBatch->CurrentProbeCursor)
				&& ActiveBatch->CurrentProbeSeededViews[ActiveBatch->CurrentProbeCursor];
			const bool bNearbyInitialGate = ActiveBatch->bCurrentProbeSeedingPass
				&& !ActiveBatch->bCurrentProbeViewPrimed
				&& ActiveBatch->CurrentProbeCursor == 0
				&& IsRecentNearbyCapture(
					ActiveBatch->LastCompletedCaptureSeconds, FPlatformTime::Seconds(),
					ActiveBatch->LastCompletedCaptureCenter, ObjectBounds.GetCenter());
			const bool bUseWarmGate = CanUseWarmProbeGate(
				Readiness,
				(bRepeatedPrimedView || bNearbyInitialGate)
					&& HasKnownConventionalCaptureResources(
						Components, ActiveBatch->CurrentStreamingAssets, ActiveBatch->CurrentStreamingMaterials),
				bRepeatedPrimedView, bNearbyInitialGate);
			if (ShouldContinueProbeGate(
				ActiveBatch->CurrentProbeWarmupTicks,
				ElapsedSeconds,
				PendingRenderWork,
				bUseWarmGate))
			{
				ScheduleCaptureTick();
				return;
			}

			ActiveBatch->bCurrentProbeWarmupComplete = true;
			UE_LOG(
				LogConvaiSceneAutoTaggerController,
				Verbose,
				TEXT("Probe warm-up '%s': frames=%d elapsed=%.3fs pendingAssets=%d compilingMaterials=%d gate=%s."),
				*Candidate->ActorLabel,
				ActiveBatch->CurrentProbeWarmupTicks,
				ElapsedSeconds,
				Readiness.PendingAssets,
				Readiness.CompilingMaterials,
				bUseWarmGate ? (bRepeatedPrimedView ? TEXT("warm-repeat") : TEXT("warm-nearby")) : TEXT("conservative"));
		}

		if (ActiveBatch->SelectedProbeIndex != INDEX_NONE)
		{
			const int32 BestViewIndex = ActiveBatch->SelectedProbeIndex;
			TArray<FColor> BestPixels;
			FString CaptureError;
			const bool bHasSelectedDirection =
				ActiveBatch->CurrentProbeLookDirections.IsValidIndex(BestViewIndex);
			if (bHasSelectedDirection && !ActiveBatch->CurrentFinalCaptureSession)
			{
				// The early request may expire while candidate-angle probes run. Refresh
				// the exact winning actor now so its highest runtime-allowed mesh LODs and
				// texture mips remain requested through calibration and retained readback.
				constexpr float FinalCapturePrestreamSeconds = 3.0f;
				RequestCaptureStreaming(
					Components,
					FinalCapturePrestreamSeconds,
					true,
					ActiveBatch->CurrentStreamingAssets,
					ActiveBatch->CurrentStreamingMaterials);
				ActiveBatch->CurrentFinalCaptureSession =
					ConvaiSceneAutoTagger::BeginLiveObjectCapture(
						ActiveWorld.Get(),
						Components,
				ObjectBounds,
						CaptureResolution,
						CaptureError,
						ActiveBatch->CurrentProbeLookDirections[BestViewIndex],
						1.0f,
						FVector2D::ZeroVector,
						ConvaiSceneAutoTagger::ShouldSuppressDirectSpecularHighlights(
							Candidate->CaptureLightingPolicy),
						ConvaiSceneAutoTagger::ShouldAllowDirectSpecularRecovery(
							Candidate->CaptureLightingPolicy),
						false);
				if (ActiveBatch->CurrentFinalCaptureSession)
				{
					ActiveBatch->CurrentFinalWarmupTicks = 0;
					ActiveBatch->CurrentFinalLastObservedFrame = GFrameCounter;
					ActiveBatch->CurrentFinalWarmupStartedAtSeconds = FPlatformTime::Seconds();
					ActiveBatch->bCurrentFinalCaptureCalibrated = false;
					ScheduleCaptureTick();
					return;
				}
			}

			if (ActiveBatch->CurrentFinalCaptureSession)
			{
				// Texture IO, mesh LOD installation, virtual-texture feedback, and Nanite
				// pages advance only across real editor frames. Keep the exact winning
				// review view alive until conventional assets settle, with bounded extra
				// frames for resources that expose no per-view completion signal.
				if (ActiveBatch->CurrentFinalLastObservedFrame == GFrameCounter)
				{
					ScheduleCaptureTick();
					return;
				}
				ActiveBatch->CurrentFinalLastObservedFrame = GFrameCounter;
				++ActiveBatch->CurrentFinalWarmupTicks;

				if (!ActiveBatch->bCurrentFinalCaptureCalibrated)
				{
					const FCaptureStreamingReadiness Readiness =
						InspectCaptureStreamingReadiness(
							ActiveBatch->CurrentStreamingAssets,
							ActiveBatch->CurrentStreamingMaterials);
					const double ElapsedSeconds = FPlatformTime::Seconds()
						- ActiveBatch->CurrentFinalWarmupStartedAtSeconds;
					if (ConvaiSceneAutoTagger::ShouldContinueAutomaticCaptureWarmup(
						ActiveBatch->CurrentFinalWarmupTicks,
						ElapsedSeconds,
						Readiness.PendingAssets + Readiness.CompilingMaterials))
					{
						ScheduleCaptureTick();
						return;
					}

					UE_LOG(
						LogConvaiSceneAutoTaggerController,
						Verbose,
						TEXT("Capture warm-up '%s': frames=%d elapsed=%.3fs assets=%d conventionalFull=%d/%d pending=%d virtual=%d materials=%d compiling=%d completion=%s."),
						*Candidate->ActorLabel,
						ActiveBatch->CurrentFinalWarmupTicks,
						ElapsedSeconds,
						Readiness.TrackedAssets,
						Readiness.FullyResidentAssets,
						Readiness.ConventionalAssets,
						Readiness.PendingAssets,
						Readiness.VirtualAssets,
						Readiness.TrackedMaterials,
						Readiness.CompilingMaterials,
						Readiness.PendingAssets == 0 && Readiness.CompilingMaterials == 0
							? TEXT("ready")
							: TEXT("bounded"));

					if (!ActiveBatch->CurrentFinalCaptureSession->CalibrateReadability(CaptureError))
					{
						ActiveBatch->CurrentFinalCaptureSession->End();
						ActiveBatch->CurrentFinalCaptureSession.Reset();
					}
					else
					{
						ActiveBatch->bCurrentFinalCaptureCalibrated = true;
						ActiveBatch->CurrentFinalWarmupTicks = 0;
						ActiveBatch->CurrentFinalLastObservedFrame = GFrameCounter;
						ScheduleCaptureTick();
						return;
					}
				}
				else if (!ConvaiSceneAutoTagger::HasAutomaticCaptureSettledAfterCalibration(
					ActiveBatch->CurrentFinalWarmupTicks))
				{
					ScheduleCaptureTick();
					return;
				}
			}

			const bool bCaptured = ActiveBatch->CurrentFinalCaptureSession
				? ActiveBatch->CurrentFinalCaptureSession->ReadCurrentPixels(BestPixels, CaptureError)
				: bHasSelectedDirection && ConvaiSceneAutoTagger::CaptureObjectCell(
					ActiveWorld.Get(),
					Components,
					ObjectBounds,
					CaptureResolution,
					BestPixels,
					CaptureError,
					ActiveBatch->CurrentProbeLookDirections[BestViewIndex],
					1.0f,
					FVector2D::ZeroVector,
					ConvaiSceneAutoTagger::ShouldSuppressDirectSpecularHighlights(
						Candidate->CaptureLightingPolicy),
					ConvaiSceneAutoTagger::ShouldAllowDirectSpecularRecovery(
						Candidate->CaptureLightingPolicy));
			if (bCaptured)
			{
				ActiveBatch->LastCompletedCaptureCenter = ObjectBounds.GetCenter();
				ActiveBatch->LastCompletedCaptureSeconds = FPlatformTime::Seconds();
				BuildPreview(*Candidate, BestPixels, CaptureResolution);
				if (ReviewBatchMode == EReviewBatchMode::FreshCapture)
				{
					ReviewBatchExpectedPreviewRevisions.FindOrAdd(Candidate->ActorPath) =
						Candidate->PreviewRevision;
				}
				const FVector BestViewDirection = ActiveBatch->CurrentProbeLookDirections[BestViewIndex];
				Candidate->EvidenceViewDirectionActorLocal = Candidate->Actor.IsValid()
					? Candidate->Actor->GetActorQuat().UnrotateVector(BestViewDirection).GetSafeNormal()
					: BestViewDirection.GetSafeNormal();
				Candidate->EvidenceDistanceScale = 1.0f;
				Candidate->EvidenceTargetOffset = FVector2D::ZeroVector;
				ConvaiSceneAutoTagger::FSceneAutoTaggerCaptureFacts CaptureFacts;
				CaptureFacts.SignificanceScore = Candidate->SignificanceScore;
				CaptureFacts.PrimitiveCount = Candidate->PrimitiveComponents.Num();
				CaptureFacts.NativeSignificanceReasonFlags =
					BuildNativeSignificanceReasonFlags(Candidate->SignificanceReason);
				CaptureFacts.bSingleCandidateRequest = bSingleCandidateRequest;
				CaptureFacts.bReviewBatchRequest = IsReviewBatchRequest();
				// Explicit Selected Actors runs retain marginal evidence for review;
				// capture failures still surface through the normal Attention path.
				CaptureFacts.bForceExplicitScope = RunOptions.bForceAnalyzeScopedActors
					&& !RunOptions.ScopedActors.IsEmpty();
				CaptureFacts.bOpposedBroadFace = Candidate->bOpposedBroadFacePresentation;
				CaptureFacts.bHasExistingObject = Candidate->bHadConvaiComponent;
				// On evaluation failure the defaults retain the row, matching the
				// keep-uncertain-rows behavior of the retired local gate.
				ConvaiSceneAutoTagger::FSceneAutoTaggerCaptureEvaluation Evaluation;
				ConvaiSceneAutoTagger::EvaluateFinalCapture(
					Candidate->PreviewBGRA,
					Candidate->PreviewSize,
					ActiveBatch->CurrentProbeMetrics,
					CaptureFacts,
					Evaluation);
				if (Evaluation.bDescriptorValid)
				{
					EvidenceDescriptorsByActorPath.Add(
						Candidate->ActorPath,
						MakeTuple(Candidate->PreviewRevision, Evaluation.Descriptor));
				}
				else
				{
					EvidenceDescriptorsByActorPath.Remove(Candidate->ActorPath);
				}
				if (!Evaluation.bRetain)
				{
					const ESceneAutoTaggerLowInformationReason LowInformationReason =
						Evaluation.bExcludedForVisibility
							? ESceneAutoTaggerLowInformationReason::ObjectNotClearlyVisible
							: ESceneAutoTaggerLowInformationReason::NoUsefulVisualDetail;
					VisuallyRejectedActorPaths.Add(Candidate->ActorPath);
					VisuallyRejectedReasonsByActorPath.Add(
						Candidate->ActorPath,
						LowInformationReason);
					// Retain the representative capture on each geometric duplicate so any
					// skipped row the user restores can enter retained-evidence analysis.
					FanOutRepresentative(
						RepresentativeIndex,
						ESceneAutoTaggerSource::DuplicateGeometry);
					for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
					{
						if (Candidates[CandidateIndex].IsValid()
							&& Candidates[CandidateIndex]->DuplicateRepresentative == RepresentativeIndex)
						{
							VisuallyRejectedActorPaths.Add(Candidates[CandidateIndex]->ActorPath);
							VisuallyRejectedReasonsByActorPath.Add(
								Candidates[CandidateIndex]->ActorPath,
								LowInformationReason);
						}
					}
					UE_LOG(
						LogConvaiSceneAutoTaggerController,
						Display,
						TEXT("Excluded '%s' after retained-image review: local relevance %.0f%% but no readable centered object or authored interior evidence."),
						*Candidate->ActorLabel,
						Candidate->SignificanceScore * 100.0f);
					++CompletedRepresentatives;
					Progress.Completed = CompletedRepresentatives;
				}
				else
				{
					TArray<FColor> GridPixels = DownsampleSquareImage(
						BestPixels,
						CaptureResolution,
						CellResolution);
					ActiveBatch->CapturedRepresentativeIndices.Add(RepresentativeIndex);
					ActiveBatch->CapturedTargetFlags.Add(true);
					ActiveBatch->CapturedPreviewRevisions.Add(Candidate->PreviewRevision);
					ActiveBatch->CapturedCells.Add(MoveTemp(GridPixels));
					const FString EchoId = BuildEchoId(*Candidate, ActiveBatch->CapturedCells.Num());
					ActiveBatch->EchoIds.Add(EchoId);
					const bool bHasPreviousProposal = !Candidate->SuggestedName.IsEmpty()
						|| !Candidate->Description.IsEmpty();
					ActiveBatch->ObjectPromptContexts.Add(BuildObjectPromptContext(
						*Candidate,
						EchoId,
						true,
						false,
						bHasPreviousProposal));
					ActiveBatch->CapturedRefinementNoteRevisions.Add(Candidate->RefinementNoteRevision);
				}
			}
			else
			{
				ActiveBatch->LastCompletedCaptureSeconds = -1.0;
				const FString Failure = FString::Printf(
					TEXT("The selected evidence view could not be captured at review resolution: %s."),
					CaptureError.IsEmpty() ? TEXT("empty capture") : *CaptureError);
				if (IsReviewBatchRequest())
				{
					PreserveReviewBatchCandidate(RepresentativeIndex, Failure);
				}
				else
				{
					MarkRepresentativeFailed(
						RepresentativeIndex,
						Failure + TEXT(" Select Recapture and retry for this row."));
					if (!bSingleCandidateRequest)
					{
						FanOutRepresentative(RepresentativeIndex, ESceneAutoTaggerSource::DuplicateGeometry);
					}
					++CompletedRepresentatives;
				}
				Progress.Completed = CompletedRepresentatives;
			}
			ResetProbeState();
		}
		else
		{
			// Angle probes only need enough resolution to compare framing, color,
			// and mid-scale detail. Recapture the winner once at review resolution.
			// Rank angles at the same detail scale used by Fast model cells. A 192 px
			// probe can erase fine relief or authored texture detail and make a noisy
			// reverse face outrank the meaningful presentation face.
			constexpr int32 ProbeResolution =
				UConvaiSceneAutoTaggerSettings::DefaultCellResolution;
			TArray<FColor> ProbePixels;
			TArray<uint8> ProbeGeometryMask;
			FString CaptureError;
			const int32 ProbeIndex = ActiveBatch->CurrentProbeCursor;
			const auto CaptureProbe = [&](TArray<FColor>& OutPixels, FString& OutError, const bool bPrimeOnly)
			{
				return ActiveBatch->CurrentProbeLookDirections.IsValidIndex(ProbeIndex)
					&& !ActiveBatch->CurrentProbeLookDirections[ProbeIndex].IsNearlyZero()
					&& ConvaiSceneAutoTagger::CaptureObjectCell(
					ActiveWorld.Get(),
					Components,
					ObjectBounds,
					FMath::Min(CaptureResolution, ProbeResolution),
					OutPixels,
					OutError,
					ActiveBatch->CurrentProbeLookDirections[ProbeIndex],
					1.0f,
					FVector2D::ZeroVector,
					ConvaiSceneAutoTagger::ShouldSuppressDirectSpecularHighlights(
						Candidate->CaptureLightingPolicy),
					ConvaiSceneAutoTagger::ShouldAllowDirectSpecularRecovery(
						Candidate->CaptureLightingPolicy),
					bPrimeOnly,
					bPrimeOnly ? nullptr : &ProbeGeometryMask);
			};
			if (!ActiveBatch->bCurrentProbeViewPrimed)
			{
				// The asset/material gate cannot trigger view-dependent shader or VT
				// work. Prime this exact direction, discard that cold draw, then reopen
				// the bounded gate before scoring it. This keeps a side profile from
				// being systematically colder than an already-rendered front.
				TArray<FColor> PrimingPixels;
				FString PrimingError;
				ActiveBatch->bCurrentProbePrimeSucceeded = CaptureProbe(PrimingPixels, PrimingError, true);
				if (ActiveBatch->bCurrentProbeSeedingPass)
				{
					ActiveBatch->CurrentProbeSeededViews[ProbeIndex] = ActiveBatch->bCurrentProbePrimeSucceeded;
				}
				ActiveBatch->bCurrentProbeViewPrimed = true;
				ActiveBatch->CurrentProbeWarmupTicks = 0;
				ActiveBatch->CurrentProbeLastObservedFrame = GFrameCounter;
				// Every seeding view keeps the conservative gate. Only a successfully
				// seeded, re-primed scoring view with proven conventional residency may
				// omit the elapsed-time floor; it still crosses three real frames.
				ActiveBatch->CurrentProbeWarmupStartedAtSeconds =
					FPlatformTime::Seconds();
				ActiveBatch->bCurrentProbeWarmupComplete = false;
				ScheduleCaptureTick();
				return;
			}

			if (ActiveBatch->bCurrentProbeSeedingPass)
			{
				// Direction-dependent virtual-texture, Nanite, and shader work cannot
				// always report a useful pending count. Visit every angle once before
				// any score is trusted, then revisit the same ordered candidates for
				// measurement. No image content or actor identity affects this policy.
				ActiveBatch->bCurrentProbeViewPrimed = false;
				const bool bWasSeeding = ActiveBatch->bCurrentProbeSeedingPass;
				ConvaiSceneAutoTagger::AdvanceAutomaticProbePass(
					ActiveBatch->CurrentProbeLookDirections.Num(),
					ActiveBatch->CurrentProbeCursor,
					ActiveBatch->bCurrentProbeSeedingPass);
				if (bWasSeeding && !ActiveBatch->bCurrentProbeSeedingPass)
				{
					UE_LOG(
						LogConvaiSceneAutoTaggerController,
						Verbose,
						TEXT("Completed unscored angle warm-up for '%s' across %d views."),
						*Candidate->ActorLabel,
						ActiveBatch->CurrentProbeLookDirections.Num());
				}
				ScheduleCaptureTick();
				return;
			}
			const bool bCaptured = CaptureProbe(ProbePixels, CaptureError, false);
			if (bCaptured)
			{
				const int32 ActualProbeResolution = FMath::RoundToInt(FMath::Sqrt(static_cast<float>(ProbePixels.Num())));
				if (GetDefault<UConvaiSceneAutoTaggerSettings>()->bSaveDebugCaptures)
				{
					const FString DebugPath = FPaths::Combine(
						FPaths::ProjectSavedDir(),
						TEXT("ConvaiSceneAutoTagger"),
						TEXT("Captures"),
						TEXT("Probes"),
						FString::Printf(
							TEXT("Probe_%s_%s_%02d.png"),
							*FPaths::MakeValidFileName(Candidate->ActorLabel),
							*ActiveRunId.ToString(EGuidFormats::Digits),
							ProbeIndex + 1));
					FString DebugSaveError;
					if (!ConvaiSceneAutoTagger::SavePixelsPngDebug(
						DebugPath,
						ProbePixels,
						ActualProbeResolution,
						ActualProbeResolution,
						DebugSaveError))
					{
						UE_LOG(
							LogConvaiSceneAutoTaggerController,
							Warning,
							TEXT("Could not save view probe for '%s': %s"),
							*Candidate->ActorLabel,
							*DebugSaveError);
					}
				}
				ConvaiSceneAutoTagger::FSceneAutoTaggerProbeImage& Probe =
					ActiveBatch->CurrentProbeImages.AddDefaulted_GetRef();
				Probe.bPresent = true;
				Probe.Width = ActualProbeResolution;
				Probe.Height = ActualProbeResolution;
				Probe.Background = ProbePixels[0];
				Probe.GeometryMask = MoveTemp(ProbeGeometryMask);
				Probe.Pixels = MoveTemp(ProbePixels);
			}
			else
			{
				// The absent slot keeps plan alignment; the core reports it bValid=false.
				ActiveBatch->CurrentProbeImages.AddDefaulted();
				ActiveBatch->CurrentProbeError = CaptureError;
			}

			ActiveBatch->bCurrentProbeViewPrimed = false;
			const bool bScoringComplete = ConvaiSceneAutoTagger::AdvanceAutomaticProbePass(
				ActiveBatch->CurrentProbeLookDirections.Num(),
				ActiveBatch->CurrentProbeCursor,
				ActiveBatch->bCurrentProbeSeedingPass);
			if (!bScoringComplete)
			{
				ScheduleCaptureTick();
				return;
			}

			ConvaiSceneAutoTagger::FSceneAutoTaggerSelectViewResult ViewSelection;
			if (!ConvaiSceneAutoTagger::SelectCapturedView(
				ActiveBatch->CurrentProbeViewPlan,
				ActiveBatch->CurrentProbeImages,
				ViewSelection))
			{
				ViewSelection.SelectedIndex = INDEX_NONE;
			}
			ActiveBatch->CurrentProbeMetrics = MoveTemp(ViewSelection.Metrics);
			ActiveBatch->SelectedProbeIndex = ViewSelection.SelectedIndex;
			for (int32 MetricsIndex = 0; MetricsIndex < ActiveBatch->CurrentProbeMetrics.Num(); ++MetricsIndex)
			{
				const ConvaiSceneAutoTagger::FSceneAutoTaggerViewMetrics& Metrics =
					ActiveBatch->CurrentProbeMetrics[MetricsIndex];
				const FVector ProbeDirection =
					ActiveBatch->CurrentProbeLookDirections.IsValidIndex(MetricsIndex)
						? ActiveBatch->CurrentProbeLookDirections[MetricsIndex]
						: FVector::ZeroVector;
				UE_LOG(
					LogConvaiSceneAutoTaggerController,
					Verbose,
					TEXT("View probe '%s' %d/%d direction=(%.4f, %.4f, %.4f): foreground=%.4f silhouette=%.4f centralFill=%.4f centralTone=%.4f centralColor=%.4f centralEdge=%.4f authored=%.4f globalColor=%.4f globalEdge=%.4f quality=%.4f valid=%s"),
					*Candidate->ActorLabel,
					MetricsIndex + 1,
					ActiveBatch->CurrentProbeMetrics.Num(),
					ProbeDirection.X,
					ProbeDirection.Y,
					ProbeDirection.Z,
					Metrics.ForegroundFraction,
					Metrics.SilhouetteInformation,
					Metrics.CentralForegroundFill,
					Metrics.CentralToneEntropy,
					Metrics.CentralColorEntropy,
					Metrics.CentralEdgeDetail,
					Metrics.CentralAuthoredContent,
					Metrics.ColorEntropy,
					Metrics.EdgeDetail,
					Metrics.CaptureQuality,
					Metrics.bValid ? TEXT("true") : TEXT("false"));
			}
			UE_LOG(
				LogConvaiSceneAutoTaggerController,
				Verbose,
				TEXT("Selected view probe '%s': %d of %d (opposedBroadFace=%s)"),
				*Candidate->ActorLabel,
				ActiveBatch->SelectedProbeIndex + 1,
				ActiveBatch->CurrentProbeImages.Num(),
				ActiveBatch->CurrentProbeViewPlan.bOpposedBroadFacePresentation ? TEXT("true") : TEXT("false"));
			if (ActiveBatch->SelectedProbeIndex != INDEX_NONE)
			{
				ScheduleCaptureTick();
				return;
			}

			const FString Failure = FString::Printf(
				TEXT("No informative capture view was available: %s."),
				ActiveBatch->CurrentProbeError.IsEmpty() ? TEXT("all candidate views were empty") : *ActiveBatch->CurrentProbeError);
			if (IsReviewBatchRequest())
			{
				PreserveReviewBatchCandidate(RepresentativeIndex, Failure);
			}
			else
			{
				MarkRepresentativeFailed(
					RepresentativeIndex,
					Failure + TEXT(" Select Recapture and retry for this row."));
				if (!bSingleCandidateRequest)
				{
					FanOutRepresentative(RepresentativeIndex, ESceneAutoTaggerSource::DuplicateGeometry);
				}
				++CompletedRepresentatives;
			}
			Progress.Completed = CompletedRepresentatives;
			ResetProbeState();
		}
	}
	else if (IsReviewBatchRequest())
	{
		PreserveReviewBatchCandidate(
			RepresentativeIndex,
			TEXT("A selected actor was no longer available for recapture; its previous review was kept."));
		Progress.Completed = CompletedRepresentatives;
	}

	++ActiveBatch->CaptureCursor;
	BroadcastChanged();
	if (ActiveBatch->CaptureCursor >= ActiveBatch->RequestedRepresentativeIndices.Num())
	{
		FinishCaptureBatch();
		return;
	}

	const TWeakPtr<FSceneAutoTaggerController> WeakController = AsShared();
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakController, CaptureRunId](float)
		{
			if (const TSharedPtr<FSceneAutoTaggerController> Self = WeakController.Pin())
			{
				Self->CaptureNextCell(CaptureRunId);
			}
			return false;
		}),
		CaptureYieldDelaySeconds);
}

void FSceneAutoTaggerController::FinishCaptureBatch()
{
	if (!ActiveBatch || bCancelRequested)
	{
		return;
	}
	if (ActiveBatch->CapturedCells.IsEmpty())
	{
		ActiveBatch.Reset();
		if (bSingleCandidateRequest)
		{
			const FString CaptureFailure = Candidates.IsValidIndex(SingleCandidateIndex)
				&& Candidates[SingleCandidateIndex].IsValid()
				? Candidates[SingleCandidateIndex]->Error
				: TEXT("No usable new image was captured.");
			const bool bRestoredPrevious = RestoreSingleCandidateRecaptureSnapshot();
			if (bRestoredPrevious)
			{
				LastError = FString::Printf(
					TEXT("Recapture failed: %s The previous image, suggestion, and review state were kept."),
					*CaptureFailure);
			}
			bCaptureComplete = true;
			Progress.Completed = 1;
			bSingleCandidateRequest = false;
			SingleCandidateIndex = INDEX_NONE;
			ConvaiSceneAutoTagger::CleanupCaptureResources();
			SetState(
				ESceneAutoTaggerState::ReviewReady,
				bRestoredPrevious
					? LOCTEXT("SingleRecaptureUnavailablePreserved", "No usable new image was captured. The previous review was kept.")
					: LOCTEXT("SingleRecaptureUnavailable", "No usable image was captured. Adjust the object or retry this row."));
			return;
		}
		ScheduleNextCaptureBatch();
		return;
	}

	const int32 CellCapacity = FMath::Square(GetRunAnalysisGridDimension());
	const int32 ContextCellResolution = GetRunAnalysisCellResolution();
	for (const int32 ContextIndex : ActiveBatch->PlannedContextIndices)
	{
		if (ActiveBatch->CapturedCells.Num() >= CellCapacity)
		{
			break;
		}
		const TSharedPtr<FSceneAutoTaggerCandidate> ContextCandidate =
			Candidates.IsValidIndex(ContextIndex) ? Candidates[ContextIndex] : nullptr;
		const int32 Resolution = ContextCandidate.IsValid()
			? ContextCandidate->PreviewSize.X
			: 0;
		if (!ContextCandidate.IsValid()
			|| Resolution <= 0
			|| ContextCandidate->PreviewSize.Y != Resolution
			|| ContextCandidate->PreviewBGRA.Num()
				!= Resolution * Resolution * static_cast<int32>(sizeof(FColor)))
		{
			continue;
		}
		TArray<FColor> FullPixels;
		FullPixels.SetNumUninitialized(Resolution * Resolution);
		FMemory::Memcpy(
			FullPixels.GetData(),
			ContextCandidate->PreviewBGRA.GetData(),
			ContextCandidate->PreviewBGRA.Num());
		TArray<FColor> CellPixels = DownsampleSquareImage(
			FullPixels,
			Resolution,
			ContextCellResolution);
		if (CellPixels.Num() != ContextCellResolution * ContextCellResolution)
		{
			continue;
		}
		ActiveBatch->CapturedRepresentativeIndices.Add(ContextIndex);
		ActiveBatch->CapturedTargetFlags.Add(false);
		ActiveBatch->CapturedPreviewRevisions.Add(ContextCandidate->PreviewRevision);
		ActiveBatch->CapturedCells.Add(MoveTemp(CellPixels));
		const FString EchoId = BuildEchoId(*ContextCandidate, ActiveBatch->CapturedCells.Num());
		ActiveBatch->EchoIds.Add(EchoId);
		ActiveBatch->ObjectPromptContexts.Add(
			BuildObjectPromptContext(*ContextCandidate, EchoId, false, true, true));
		ActiveBatch->CapturedRefinementNoteRevisions.Add(0u);
	}

	const int32 GridDimension = bSingleCandidateRequest
		? 1
		: GetRunAnalysisGridDimension();
	const int32 CellResolution = GetRunAnalysisCellResolution();
	FString CompositeError;
	const bool bComposited = ConvaiSceneAutoTagger::CompositeGridPng(
		ActiveBatch->CapturedCells,
		GridDimension,
		CellResolution,
		GridDimension * CellResolution,
		ActiveBatch->GridPngBytes,
		CompositeError);
	if (!bComposited || ActiveBatch->GridPngBytes.IsEmpty())
	{
		const FString Error = FString::Printf(TEXT("Contact sheet generation failed: %s"), *CompositeError);
		AppendWarning(LastError, Error);
		for (int32 CellIndex = 0;
			CellIndex < ActiveBatch->CapturedRepresentativeIndices.Num();
			++CellIndex)
		{
			if (!ActiveBatch->IsTargetCell(CellIndex))
			{
				continue;
			}
			const int32 RepresentativeIndex = ActiveBatch->CapturedRepresentativeIndices[CellIndex];
			if (Candidates.IsValidIndex(RepresentativeIndex) && Candidates[RepresentativeIndex].IsValid())
			{
				if (IsReviewBatchRequest())
				{
					PreserveReviewBatchCandidate(RepresentativeIndex, Error);
				}
				else
				{
					MarkRepresentativeFailed(RepresentativeIndex, Error);
					if (!bSingleCandidateRequest)
					{
						FanOutRepresentative(RepresentativeIndex, ESceneAutoTaggerSource::DuplicateGeometry);
					}
					++CompletedRepresentatives;
				}
			}
		}
		Progress.Completed = CompletedRepresentatives;
		ActiveBatch.Reset();
		BroadcastChanged();
		if (bSingleCandidateRequest)
		{
			const bool bRestoredPrevious = RestoreSingleCandidateRecaptureSnapshot();
			if (bRestoredPrevious)
			{
				LastError = FString::Printf(
					TEXT("Recapture failed: %s The previous image, suggestion, and review state were kept."),
					*Error);
			}
			bCaptureComplete = true;
			Progress.Completed = 1;
			bSingleCandidateRequest = false;
			SingleCandidateIndex = INDEX_NONE;
			ConvaiSceneAutoTagger::CleanupCaptureResources();
			SetState(
				ESceneAutoTaggerState::ReviewReady,
				bRestoredPrevious
					? LOCTEXT("SingleRecaptureEncodingFailedPreserved", "The new image could not be prepared. The previous review was kept.")
					: LOCTEXT("SingleRecaptureEncodingFailed", "The new image could not be prepared. This object can be retried."));
			return;
		}
		ScheduleNextCaptureBatch();
		return;
	}

	UE_LOG(
		LogConvaiSceneAutoTaggerController,
		Display,
		TEXT("Prepared one model contact sheet: %d object(s), %dx%d, %.1f KiB in %.2f s. High-resolution review evidence remains local."),
		ActiveBatch->CapturedRepresentativeIndices.Num(),
		GridDimension * CellResolution,
		GridDimension * CellResolution,
		static_cast<double>(ActiveBatch->GridPngBytes.Num()) / 1024.0,
		FPlatformTime::Seconds() - ActiveBatch->CaptureStartedAtSeconds);

	const UConvaiSceneAutoTaggerSettings* Settings =
		GetDefault<UConvaiSceneAutoTaggerSettings>();
	if (Settings->bSaveDebugCaptures && !ActiveBatch->GridPngBytes.IsEmpty())
	{
		const FString FilePath = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("ConvaiSceneAutoTagger"),
			TEXT("Captures"),
			FString::Printf(TEXT("Grid_%s.png"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		FString DebugSaveError;
		if (!ConvaiSceneAutoTagger::SavePngDebug(FilePath, ActiveBatch->GridPngBytes, DebugSaveError))
		{
			AppendWarning(LastError, DebugSaveError);
		}
	}

	PendingAnalysisBatches.Add(ActiveBatch);
	ActiveBatch.Reset();
	BroadcastChanged();
	if (bSingleCandidateRequest)
	{
		bCaptureComplete = true;
		ConvaiSceneAutoTagger::CleanupCaptureResources();
		PumpAnalysisQueue();
		return;
	}
	ScheduleNextCaptureBatch();
}

void FSceneAutoTaggerController::PumpAnalysisQueue()
{
	if (bCancelRequested || !bCaptureComplete)
	{
		return;
	}

	const int32 MaximumConcurrent = bSingleCandidateRequest
		? 1
		: FMath::Clamp(GetDefault<UConvaiSceneAutoTaggerSettings>()->MaxConcurrentAnalyses, 1, 6);
	while (!PendingAnalysisBatches.IsEmpty() && InFlightAnalysisBatches.Num() < MaximumConcurrent)
	{
		TSharedPtr<FActiveBatch> Batch = PendingAnalysisBatches[0];
		PendingAnalysisBatches.RemoveAt(0, 1, EAllowShrinking::No);
		if (!Batch.IsValid())
		{
			continue;
		}
		InFlightAnalysisBatches.Add(Batch->BatchId, Batch);
		SubmitAnalysisBatch(Batch);
	}
}

void FSceneAutoTaggerController::SubmitAnalysisBatch(const TSharedPtr<FActiveBatch>& Batch)
{
	if (!Batch.IsValid() || bCancelRequested || !InFlightAnalysisBatches.Contains(Batch->BatchId))
	{
		return;
	}

	if (bSingleCandidateRequest || Batch->ProviderRetryCount > 0 || Batch->AlignmentRetryCount > 0)
	{
		SetState(
			ESceneAutoTaggerState::Tagging,
			bSingleCandidateRequest
				? (Batch->ProviderRetryCount > 0
					? LOCTEXT("RetryingSingleCandidate", "Analysis was temporarily unavailable; retrying this object once...")
					: LOCTEXT("AnalyzingSingleCandidate", "Analyzing the selected object..."))
				: (Batch->ProviderRetryCount > 0
					? LOCTEXT("RetryingBatch", "Analysis was temporarily unavailable; retrying one contact sheet once...")
					: LOCTEXT("RetryingAlignment", "Checking incomplete objects again...")));
	}
	Batch->ProviderStartedAtSeconds = FPlatformTime::Seconds();
	UE_LOG(
		LogConvaiSceneAutoTaggerController,
		Display,
		TEXT("Starting vision analysis for contact sheet %s: %d object(s), %d active request(s)."),
		*Batch->BatchId.ToString(EGuidFormats::Digits).Left(8),
		Batch->CapturedRepresentativeIndices.Num(),
		InFlightAnalysisBatches.Num());

	const FGuid CallbackRunId = ActiveRunId;
	const FGuid CallbackBatchId = Batch->BatchId;
	const TWeakPtr<FSceneAutoTaggerController> WeakController = AsShared();

	const FString FileName = FString::Printf(
		TEXT("sheet-%s.png"),
		*Batch->BatchId.ToString(EGuidFormats::Digits).Left(8));
	// The endpoint caps one PNG at 10 MB; sheets stay far below it, so the
	// narrowing copy from the 64-bit capture buffer is safe.
	TArray<uint8> SheetPng;
	SheetPng.Append(
		Batch->GridPngBytes.GetData(),
		static_cast<int32>(Batch->GridPngBytes.Num()));

	// The DLL owns the prompt; each cell travels as raw structured fields.
	TArray<FConvaiVisionService::FCellMeta> Cells;
	Cells.Reserve(Batch->CapturedRepresentativeIndices.Num());
	for (int32 CellIndex = 0; CellIndex < Batch->CapturedRepresentativeIndices.Num(); ++CellIndex)
	{
		FConvaiVisionService::FCellMeta& Cell = Cells.AddDefaulted_GetRef();
		Cell.EchoId = Batch->EchoIds.IsValidIndex(CellIndex)
			? Batch->EchoIds[CellIndex]
			: FString();
		if (Batch->ObjectPromptContexts.IsValidIndex(CellIndex))
		{
			const FSceneAutoTaggerObjectPromptContext& Context = Batch->ObjectPromptContexts[CellIndex];
			Cell.bContextOnly = Context.bContextOnly;
			if (Context.bIncludePreviousProposal)
			{
				Cell.PreviousProposalName = Context.PreviousName;
				Cell.PreviousProposalDescription = Context.PreviousDescription;
			}
			const FString NormalizedNote =
				ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(Context.RefinementNote);
			if (!NormalizedNote.IsEmpty())
			{
				Cell.RefinementNote = NormalizedNote;
				Cell.PreviousProposalName = Context.PreviousName;
				Cell.PreviousProposalDescription = Context.PreviousDescription;
			}
		}
	}

	FConvaiTaggingPipelineParams Params;
	Params.CharacterID = RunOptions.VisionCharacterID;
	Params.SceneDescription = RunOptions.SceneContext;
	Params.DescriptionFocus = RunOptions.DescriptionFocus;
	Params.MaxParallelRequests = bSingleCandidateRequest
		? 1
		: FMath::Clamp(GetDefault<UConvaiSceneAutoTaggerSettings>()->MaxConcurrentAnalyses, 1, 6);
	Params.BaseUrlOverride = Convai::Get().GetConvaiSettings()->CustomProdURL.TrimStartAndEnd();

	// Cancellation is enforced by the RunId/BatchId guards in HandleVisionResponse;
	// the HTTP request itself is left to finish quietly.
	Batch->Request.Reset();
	FConvaiVisionService::RequestTagging(
		UConvaiUtils::GetAuthHeaderAndKey(),
		MoveTemp(SheetPng),
		FileName,
		MoveTemp(Cells),
		Params,
		[WeakController, CallbackRunId, CallbackBatchId](const FConvaiVisionService::FResult& Result) mutable
		{
			const TSharedPtr<FSceneAutoTaggerController> Self = WeakController.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			FSceneAutoTaggerVisionResponse Response;
			if (Result.bSuccess)
			{
				for (const FConvaiVisionService::FObjectResult& Object : Result.Objects)
				{
					FSceneAutoTaggerVisionItem& Item = Response.Items.AddDefaulted_GetRef();
					Item.Index = Object.CellIndex;
					Item.EchoId = Object.EchoId;
					Item.Name = Object.Name;
					Item.Description = Object.Description;
					Item.Confidence = Object.Confidence;
				}
			}

			Self->HandleVisionResponse(
				CallbackRunId,
				CallbackBatchId,
				Result.bSuccess,
				Result.HttpStatus,
				MoveTemp(Response),
				FString(Result.Error));
		});
}

void FSceneAutoTaggerController::HandleVisionResponse(
	FGuid CallbackRunId,
	FGuid CallbackBatchId,
	bool bSuccess,
	int32 ResponseCode,
	FSceneAutoTaggerVisionResponse&& Response,
	FString&& Error)
{
	if (CallbackRunId != ActiveRunId || bCancelRequested)
	{
		return;
	}
	if (!IsActiveEditorWorldCurrent())
	{
		AbortRun(
			TEXT("The editor level was closed or replaced while analysis was running."),
			LOCTEXT("WorldChangedDuringAnalysis", "Analysis stopped because the level is no longer current."));
		return;
	}
	const TSharedPtr<FActiveBatch>* FoundBatch = InFlightAnalysisBatches.Find(CallbackBatchId);
	if (!FoundBatch || !FoundBatch->IsValid())
	{
		return;
	}
	const TSharedPtr<FActiveBatch> Batch = *FoundBatch;
	Batch->Request.Reset();
	if (!bSuccess)
	{
		// 429 backoff/retries already ran inside FConvaiVisionService; a 429
		// arriving here means they were exhausted, which is a definitive failure.
		// Transport failures and server errors earn one bounded provider retry;
		// definitive 4xx rejections do not.
		const bool bTransient = ResponseCode == 0 || ResponseCode >= 500;
		FinishAnalysisBatchWithFailure(
			Batch,
			ResponseCode > 0
				? FString::Printf(TEXT("The analysis service returned HTTP %d: %s"), ResponseCode, *Error)
				: Error,
			bTransient);
		return;
	}
	UE_LOG(
		LogConvaiSceneAutoTaggerController,
		Display,
		TEXT("Agent completed model contact sheet %s in %.2f s."),
		*Batch->BatchId.ToString(EGuidFormats::Digits).Left(8),
		FPlatformTime::Seconds() - Batch->ProviderStartedAtSeconds);

	const int32 FilledCount = Batch->CapturedRepresentativeIndices.Num();
	const int32 TargetCellCount = Batch->CountTargetCells();
	TMap<int32, const FSceneAutoTaggerVisionItem*> ValidItems;
	for (const FSceneAutoTaggerVisionItem& Item : Response.Items)
	{
		const int32 ZeroBasedIndex = Item.Index - 1;
		if (!Batch->EchoIds.IsValidIndex(ZeroBasedIndex)
			|| Item.EchoId != Batch->EchoIds[ZeroBasedIndex]
			|| ValidItems.Contains(ZeroBasedIndex))
		{
			continue;
		}
		ValidItems.Add(ZeroBasedIndex, &Item);
	}
	TSet<int32> UnchangedRefinementCells;
	for (int32 CellIndex = 0; CellIndex < FilledCount; ++CellIndex)
	{
		if (!Batch->IsTargetCell(CellIndex))
		{
			continue;
		}
		const FSceneAutoTaggerObjectPromptContext* SubmittedContext =
			Batch->ObjectPromptContexts.IsValidIndex(CellIndex)
				? &Batch->ObjectPromptContexts[CellIndex]
				: nullptr;
		const FSceneAutoTaggerVisionItem* const* Item = ValidItems.Find(CellIndex);
		if (SubmittedContext && !SubmittedContext->RefinementNote.IsEmpty() && Item
			&& !ConvaiSceneAutoTagger::VisionProtocol::HasMaterialProposalChange(
				*SubmittedContext,
				**Item))
		{
			// Confidence-only or cosmetically identical output did not apply the
			// one-shot correction. Treat it like a missing aligned cell so it gets
			// one bounded targeted retry and never silently consumes the note.
			ValidItems.Remove(CellIndex);
			UnchangedRefinementCells.Add(CellIndex);
		}
	}
	int32 ValidTargetCellCount = 0;
	for (int32 CellIndex = 0; CellIndex < FilledCount; ++CellIndex)
	{
		ValidTargetCellCount += Batch->IsTargetCell(CellIndex) && ValidItems.Contains(CellIndex) ? 1 : 0;
	}
	const int32 MissingAlignedCells = FMath::Max(0, TargetCellCount - ValidTargetCellCount);
	if (!bSingleCandidateRequest
		&& MissingAlignedCells > 0
		&& Batch->AlignmentRetryCount == 0)
	{
		TArray<int32> RecoveryRepresentativeIndices;
		TArray<uint32> RecoveryPreviewRevisions;
		TArray<TArray<FColor>> RecoveryCells;
		TArray<FString> RecoveryEchoIds;
		TArray<FSceneAutoTaggerObjectPromptContext> RecoveryObjectPromptContexts;
		TArray<uint32> RecoveryRefinementNoteRevisions;
		RecoveryRepresentativeIndices.Reserve(MissingAlignedCells);
		RecoveryPreviewRevisions.Reserve(MissingAlignedCells);
		RecoveryCells.Reserve(MissingAlignedCells);
		RecoveryEchoIds.Reserve(MissingAlignedCells);
		RecoveryObjectPromptContexts.Reserve(MissingAlignedCells);
		RecoveryRefinementNoteRevisions.Reserve(MissingAlignedCells);
		for (int32 CellIndex = 0; CellIndex < FilledCount; ++CellIndex)
		{
			if (Batch->IsTargetCell(CellIndex)
				&& !ValidItems.Contains(CellIndex)
				&& Batch->CapturedRepresentativeIndices.IsValidIndex(CellIndex)
				&& Batch->CapturedPreviewRevisions.IsValidIndex(CellIndex)
				&& Batch->CapturedCells.IsValidIndex(CellIndex)
				&& Batch->EchoIds.IsValidIndex(CellIndex))
			{
				const int32 RepresentativeIndex = Batch->CapturedRepresentativeIndices[CellIndex];
				RecoveryRepresentativeIndices.Add(RepresentativeIndex);
				RecoveryPreviewRevisions.Add(Batch->CapturedPreviewRevisions[CellIndex]);
				RecoveryCells.Add(Batch->CapturedCells[CellIndex]);
				const FString RecoveryEchoId =
					Candidates.IsValidIndex(RepresentativeIndex) && Candidates[RepresentativeIndex].IsValid()
						? BuildEchoId(*Candidates[RepresentativeIndex], RecoveryEchoIds.Num() + 1)
						: Batch->EchoIds[CellIndex];
				RecoveryEchoIds.Add(RecoveryEchoId);
				FSceneAutoTaggerObjectPromptContext PromptContext =
					Batch->ObjectPromptContexts.IsValidIndex(CellIndex)
						? Batch->ObjectPromptContexts[CellIndex]
						: FSceneAutoTaggerObjectPromptContext();
				PromptContext.EchoId = RecoveryEchoId;
				RecoveryObjectPromptContexts.Add(MoveTemp(PromptContext));
				RecoveryRefinementNoteRevisions.Add(
					Batch->CapturedRefinementNoteRevisions.IsValidIndex(CellIndex)
						? Batch->CapturedRefinementNoteRevisions[CellIndex]
						: 0);
			}
		}

		const int32 MaximumGridDimension = GetRunAnalysisGridDimension();
		const int32 CellResolution = GetRunAnalysisCellResolution();
		const int32 RecoveryGridDimension = FMath::Clamp(
			FMath::CeilToInt(FMath::Sqrt(static_cast<float>(RecoveryCells.Num()))),
			1,
			MaximumGridDimension);
		TArray64<uint8> RecoveryGridPngBytes;
		FString RecoveryCompositeError;
		if (!RecoveryCells.IsEmpty()
			&& ConvaiSceneAutoTagger::CompositeGridPng(
				RecoveryCells,
				RecoveryGridDimension,
				CellResolution,
				RecoveryGridDimension * CellResolution,
				RecoveryGridPngBytes,
				RecoveryCompositeError)
			&& !RecoveryGridPngBytes.IsEmpty())
		{
			for (int32 CellIndex = 0; CellIndex < FilledCount; ++CellIndex)
			{
				if (Batch->IsTargetCell(CellIndex))
				{
					if (const FSceneAutoTaggerVisionItem* const* Item = ValidItems.Find(CellIndex))
					{
						const int32 RepresentativeIndex = Batch->CapturedRepresentativeIndices[CellIndex];
						if (IsReviewBatchRequest())
						{
							const uint32 ExpectedRevision = Batch->CapturedPreviewRevisions.IsValidIndex(CellIndex)
								? Batch->CapturedPreviewRevisions[CellIndex]
								: MAX_uint32;
							CommitReviewBatchResult(RepresentativeIndex, ExpectedRevision, **Item);
						}
						else
						{
							ApplyResultToRepresentative(RepresentativeIndex, **Item);
							SaveRepresentativeToCache(RepresentativeIndex);
							FanOutRepresentative(RepresentativeIndex, ESceneAutoTaggerSource::DuplicateGeometry);
							++CompletedRepresentatives;
						}
						}
					}
				}
				if (!IsReviewBatchRequest() && Cache
				&& GetDefault<UConvaiSceneAutoTaggerSettings>()->bUseGeometryCache)
			{
				FString CacheSaveError;
				if (!Cache->Save(CacheSaveError))
				{
					AppendWarning(LastError, CacheSaveError);
				}
			}

			Batch->RequestedRepresentativeIndices = RecoveryRepresentativeIndices;
			Batch->CapturedRepresentativeIndices = MoveTemp(RecoveryRepresentativeIndices);
			Batch->CapturedTargetFlags.Init(true, Batch->CapturedRepresentativeIndices.Num());
			Batch->CapturedPreviewRevisions = MoveTemp(RecoveryPreviewRevisions);
			Batch->CapturedCells = MoveTemp(RecoveryCells);
			Batch->EchoIds = MoveTemp(RecoveryEchoIds);
			Batch->ObjectPromptContexts = MoveTemp(RecoveryObjectPromptContexts);
			Batch->CapturedRefinementNoteRevisions = MoveTemp(RecoveryRefinementNoteRevisions);
			Batch->GridPngBytes = MoveTemp(RecoveryGridPngBytes);
			++Batch->AlignmentRetryCount;
			Progress.Completed = CompletedRepresentatives;
			SubmitAnalysisBatch(Batch);
			return;
		}
		UE_LOG(
			LogConvaiSceneAutoTaggerController,
			Warning,
			TEXT("Could not prepare targeted wording recovery: %s"),
			*RecoveryCompositeError);
	}

	if (TargetCellCount > 0
		&& static_cast<float>(MissingAlignedCells) / static_cast<float>(TargetCellCount) > 0.20f
		&& Batch->AlignmentRetryCount == 0)
	{
		++Batch->AlignmentRetryCount;
		SubmitAnalysisBatch(Batch);
		return;
	}

	if (bSingleCandidateRequest)
	{
		const int32 CandidateIndex = Batch->CapturedRepresentativeIndices.IsEmpty()
			? SingleCandidateIndex
			: Batch->CapturedRepresentativeIndices[0];
		const bool bWasFreshRecapture = SingleCandidateRecaptureSnapshot.IsValid();
		bool bUpdated = false;
		if (const FSceneAutoTaggerVisionItem* const* Item = ValidItems.Find(0))
		{
			ApplyResultToRepresentative(CandidateIndex, **Item);
			SingleCandidateRecaptureSnapshot.Reset();
			if (Candidates.IsValidIndex(CandidateIndex) && Candidates[CandidateIndex].IsValid())
			{
				FSceneAutoTaggerCandidate& Candidate = *Candidates[CandidateIndex];
				Candidate.bUserCapturePendingAnalysis = false;
				const FSceneAutoTaggerObjectPromptContext* SubmittedContext =
					Batch->ObjectPromptContexts.IsValidIndex(0)
						? &Batch->ObjectPromptContexts[0]
						: nullptr;
				const uint32 SubmittedNoteRevision =
					Batch->CapturedRefinementNoteRevisions.IsValidIndex(0)
						? Batch->CapturedRefinementNoteRevisions[0]
						: MAX_uint32;
				const bool bConsumeRefinement = SubmittedContext
					&& !SubmittedContext->RefinementNote.IsEmpty()
					&& Candidate.RefinementNoteRevision == SubmittedNoteRevision
					&& ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
						Candidate.RefinementNote) == SubmittedContext->RefinementNote;
				if (bConsumeRefinement)
				{
					Candidate.RefinementNote.Reset();
					++Candidate.RefinementNoteRevision;
					if (Cache && Cache->Remove(Candidate.Fingerprint))
					{
						FString CacheSaveError;
						if (!Cache->Save(CacheSaveError))
						{
							AppendWarning(LastError, CacheSaveError);
						}
					}
				}
				bUpdated = true;
			}
			if (!ContainsRefinementGuidance(Batch->ObjectPromptContexts))
			{
				ReconcileVisualDuplicates();
			}
		}
		else if (bWasFreshRecapture && RestoreSingleCandidateRecaptureSnapshot())
		{
			LastError = TEXT("Recapture analysis returned no reliable result. The previous image, suggestion, and review state were kept.");
		}
		else if (Candidates.IsValidIndex(CandidateIndex) && Candidates[CandidateIndex].IsValid())
		{
			Candidates[CandidateIndex]->Error =
				TEXT("The agent did not return a reliable result for this image. You can retry analysis without recapturing it.");
		}
		Progress.Completed = 1;
		InFlightAnalysisBatches.Remove(Batch->BatchId);
		bSingleCandidateRequest = false;
		SingleCandidateIndex = INDEX_NONE;
		SetState(
			ESceneAutoTaggerState::ReviewReady,
			bUpdated
				? LOCTEXT("SingleCandidateUpdated", "The selected object's description is ready for review.")
				: (bWasFreshRecapture
					? LOCTEXT("SingleRecaptureMissing", "No reliable description was returned. The previous review was kept.")
					: LOCTEXT("SingleCandidateMissing", "No reliable description was returned. The captured image remains available to retry.")));
		return;
	}

	for (int32 CellIndex = 0; CellIndex < FilledCount; ++CellIndex)
	{
		if (!Batch->IsTargetCell(CellIndex))
		{
			continue;
		}
		const int32 RepresentativeIndex = Batch->CapturedRepresentativeIndices[CellIndex];
		if (IsReviewBatchRequest())
		{
			if (const FSceneAutoTaggerVisionItem* const* Item = ValidItems.Find(CellIndex))
			{
				const uint32 ExpectedRevision = Batch->CapturedPreviewRevisions.IsValidIndex(CellIndex)
					? Batch->CapturedPreviewRevisions[CellIndex]
					: MAX_uint32;
				CommitReviewBatchResult(RepresentativeIndex, ExpectedRevision, **Item);
			}
			else
			{
				const bool bIgnoredRefinement = UnchangedRefinementCells.Contains(CellIndex);
				PreserveReviewBatchCandidate(
					RepresentativeIndex,
					bIgnoredRefinement
						? TEXT("The agent did not apply one saved refinement note after a retry; its previous suggestion and note were kept.")
						: TEXT("The agent did not return a reliable result for one selected image; its previous review was kept."));
				if (bIgnoredRefinement && Candidates.IsValidIndex(RepresentativeIndex)
					&& Candidates[RepresentativeIndex].IsValid())
				{
					Candidates[RepresentativeIndex]->Error =
						TEXT("The saved refinement note was not applied. Edit the note or analyze it again.");
				}
			}
		}
		else
		{
			if (const FSceneAutoTaggerVisionItem* const* Item = ValidItems.Find(CellIndex))
			{
				ApplyResultToRepresentative(RepresentativeIndex, **Item);
				SaveRepresentativeToCache(RepresentativeIndex);
			}
			else if (Candidates.IsValidIndex(RepresentativeIndex) && Candidates[RepresentativeIndex].IsValid())
			{
				MarkRepresentativeFailed(
					RepresentativeIndex,
					TEXT("The agent did not return a reliable result for this image. Select Retry analysis for this row."));
			}
			FanOutRepresentative(RepresentativeIndex, ESceneAutoTaggerSource::DuplicateGeometry);
			++CompletedRepresentatives;
		}
	}

	if (!IsReviewBatchRequest()
		&& Cache
		&& GetDefault<UConvaiSceneAutoTaggerSettings>()->bUseGeometryCache)
	{
		FString CacheSaveError;
		if (!Cache->Save(CacheSaveError))
		{
			AppendWarning(LastError, CacheSaveError);
		}
	}
	Progress.Completed = CompletedRepresentatives;
	CompleteAnalysisBatch(Batch);
}

void FSceneAutoTaggerController::FinishAnalysisBatchWithFailure(
	const TSharedPtr<FActiveBatch>& Batch,
	const FString& Error,
	const bool bTransientRetryEligible)
{
	if (!Batch.IsValid() || !InFlightAnalysisBatches.Contains(Batch->BatchId))
	{
		return;
	}
	UE_LOG(
		LogConvaiSceneAutoTaggerController,
		Warning,
		TEXT("Vision analysis failed after %.2f s: %s"),
		Batch->ProviderStartedAtSeconds > 0.0
			? FPlatformTime::Seconds() - Batch->ProviderStartedAtSeconds
			: 0.0,
		*Error);
	if (!bCancelRequested
		&& Batch->ProviderRetryCount == 0
		&& bTransientRetryEligible)
	{
		++Batch->ProviderRetryCount;
		Batch->Request.Reset();
		SubmitAnalysisBatch(Batch);
		return;
	}
	if (IsReviewBatchRequest())
	{
		for (int32 CellIndex = 0; CellIndex < Batch->CapturedRepresentativeIndices.Num(); ++CellIndex)
		{
			if (!Batch->IsTargetCell(CellIndex))
			{
				continue;
			}
			const int32 CandidateIndex = Batch->CapturedRepresentativeIndices[CellIndex];
			PreserveReviewBatchCandidate(
				CandidateIndex,
				FString::Printf(
					TEXT("Batch analysis was unavailable: %s The previous review was kept."),
					*Error));
		}
		Progress.Completed = CompletedRepresentatives;
		CompleteAnalysisBatch(Batch);
		return;
	}
	if (bSingleCandidateRequest)
	{
		const bool bRestoredPrevious = RestoreSingleCandidateRecaptureSnapshot();
		if (bRestoredPrevious)
		{
			LastError = FString::Printf(
				TEXT("Recapture analysis failed: %s The previous image, suggestion, and review state were kept."),
				*Error);
		}
		else if (Candidates.IsValidIndex(SingleCandidateIndex) && Candidates[SingleCandidateIndex].IsValid())
		{
			FSceneAutoTaggerCandidate& Candidate = *Candidates[SingleCandidateIndex];
			Candidate.Error = Candidate.bHasUserCapture
				? FString::Printf(
					TEXT("Adjusted-view analysis unavailable: %s You can retry without losing the previous text."),
					*Error)
				: FString::Printf(
					TEXT("Analysis unavailable: %s The captured image is retained for Retry analysis."),
					*Error);
		}
		Progress.Completed = 1;
		Batch->Request.Reset();
		InFlightAnalysisBatches.Remove(Batch->BatchId);
		bSingleCandidateRequest = false;
		SingleCandidateIndex = INDEX_NONE;
		SetState(
			ESceneAutoTaggerState::ReviewReady,
			bRestoredPrevious
				? LOCTEXT("SingleRecaptureRetryPreserved", "Recapture analysis failed. The previous review was kept.")
				: LOCTEXT("SingleCandidateRetry", "The captured image is saved locally and can be analyzed again."));
		return;
	}
	for (int32 CellIndex = 0; CellIndex < Batch->CapturedRepresentativeIndices.Num(); ++CellIndex)
	{
		if (!Batch->IsTargetCell(CellIndex))
		{
			continue;
		}
		const int32 RepresentativeIndex = Batch->CapturedRepresentativeIndices[CellIndex];
		if (Candidates.IsValidIndex(RepresentativeIndex) && Candidates[RepresentativeIndex].IsValid())
		{
			MarkRepresentativeFailed(
				RepresentativeIndex,
				FString::Printf(TEXT("Analysis unavailable: %s Select Retry analysis for this row."), *Error));
			FanOutRepresentative(RepresentativeIndex, ESceneAutoTaggerSource::DuplicateGeometry);
			++CompletedRepresentatives;
		}
	}
	Progress.Completed = CompletedRepresentatives;
	CompleteAnalysisBatch(Batch);
}

void FSceneAutoTaggerController::CompleteAnalysisBatch(const TSharedPtr<FActiveBatch>& Batch)
{
	if (Batch.IsValid())
	{
		Batch->Request.Reset();
		InFlightAnalysisBatches.Remove(Batch->BatchId);
	}
	BroadcastChanged();
	PumpAnalysisQueue();
	TryFinishExploration();
}

void FSceneAutoTaggerController::TryFinishExploration()
{
	if (!bCancelRequested
		&& bCaptureComplete
		&& !ActiveBatch.IsValid()
		&& PendingAnalysisBatches.IsEmpty()
		&& InFlightAnalysisBatches.IsEmpty())
	{
		FinishExploration();
	}
}

void FSceneAutoTaggerController::FinishExploration()
{
	if (IsReviewBatchRequest())
	{
		FinishReviewBatch();
		return;
	}
	ActiveBatch.Reset();
	PendingAnalysisBatches.Reset();
	InFlightAnalysisBatches.Reset();
	ConvaiSceneAutoTagger::CleanupCaptureResources();
	ReconcileVisualDuplicates();
	RemoveVisuallyRejectedCandidates();
	bCompletionSummaryAcknowledged = CountAttentionCandidates() == 0
		&& CountLowInformationFilteredCandidates() == 0;
	Progress.Completed = FMath::Min(CompletedRepresentatives, Progress.Total);
	int32 VisibleCandidateCount = 0;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		VisibleCandidateCount += Candidate.IsValid() && !Candidate->bDismissedFromReview ? 1 : 0;
	}
	if (VisibleCandidateCount == 0)
	{
		Progress.Total = 0;
		Progress.Completed = 0;
		SetState(
			ESceneAutoTaggerState::ReviewReady,
			LOCTEXT("NoRenderedCandidates", "No objects contained enough visual information for scene suggestions."));
		return;
	}
	int32 ReadyCount = 0;
	int32 FailedCount = 0;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (Candidate.IsValid() && !Candidate->bDismissedFromReview)
		{
			ReadyCount += Candidate->bTagComplete ? 1 : 0;
			FailedCount += !Candidate->bTagComplete && !Candidate->Error.IsEmpty() ? 1 : 0;
		}
	}
	if (FailedCount > 0)
	{
		SetState(
			ESceneAutoTaggerState::ReviewReady,
			FText::Format(
				LOCTEXT("ReviewPartiallyReady", "{0} suggestion(s) are ready; {1} object(s) still need analysis. Nothing changes until you include and apply."),
				FText::AsNumber(ReadyCount),
				FText::AsNumber(FailedCount)));
	}
	else
	{
		SetState(
			ESceneAutoTaggerState::ReviewReady,
			FText::Format(
				LOCTEXT("ReviewReady", "Review {0} suggested scene object(s). Nothing changes until you include and apply."),
				FText::AsNumber(ReadyCount)));
	}
}

void FSceneAutoTaggerController::RemoveVisuallyRejectedCandidates()
{
	if (VisuallyRejectedActorPaths.IsEmpty())
	{
		VisuallyRejectedReasonsByActorPath.Reset();
		return;
	}

	bool bFilteredAny = false;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (!Candidate.IsValid() || !VisuallyRejectedActorPaths.Contains(Candidate->ActorPath))
		{
			continue;
		}

		Candidate->SuggestedName.Reset();
		Candidate->Description.Reset();
		Candidate->Confidence = 0.0f;
		Candidate->Source = ESceneAutoTaggerSource::None;
		Candidate->Decision = ESceneAutoTaggerDecision::Pending;
		Candidate->Error.Reset();
		Candidate->bTagComplete = false;
		Candidate->bAnalysisCancelled = false;
		Candidate->bApplied = false;
		Candidate->bUserCapturePendingAnalysis = false;
		Candidate->bDismissedFromReview = true;
		Candidate->bFilteredForLowInformation = true;
		Candidate->LowInformationReason =
			VisuallyRejectedReasonsByActorPath.FindRef(Candidate->ActorPath);
		if (Candidate->LowInformationReason == ESceneAutoTaggerLowInformationReason::None)
		{
			Candidate->LowInformationReason =
				ESceneAutoTaggerLowInformationReason::NoUsefulVisualDetail;
		}
		bFilteredAny = true;
	}

	VisuallyRejectedActorPaths.Reset();
	VisuallyRejectedReasonsByActorPath.Reset();
	if (bFilteredAny)
	{
		bCompletionSummaryAcknowledged = false;
	}
}

void FSceneAutoTaggerController::ApplyResultToRepresentative(
	int32 CandidateIndex,
	const FSceneAutoTaggerVisionItem& Item)
{
	if (!Candidates.IsValidIndex(CandidateIndex) || !Candidates[CandidateIndex].IsValid())
	{
		return;
	}
	FSceneAutoTaggerCandidate& Candidate = *Candidates[CandidateIndex];
	Candidate.SuggestedName = Trimmed(Item.Name, 96);
	Candidate.Description = Trimmed(Item.Description, 320);
	Candidate.Confidence = FMath::Clamp(Item.Confidence, 0.0f, 1.0f);
	Candidate.Source = ESceneAutoTaggerSource::Vision;
	Candidate.Error.Reset();
	Candidate.bTagComplete = true;
	Candidate.bAnalysisCancelled = false;
}

void FSceneAutoTaggerController::MarkRepresentativeFailed(
	int32 CandidateIndex,
	const FString& Error)
{
	if (!Candidates.IsValidIndex(CandidateIndex) || !Candidates[CandidateIndex].IsValid())
	{
		return;
	}
	FSceneAutoTaggerCandidate& Candidate = *Candidates[CandidateIndex];
	Candidate.SuggestedName.Reset();
	Candidate.Description.Reset();
	Candidate.Confidence = 0.0f;
	Candidate.Source = ESceneAutoTaggerSource::None;
	Candidate.Decision = ESceneAutoTaggerDecision::Pending;
	Candidate.Error = Error;
	Candidate.bTagComplete = false;
	Candidate.bAnalysisCancelled = false;
}

void FSceneAutoTaggerController::FanOutRepresentative(
	int32 RepresentativeIndex,
	ESceneAutoTaggerSource DuplicateSource)
{
	if (!Candidates.IsValidIndex(RepresentativeIndex) || !Candidates[RepresentativeIndex].IsValid())
	{
		return;
	}
	const FSceneAutoTaggerCandidate& Representative = *Candidates[RepresentativeIndex];
	const TPair<uint32, ConvaiSceneAutoTagger::FSceneAutoTaggerEvidenceDescriptor>* FoundDescriptor =
		EvidenceDescriptorsByActorPath.Find(Representative.ActorPath);
	const bool bHasRepresentativeDescriptor = FoundDescriptor != nullptr;
	const TPair<uint32, ConvaiSceneAutoTagger::FSceneAutoTaggerEvidenceDescriptor>
		RepresentativeDescriptor = bHasRepresentativeDescriptor
			? *FoundDescriptor
			: TPair<uint32, ConvaiSceneAutoTagger::FSceneAutoTaggerEvidenceDescriptor>();
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		if (CandidateIndex == RepresentativeIndex || !Candidates[CandidateIndex].IsValid()
			|| Candidates[CandidateIndex]->DuplicateRepresentative != RepresentativeIndex)
		{
			continue;
		}
		FSceneAutoTaggerCandidate& Duplicate = *Candidates[CandidateIndex];
		Duplicate.SuggestedName = Representative.SuggestedName;
		Duplicate.Description = Representative.Description;
		Duplicate.Confidence = Representative.Confidence;
		Duplicate.Source = Representative.bTagComplete ? DuplicateSource : Representative.Source;
		Duplicate.Error = Representative.Error;
		Duplicate.bTagComplete = Representative.bTagComplete;
		Duplicate.bAnalysisCancelled = Representative.bAnalysisCancelled;
		Duplicate.PreviewBGRA = Representative.PreviewBGRA;
		Duplicate.PreviewSize = Representative.PreviewSize;
		Duplicate.PreviewRevision = Representative.PreviewRevision;
		// The copied preview carries the representative's evaluation; a stale
		// entry for the duplicate's own path must not survive the overwrite.
		if (bHasRepresentativeDescriptor)
		{
			EvidenceDescriptorsByActorPath.Add(Duplicate.ActorPath, RepresentativeDescriptor);
		}
		else
		{
			EvidenceDescriptorsByActorPath.Remove(Duplicate.ActorPath);
		}
		if (Representative.Actor.IsValid() && Duplicate.Actor.IsValid())
		{
			const FVector WorldDirection = Representative.Actor->GetActorQuat()
				.RotateVector(Representative.EvidenceViewDirectionActorLocal);
			Duplicate.EvidenceViewDirectionActorLocal = Duplicate.Actor->GetActorQuat()
				.UnrotateVector(WorldDirection)
				.GetSafeNormal();
		}
		else
		{
			Duplicate.EvidenceViewDirectionActorLocal =
				Representative.EvidenceViewDirectionActorLocal;
		}
		Duplicate.EvidenceDistanceScale = Representative.EvidenceDistanceScale;
		Duplicate.EvidenceTargetOffset = Representative.EvidenceTargetOffset;
		Duplicate.CaptureLightingPolicy = Representative.CaptureLightingPolicy;
	}
}

void FSceneAutoTaggerController::RebuildDuplicateGroupsFromFingerprints()
{
	TMap<FString, int32> FingerprintRepresentatives;
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (!Candidate.IsValid())
		{
			continue;
		}
		Candidate->DuplicateRepresentative = CandidateIndex;
		Candidate->DuplicateGroupSize = 1;
		if (Candidate->Fingerprint.IsEmpty())
		{
			continue;
		}
		if (const int32* ExistingRepresentative =
			FingerprintRepresentatives.Find(Candidate->Fingerprint))
		{
			Candidate->DuplicateRepresentative = *ExistingRepresentative;
		}
		else
		{
			FingerprintRepresentatives.Add(Candidate->Fingerprint, CandidateIndex);
		}
	}

	TMap<int32, int32> DuplicateCounts;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (Candidate.IsValid())
		{
			++DuplicateCounts.FindOrAdd(Candidate->DuplicateRepresentative);
		}
	}
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (Candidate.IsValid())
		{
			Candidate->DuplicateGroupSize =
				FMath::Max(1, DuplicateCounts.FindRef(Candidate->DuplicateRepresentative));
		}
	}
}

void FSceneAutoTaggerController::ReconcileVisualDuplicates()
{
	// Review recapture may invalidate a prior retained-image match. Rebuild the
	// immutable geometry baseline first, then prove every visual merge again.
	RebuildDuplicateGroupsFromFingerprints();
	if (Candidates.Num() < 2)
	{
		return;
	}

	// The capture-time core evaluation already produced a descriptor for most
	// rows; reuse it while its preview revision still matches. Cache-loaded and
	// user-edited rows fall back to a one-off rebuild so pairwise comparison
	// still touches only 32x32 descriptors on the game thread.
	TArray<ConvaiSceneAutoTagger::FSceneAutoTaggerEvidenceDescriptor> Descriptors;
	Descriptors.SetNum(Candidates.Num());
	TArray<bool> bDescriptorValid;
	bDescriptorValid.Init(false, Candidates.Num());
	TArray<int32> CandidateOrder;
	CandidateOrder.Reserve(Candidates.Num());
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[Index];
		if (Candidate.IsValid() && !Candidate->PreviewBGRA.IsEmpty())
		{
			const TPair<uint32, ConvaiSceneAutoTagger::FSceneAutoTaggerEvidenceDescriptor>* Cached =
				EvidenceDescriptorsByActorPath.Find(Candidate->ActorPath);
			if (Cached && Cached->Key == Candidate->PreviewRevision)
			{
				Descriptors[Index] = Cached->Value;
				bDescriptorValid[Index] = true;
			}
			else
			{
				bDescriptorValid[Index] = ConvaiSceneAutoTagger::BuildCapturedViewDescriptor(
					Candidate->PreviewBGRA,
					Candidate->PreviewSize,
					Descriptors[Index]);
			}
		}
		CandidateOrder.Add(Index);
	}
	CandidateOrder.StableSort([this](const int32 LeftIndex, const int32 RightIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Left = Candidates[LeftIndex];
		const TSharedPtr<FSceneAutoTaggerCandidate>& Right = Candidates[RightIndex];
		if (!Left.IsValid() || !Right.IsValid())
		{
			return Left.IsValid();
		}
		if (Left->bTagComplete != Right->bTagComplete)
		{
			return Left->bTagComplete;
		}
		if (!FMath::IsNearlyEqual(Left->Confidence, Right->Confidence))
		{
			return Left->Confidence > Right->Confidence;
		}
		return LeftIndex < RightIndex;
	});

	TArray<int32> VisualRepresentatives;
	bool bCacheUpdated = false;
	for (const int32 Index : CandidateOrder)
	{
		if (!bDescriptorValid[Index] || !Candidates[Index].IsValid())
		{
			continue;
		}
		int32 MatchingRepresentative = INDEX_NONE;
		for (const int32 RepresentativeIndex : VisualRepresentatives)
		{
			const TSharedPtr<FSceneAutoTaggerCandidate>& Representative =
				Candidates[RepresentativeIndex];
			if (Representative.IsValid()
				&& Representative->bOpposedBroadFacePresentation
					== Candidates[Index]->bOpposedBroadFacePresentation
				&& ConvaiSceneAutoTagger::AreCapturedViewDescriptorsVisuallyEquivalent(
					Descriptors[RepresentativeIndex],
					Descriptors[Index]))
			{
				MatchingRepresentative = RepresentativeIndex;
				break;
			}
		}
		if (MatchingRepresentative == INDEX_NONE)
		{
			// Similarity is intentionally not treated as transitive. Every member
			// of a group must match this fixed, highest-confidence representative.
			VisualRepresentatives.Add(Index);
			continue;
		}
		if (!Candidates[MatchingRepresentative].IsValid()
			|| !Candidates[MatchingRepresentative]->bTagComplete)
		{
			continue;
		}

		FSceneAutoTaggerCandidate& Match = *Candidates[Index];
		const FSceneAutoTaggerCandidate& Representative = *Candidates[MatchingRepresentative];
		Match.SuggestedName = Representative.SuggestedName;
		Match.Description = Representative.Description;
		Match.Confidence = Representative.Confidence;
		Match.Source = ESceneAutoTaggerSource::DuplicateVisual;
		Match.Error.Reset();
		Match.bTagComplete = true;
		// A strict retained-image match is safe to merge like an exact geometry
		// duplicate. Equal generated names alone are deliberately insufficient.
		Match.DuplicateRepresentative = Representative.DuplicateRepresentative;

		if (Cache && GetDefault<UConvaiSceneAutoTaggerSettings>()->bUseGeometryCache
			&& !Match.Fingerprint.IsEmpty())
		{
			ConvaiSceneAutoTagger::FSceneAutoTaggerCachedTag Tag;
			Tag.Name = Match.SuggestedName;
			Tag.Description = Match.Description;
			Tag.Confidence = Match.Confidence;
			Cache->Store(Match.Fingerprint, Tag);
			bCacheUpdated = true;
		}
	}
	TMap<int32, int32> DuplicateCounts;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (Candidate.IsValid() && Candidate->DuplicateRepresentative != INDEX_NONE)
		{
			++DuplicateCounts.FindOrAdd(Candidate->DuplicateRepresentative);
		}
	}
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (Candidate.IsValid())
		{
			Candidate->DuplicateGroupSize = FMath::Max(
				1,
				DuplicateCounts.FindRef(Candidate->DuplicateRepresentative));
		}
	}

	if (bCacheUpdated)
	{
		FString CacheSaveError;
		if (!Cache->Save(CacheSaveError))
		{
			AppendWarning(LastError, CacheSaveError);
		}
	}
}

void FSceneAutoTaggerController::SaveRepresentativeToCache(int32 RepresentativeIndex)
{
	if (!Cache || !Candidates.IsValidIndex(RepresentativeIndex) || !Candidates[RepresentativeIndex].IsValid())
	{
		return;
	}
	const UConvaiSceneAutoTaggerSettings* Settings = GetDefault<UConvaiSceneAutoTaggerSettings>();
	const FSceneAutoTaggerCandidate& Candidate = *Candidates[RepresentativeIndex];
	if (!Settings->bUseGeometryCache || Candidate.Source != ESceneAutoTaggerSource::Vision
		|| Candidate.Fingerprint.IsEmpty())
	{
		return;
	}
	ConvaiSceneAutoTagger::FSceneAutoTaggerCachedTag Tag;
	Tag.Name = Candidate.SuggestedName;
	Tag.Description = Candidate.Description;
	Tag.Confidence = Candidate.Confidence;
	Cache->Store(Candidate.Fingerprint, Tag);
}

void FSceneAutoTaggerController::EnsureUniqueAcceptedNames()
{
	if (FrozenDuplicateClusteringPlan.IsValid())
	{
		EnsureUniqueAcceptedNames(*FrozenDuplicateClusteringPlan);
		return;
	}

	const UConvaiSceneAutoTaggerSettings* Settings = GetDefault<UConvaiSceneAutoTaggerSettings>();
	const ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan ClusteringPlan =
		BuildDuplicateClusteringPlanForCandidates(Candidates, *Settings);
	EnsureUniqueAcceptedNames(ClusteringPlan);
}

void FSceneAutoTaggerController::EnsureUniqueAcceptedNames(
	const ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan& ClusteringPlan)
{
	TMap<FString, FString> NameOwners;
	TMap<FString, FString> PreferredNameByMergedOwner;
	const auto GetOwnerKey = [this, &ClusteringPlan](const int32 CandidateIndex)
	{
		FString OwnerKey = ClusteringPlan.GetOwnerKeyForCandidate(CandidateIndex);
		if (!OwnerKey.IsEmpty())
		{
			return OwnerKey;
		}
		if (Candidates.IsValidIndex(CandidateIndex) && Candidates[CandidateIndex].IsValid()
			&& !Candidates[CandidateIndex]->ActorPath.IsEmpty())
		{
			return Candidates[CandidateIndex]->ActorPath;
		}
		return FString::Printf(TEXT("candidate:%d"), CandidateIndex);
	};
	const auto GetClusterOrderKey = [this, &ClusteringPlan](const int32 CandidateIndex)
	{
		if (const ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateCluster* Cluster =
			ClusteringPlan.FindClusterForCandidate(CandidateIndex))
		{
			if (!Cluster->ActorPaths.IsEmpty())
			{
				return Cluster->ActorPaths[0];
			}
		}
		return Candidates.IsValidIndex(CandidateIndex) && Candidates[CandidateIndex].IsValid()
			? Candidates[CandidateIndex]->ActorPath
			: FString();
	};
	const auto SortByStableOwner = [this, &GetOwnerKey, &GetClusterOrderKey](
		const int32 Left,
		const int32 Right)
	{
		const int32 ClusterComparison = GetClusterOrderKey(Left).Compare(
			GetClusterOrderKey(Right),
			ESearchCase::CaseSensitive);
		if (ClusterComparison != 0)
		{
			return ClusterComparison < 0;
		}
		const int32 OwnerComparison = GetOwnerKey(Left).Compare(
			GetOwnerKey(Right),
			ESearchCase::CaseSensitive);
		if (OwnerComparison != 0)
		{
			return OwnerComparison < 0;
		}
		const FString LeftPath = Candidates.IsValidIndex(Left) && Candidates[Left].IsValid()
			? Candidates[Left]->ActorPath
			: FString();
		const FString RightPath = Candidates.IsValidIndex(Right) && Candidates[Right].IsValid()
			? Candidates[Right]->ActorPath
			: FString();
		const int32 PathComparison = LeftPath.Compare(RightPath, ESearchCase::CaseSensitive);
		return PathComparison != 0 ? PathComparison < 0 : Left < Right;
	};

	TArray<int32> AppliedIndices;
	TArray<int32> ReadyIndices;
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (Candidate.IsValid() && Candidate->Decision == ESceneAutoTaggerDecision::Accepted
			&& Candidate->bApplied)
		{
			AppliedIndices.Add(CandidateIndex);
		}
		if (IsReadyToApplyCandidate(Candidate))
		{
			ReadyIndices.Add(CandidateIndex);
		}
	}
	AppliedIndices.Sort(SortByStableOwner);
	ReadyIndices.Sort(SortByStableOwner);

	// Already-applied rows reserve their serialized names but must never be silently renamed.
	for (const int32 CandidateIndex : AppliedIndices)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		const FString CanonicalName = FConvaiObjectEntry::NormalizeMovementPointName(Candidate->SuggestedName);
		if (CanonicalName.IsEmpty())
		{
			continue;
		}
		const FString OwnerKey = GetOwnerKey(CandidateIndex);
		if (!NameOwners.Contains(CanonicalName.ToLower()))
		{
			NameOwners.Add(CanonicalName.ToLower(), OwnerKey);
		}
		if (ClusteringPlan.ShouldMergeCandidate(CandidateIndex))
		{
			if (!PreferredNameByMergedOwner.Contains(OwnerKey))
			{
				PreferredNameByMergedOwner.Add(OwnerKey, CanonicalName);
			}
		}
	}

	for (const int32 CandidateIndex : ReadyIndices)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		FString BaseName = FConvaiObjectEntry::NormalizeMovementPointName(
			Trimmed(Candidate->SuggestedName, 64));
		const FString OwnerKey = GetOwnerKey(CandidateIndex);
		if (ClusteringPlan.ShouldMergeCandidate(CandidateIndex))
		{
			if (const FString* PreferredName = PreferredNameByMergedOwner.Find(OwnerKey))
			{
				BaseName = *PreferredName;
			}
			else
			{
				PreferredNameByMergedOwner.Add(OwnerKey, BaseName);
			}
		}
		FString UniqueName = BaseName;
		int32 Suffix = 2;
		while (const FString* ExistingOwner = NameOwners.Find(UniqueName.ToLower()))
		{
			if (*ExistingOwner == OwnerKey)
			{
				break;
			}
			UniqueName = BuildBoundedCollisionName(BaseName, Suffix++);
		}
		NameOwners.FindOrAdd(UniqueName.ToLower()) = OwnerKey;
		Candidate->SuggestedName = UniqueName;
	}
}

void FSceneAutoTaggerController::ApplyAccepted()
{
	if (IsBusy())
	{
		return;
	}
	const UConvaiSceneAutoTaggerSettings* Settings = GetDefault<UConvaiSceneAutoTaggerSettings>();
	TUniquePtr<ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan> PendingPlan;
	const ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan* ClusteringPlan =
		FrozenDuplicateClusteringPlan.Get();
	if (!ClusteringPlan)
	{
		PendingPlan = MakeUnique<ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan>(
			BuildDuplicateClusteringPlanForCandidates(Candidates, *Settings));
		ClusteringPlan = PendingPlan.Get();
	}
	EnsureUniqueAcceptedNames(*ClusteringPlan);
	TArray<FSceneAutoTaggerApplyRequest> Requests;
	TArray<TSharedPtr<FSceneAutoTaggerCandidate>> RequestCandidates;
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate = Candidates[CandidateIndex];
		if (!IsReadyToApplyCandidate(Candidate))
		{
			continue;
		}
		FSceneAutoTaggerApplyRequest& Request = Requests.AddDefaulted_GetRef();
		Request.Actor = Candidate->Actor;
		Request.Name = Candidate->SuggestedName;
		Request.Description = Candidate->Description;
		const int32 StableMergeGroupIndex =
			ClusteringPlan->GetMergeGroupIndexForCandidate(CandidateIndex);
		Request.bMergeDuplicates = ClusteringPlan->ShouldMergeCandidate(CandidateIndex);
		Request.MergeGroupIndex = Request.bMergeDuplicates
			? StableMergeGroupIndex
			: 0;
		Request.bIncludeInSpatialAwareness = Settings->bIncludeInSpatialAwareness;
		Request.bGazeable = Settings->bGazeable;
		Request.bUpdateExistingWholeActorComponent = true;
		RequestCandidates.Add(Candidate);
	}

	if (Requests.IsEmpty())
	{
		LastApplySummary = TEXT("No included, unapplied objects are ready.");
		BroadcastChanged();
		return;
	}

	SetState(
		ESceneAutoTaggerState::Applying,
		FText::Format(
			LOCTEXT("Applying", "Applying Convai metadata to {0} included object(s)..."),
			FText::AsNumber(Requests.Num())));
	const FSceneAutoTaggerApplyResult Result = FSceneAutoTaggerApply::ApplyAcceptedRequests(
		Requests,
		bGenerateMovementPointsOnApply);
	bool bAppliedAnyRequest = false;
	for (int32 Index = 0; Index < Result.ActorOutcomes.Num() && Index < RequestCandidates.Num(); ++Index)
	{
		if (Result.ActorOutcomes[Index].WasApplied())
		{
			bAppliedAnyRequest = true;
			RequestCandidates[Index]->bApplied = true;
			UndoTrackedAppliedActorPaths.Add(RequestCandidates[Index]->ActorPath);
			RequestCandidates[Index]->Error.Reset();
		}
		else
		{
			RequestCandidates[Index]->bApplied = false;
			RequestCandidates[Index]->Error = Result.ActorOutcomes[Index].Message.ToString();
		}
	}
	if (!FrozenDuplicateClusteringPlan.IsValid() && bAppliedAnyRequest && PendingPlan.IsValid())
	{
		FrozenDuplicateClusteringPlan = MoveTemp(PendingPlan);
	}
	LastApplySummary = FString::Printf(
		TEXT("Applied %d object(s): %d created, %d updated, %d already matched; %d failed."),
		Result.CreatedComponents + Result.UpdatedComponents + Result.UnchangedComponents,
		Result.CreatedComponents,
		Result.UpdatedComponents,
		Result.UnchangedComponents,
		Result.FailedRequests);
	if (Result.bMovementPointGenerationRequested)
	{
		LastApplySummary += FString::Printf(
			TEXT(" Movement points: %d generated for %d object(s); %d not changed (%d already configured, %d need navigation, %d had no valid standing location)."),
			Result.GeneratedMovementPoints,
			Result.GeneratedMovementPointComponents,
			Result.MovementPointSkippedComponents,
			Result.MovementPointExistingComponents,
			Result.MovementPointMissingNavigationComponents,
			Result.MovementPointNoValidCandidateComponents);
	}
	SetState(
		ESceneAutoTaggerState::Completed,
		Result.FailedRequests > 0
			? LOCTEXT("ApplyPartial", "Apply completed with some errors; review the affected rows.")
			: LOCTEXT("ApplyComplete", "Included objects now have Convai Object Components."));
}

void FSceneAutoTaggerController::SetGenerateMovementPointsOnApply(const bool bGenerate)
{
	if (IsBusy() || bGenerateMovementPointsOnApply == bGenerate)
	{
		return;
	}
	bGenerateMovementPointsOnApply = bGenerate;
	BroadcastChanged();
}

TArray<TSharedPtr<FSceneAutoTaggerCandidate>>
FSceneAutoTaggerController::GetLowInformationFilteredCandidates() const
{
	TArray<TSharedPtr<FSceneAutoTaggerCandidate>> FilteredCandidates;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (Candidate.IsValid()
			&& Candidate->Actor.IsValid()
			&& Candidate->bDismissedFromReview
			&& Candidate->bFilteredForLowInformation)
		{
			FilteredCandidates.Add(Candidate);
		}
	}
	return FilteredCandidates;
}

int32 FSceneAutoTaggerController::CountLowInformationFilteredCandidates() const
{
	int32 Count = 0;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		Count += Candidate.IsValid()
			&& Candidate->Actor.IsValid()
			&& Candidate->bDismissedFromReview
			&& Candidate->bFilteredForLowInformation
			? 1
			: 0;
	}
	return Count;
}

TArray<TSharedPtr<FSceneAutoTaggerCandidate>>
FSceneAutoTaggerController::GetAttentionCandidates() const
{
	TArray<TSharedPtr<FSceneAutoTaggerCandidate>> AttentionCandidates;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (Candidate.IsValid() && !Candidate->bDismissedFromReview
			&& (!Candidate->Actor.IsValid()
				|| (!Candidate->bTagComplete && !Candidate->bAnalysisCancelled
					&& !Candidate->Error.IsEmpty())))
		{
			AttentionCandidates.Add(Candidate);
		}
	}
	return AttentionCandidates;
}

int32 FSceneAutoTaggerController::CountAttentionCandidates() const
{
	int32 Count = 0;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		Count += Candidate.IsValid() && !Candidate->bDismissedFromReview
			&& (!Candidate->Actor.IsValid()
				|| (!Candidate->bTagComplete && !Candidate->bAnalysisCancelled
					&& !Candidate->Error.IsEmpty()))
			? 1
			: 0;
	}
	return Count;
}

bool FSceneAutoTaggerController::ShouldShowCompletionSummary() const
{
	return !bCompletionSummaryAcknowledged
		&& (CountAttentionCandidates() > 0
			|| CountLowInformationFilteredCandidates() > 0)
		&& (Progress.State == ESceneAutoTaggerState::ReviewReady
			|| Progress.State == ESceneAutoTaggerState::Completed);
}

void FSceneAutoTaggerController::AcknowledgeCompletionSummary()
{
	if (!bCompletionSummaryAcknowledged)
	{
		bCompletionSummaryAcknowledged = true;
		BroadcastChanged();
	}
}

void FSceneAutoTaggerController::ReopenCompletionSummary()
{
	if (bCompletionSummaryAcknowledged
		&& (CountAttentionCandidates() > 0
			|| CountLowInformationFilteredCandidates() > 0))
	{
		bCompletionSummaryAcknowledged = false;
		BroadcastChanged();
	}
}

int32 FSceneAutoTaggerController::CountDecision(ESceneAutoTaggerDecision Decision) const
{
	int32 Count = 0;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		Count += Candidate.IsValid() && !Candidate->bDismissedFromReview
			&& Candidate->Decision == Decision ? 1 : 0;
	}
	return Count;
}

int32 FSceneAutoTaggerController::CountReadyToApply() const
{
	int32 Count = 0;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		Count += IsReadyToApplyCandidate(Candidate) ? 1 : 0;
	}
	return Count;
}

int32 FSceneAutoTaggerController::CountApplied() const
{
	int32 Count = 0;
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		Count += Candidate.IsValid() && Candidate->bApplied ? 1 : 0;
	}
	return Count;
}

bool FSceneAutoTaggerController::IsBusy() const
{
	return Progress.State == ESceneAutoTaggerState::Discovering
		|| Progress.State == ESceneAutoTaggerState::Capturing
		|| Progress.State == ESceneAutoTaggerState::Tagging
		|| Progress.State == ESceneAutoTaggerState::Applying;
}

bool FSceneAutoTaggerController::CanApply() const
{
	return !IsBusy() && CountReadyToApply() > 0;
}

bool FSceneAutoTaggerController::HasReviewResults() const
{
	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : Candidates)
	{
		if (Candidate.IsValid() && !Candidate->bDismissedFromReview && Candidate->bTagComplete)
		{
			return true;
		}
	}
	return false;
}

void FSceneAutoTaggerController::SetState(ESceneAutoTaggerState State, const FText& Message)
{
	Progress.State = State;
	Progress.Message = Message;
	BroadcastChanged();
}

void FSceneAutoTaggerController::BroadcastChanged()
{
	ChangedDelegate.Broadcast();
}

FString FSceneAutoTaggerController::BuildEchoId(
	const FSceneAutoTaggerCandidate& Candidate,
	int32 CellIndex)
{
	return FString::Printf(
		TEXT("cell-%02d-%s"),
		CellIndex,
		*Sha1Prefix(Candidate.Fingerprint + TEXT("|") + Candidate.ActorPath, 16));
}

void FSceneAutoTaggerController::BuildPreview(
	FSceneAutoTaggerCandidate& Candidate,
	const TArray<FColor>& CellPixels,
	int32 CellResolution)
{
	Candidate.PreviewBGRA.SetNumUninitialized(CellPixels.Num() * sizeof(FColor));
	if (!CellPixels.IsEmpty())
	{
		FMemory::Memcpy(
			Candidate.PreviewBGRA.GetData(),
			CellPixels.GetData(),
			Candidate.PreviewBGRA.Num());
	}
	Candidate.PreviewSize = FIntPoint(CellResolution, CellResolution);
	++Candidate.PreviewRevision;
}


#undef LOCTEXT_NAMESPACE
