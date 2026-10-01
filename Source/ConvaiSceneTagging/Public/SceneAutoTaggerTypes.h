// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiSceneAutoTaggerSettings.h"

class AActor;
class UPrimitiveComponent;

namespace ConvaiSceneAutoTagger
{
/** Lighting treatment retained with the selected evidence for consistent recaptures. */
enum class ESceneAutoTaggerCaptureLightingPolicy : uint8
{
	PreserveMaterialSpecular,
	SuppressDirectSpecular,
	/** Keep broad-face captures diffuse unless a measured last-resort metal probe is required. */
	SuppressDirectSpecularWithMetalRecovery
};

inline bool ShouldSuppressDirectSpecularHighlights(
	const ESceneAutoTaggerCaptureLightingPolicy Policy)
{
	return Policy != ESceneAutoTaggerCaptureLightingPolicy::PreserveMaterialSpecular;
}

inline bool ShouldAllowDirectSpecularRecovery(
	const ESceneAutoTaggerCaptureLightingPolicy Policy)
{
	return Policy != ESceneAutoTaggerCaptureLightingPolicy::SuppressDirectSpecular;
}
}

enum class ESceneAutoTaggerState : uint8
{
	Idle,
	Discovering,
	Capturing,
	Tagging,
	ReviewReady,
	Applying,
	Completed,
	Cancelled,
	Failed
};

enum class ESceneAutoTaggerDecision : uint8
{
	Pending,
	Accepted,
	Rejected
};

/** Why a successfully captured object was conservatively skipped before provider analysis. */
enum class ESceneAutoTaggerLowInformationReason : uint8
{
	None,
	ObjectNotClearlyVisible,
	NoUsefulVisualDetail
};

enum class ESceneAutoTaggerSource : uint8
{
	None,
	Vision,
	GeometryCache,
	DuplicateGeometry,
	DuplicateVisual,
	HeuristicFallback
};

struct FSceneAutoTaggerCandidate : public TSharedFromThis<FSceneAutoTaggerCandidate>
{
	TWeakObjectPtr<AActor> Actor;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> PrimitiveComponents;
	FBox WorldBounds = FBox(ForceInit);
	FString ActorLabel;
	FString ActorPath;
	FString AssetSummary;
	FString Fingerprint;
	FString SignificanceReason;
	float SignificanceScore = 0.0f;
	/** Opaque protected-core evidence flags retained for later native eligibility decisions. */
	uint32 SignificanceReasonFlags = 0;
	/** Opaque discovery result used only to choose an asset-backed fallback label. */
	bool bHasAuthoredLabel = false;
	bool bHadConvaiComponent = false;

	FString SuggestedName;
	FString Description;
	/** Review-local, one-shot user guidance for the next analysis of this actor. Never applied to the scene. */
	FString RefinementNote;
	/** Changes whenever RefinementNote changes so an older async result cannot consume a newer draft. */
	uint32 RefinementNoteRevision = 0;
	float Confidence = 0.0f;
	ESceneAutoTaggerSource Source = ESceneAutoTaggerSource::None;
	ESceneAutoTaggerDecision Decision = ESceneAutoTaggerDecision::Pending;
	FString Error;
	bool bTagComplete = false;
	/** User cancellation stopped this row before it produced a result; distinct from an error and cleared by retry. */
	bool bAnalysisCancelled = false;
	bool bApplied = false;
	/** Run-local tombstone. Preserves candidate indices while hiding a row from the primary review. */
	bool bDismissedFromReview = false;
	/** Distinguishes an automatic low-information skip from a user's manual review dismissal. */
	bool bFilteredForLowInformation = false;
	ESceneAutoTaggerLowInformationReason LowInformationReason =
		ESceneAutoTaggerLowInformationReason::None;
	int32 DuplicateRepresentative = INDEX_NONE;
	int32 DuplicateGroupSize = 1;

	/** Small BGRA preview generated from the isolated capture cell. */
	TArray<uint8> PreviewBGRA;
	FIntPoint PreviewSize = FIntPoint::ZeroValue;
	uint32 PreviewRevision = 0;
	/** Actor-local direction from the capture camera toward the object. */
	FVector EvidenceViewDirectionActorLocal = FVector::ZeroVector;
	float EvidenceDistanceScale = 1.0f;
	FVector2D EvidenceTargetOffset = FVector2D::ZeroVector;
	/** Geometry-only presentation signal retained independently from the model's semantic classification. */
	bool bOpposedBroadFacePresentation = false;
	/** Persisted so probes, winning evidence, duplicate rows, and Edit View use one lighting treatment. */
	ConvaiSceneAutoTagger::ESceneAutoTaggerCaptureLightingPolicy CaptureLightingPolicy =
		ConvaiSceneAutoTagger::ESceneAutoTaggerCaptureLightingPolicy::PreserveMaterialSpecular;
	bool bHasUserCapture = false;
	bool bUserCapturePendingAnalysis = false;
};

/** Cancellation handle for one in-flight analysis request. */
class ISceneAutoTaggerRequest
{
public:
	virtual ~ISceneAutoTaggerRequest() = default;
	virtual void Cancel() = 0;
};

struct FSceneAutoTaggerRunOptions
{
	TArray<TWeakObjectPtr<AActor>> ScopedActors;
	/** Explicit Convai character selected for this run. No project/test-character fallback is used. */
	FString VisionCharacterID;
	/** Display name snapshotted with VisionCharacterID for stable analysis and review UI. */
	FString VisionCharacterName;
	/** Optional user-authored scene theme, snapshotted for the entire review. */
	FString SceneContext;
	/** Optional emphasis for pixel-supported details, snapshotted for the entire review. */
	FString DescriptionFocus;
    /** Complete analysis preset, snapshotted so later requests cannot observe mutable settings. */
    EConvaiSceneAutoTaggerAnalysisDetail AnalysisDetail =
        EConvaiSceneAutoTaggerAnalysisDetail::Unspecified;
    /** Resolved contact-sheet packing, snapshotted so settings cannot change an active run. Zero resolves from settings at startup. */
    int32 AnalysisGridDimension = 0;
	/** Resolved pixels per object in the model contact sheet. Zero resolves from settings at startup. */
	int32 AnalysisCellResolution = 0;
	/** Resolved retained evidence size. Zero resolves from settings at startup. */
	int32 EvidencePreviewResolution = 0;
	float SignificanceThreshold = 0.52f;
	bool bIncludeAlreadyTaggedActors = false;
	/** Explicitly scoped actors bypass soft relevance and retained-information pruning. Hard eligibility rules still apply. */
	bool bForceAnalyzeScopedActors = false;
};

struct FSceneAutoTaggerProgress
{
	ESceneAutoTaggerState State = ESceneAutoTaggerState::Idle;
	int32 Completed = 0;
	int32 Total = 0;
	FText Message;

	float Fraction() const
	{
		return Total > 0 ? FMath::Clamp(static_cast<float>(Completed) / static_cast<float>(Total), 0.0f, 1.0f) : 0.0f;
	}
};

inline const TCHAR* LexToString(ESceneAutoTaggerSource Source)
{
	switch (Source)
	{
	case ESceneAutoTaggerSource::Vision: return TEXT("Vision");
	case ESceneAutoTaggerSource::GeometryCache: return TEXT("Cache");
	case ESceneAutoTaggerSource::DuplicateGeometry: return TEXT("Duplicate");
	case ESceneAutoTaggerSource::DuplicateVisual: return TEXT("Visual duplicate");
	case ESceneAutoTaggerSource::HeuristicFallback: return TEXT("Offline");
	default: return TEXT("Pending");
	}
}
