// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneAutoTaggerTypes.h"

class AActor;
class UPrimitiveComponent;

namespace ConvaiSceneAutoTagger
{
/** Unreal-facing copy of the rendered evidence returned by the native core. */
struct FSceneAutoTaggerViewMetrics
{
	bool bValid = false;
	int32 ForegroundPixels = 0;
	int32 InteriorPixels = 0;
	float ForegroundFraction = 0.0f;
	float BorderTouchFraction = 0.0f;
	float ColorEntropy = 0.0f;
	float EdgeDetail = 0.0f;
	float SilhouetteInformation = 0.0f;
	bool bShapeProfileCandidate = false;
	int32 CentralForegroundPixels = 0;
	float CentralToneEntropy = 0.0f;
	float CentralColorEntropy = 0.0f;
	float CentralEdgeDetail = 0.0f;
	float CentralForegroundFill = 0.0f;
	float CentralAuthoredContent = 0.0f;
	float CaptureQuality = 0.0f;
	float FrontPreference = 0.0f;
	float SelectionScore = -1.0f;
};

/** Candidate directions point from the object toward the camera in actor-local space. */
struct FSceneAutoTaggerViewPlan
{
	TArray<FVector> CameraOffsetsActorLocal;
	TArray<float> FrontPreferences;
	/** Native-core candidate flags, parallel to CameraOffsetsActorLocal. */
	TArray<bool> ShapeProfileCandidates;
	ESceneAutoTaggerCaptureLightingPolicy CaptureLightingPolicy =
		ESceneAutoTaggerCaptureLightingPolicy::PreserveMaterialSpecular;
	bool bOpposedBroadFacePresentation = false;
	bool bPlanar = false;
};

/** Opaque native-core evidence. No Unreal code may inspect its contents. */
struct FSceneAutoTaggerEvidenceDescriptor
{
	TArray<uint8> NativeDescriptorBytes;
};

/** Caller-owned facts supplied to the protected rendered-information policy. */
struct FSceneAutoTaggerRenderedInformationContext
{
	float SignificanceScore = 0.0f;
	uint32 SignificanceReasonFlags = 0;
	int32 PrimitiveCount = 0;
	bool bSingleCandidateRequest = false;
	bool bReviewBatchRequest = false;
	bool bForceExplicitScope = false;
	bool bOpposedBroadFacePresentation = false;
	bool bHasExistingObject = false;
};

/** Builds bounded native-core view candidates from Unreal component proxies and presentation hints. */
CONVAISCENETAGGING_API FSceneAutoTaggerViewPlan BuildActorLocalViewPlan(
	const AActor& Actor,
	const TArray<UPrimitiveComponent*>& Components);

/** One probe render, parallel to the plan's candidate at the same index.
 *  bPresent=false represents a failed capture and keeps the slot. */
struct FSceneAutoTaggerProbeImage
{
	bool bPresent = false;
	TArray<FColor> Pixels;
	int32 Width = 0;
	int32 Height = 0;
	FColor Background = FColor::Black;
	int32 BackgroundEpsilon = 8;
	TArray<uint8> GeometryMask; // empty = none
};

struct FSceneAutoTaggerSelectViewResult
{
	int32 SelectedIndex = INDEX_NONE;
	float SelectionMargin = 0.0f;
	float SelectedScore = -1.0f;
	TArray<FSceneAutoTaggerViewMetrics> Metrics; // parallel to probes; absent probes yield bValid=false entries
};

/** Analyzes and ranks all probes in one native call. False = core unavailable or invalid input. */
CONVAISCENETAGGING_API bool SelectCapturedView(
	const FSceneAutoTaggerViewPlan& Plan,
	const TArray<FSceneAutoTaggerProbeImage>& Probes,
	FSceneAutoTaggerSelectViewResult& OutResult);

struct FSceneAutoTaggerCaptureFacts
{
	float SignificanceScore = 0.0f;
	int32 PrimitiveCount = 0;
	uint32 NativeSignificanceReasonFlags = 0; // CONVAI_SAT_SIGNIFICANCE_REASON_* bit values
	bool bSingleCandidateRequest = false;
	bool bReviewBatchRequest = false;
	bool bForceExplicitScope = false;
	bool bOpposedBroadFace = false;
	bool bHasExistingObject = false;
};

struct FSceneAutoTaggerCaptureEvaluation
{
	bool bRetain = true; // fail-closed
	bool bExcludedForVisibility = false;      // reason OBJECT_NOT_CLEARLY_VISIBLE
	bool bExcludedForNoUsefulDetail = false;  // reason NO_USEFUL_VISUAL_DETAIL
	bool bReadableCenteredCoverage = false;
	bool bInformativeEvidence = false;
	bool bMeaningfulMidScaleStructure = false;
	bool bDescriptorValid = false;
	FSceneAutoTaggerEvidenceDescriptor Descriptor;
};

/** Final-capture eligibility + evidence descriptor in one native call.
 *  ProbeMetrics is the Metrics array from SelectCapturedView (may be empty).
 *  False = core unavailable or invalid input; OutEvaluation stays retained. */
CONVAISCENETAGGING_API bool EvaluateFinalCapture(
	const TArray<uint8>& BGRA,
	const FIntPoint& Size,
	const TArray<FSceneAutoTaggerViewMetrics>& ProbeMetrics,
	const FSceneAutoTaggerCaptureFacts& Facts,
	FSceneAutoTaggerCaptureEvaluation& OutEvaluation);

/** Builds an opaque native descriptor. Low-information captures are rejected. */
CONVAISCENETAGGING_API bool BuildCapturedViewDescriptor(
	const TArray<uint8>& BGRA,
	const FIntPoint& Size,
	FSceneAutoTaggerEvidenceDescriptor& OutDescriptor);

/** Compares two opaque descriptors in the native core. */
CONVAISCENETAGGING_API bool AreCapturedViewDescriptorsVisuallyEquivalent(
	const FSceneAutoTaggerEvidenceDescriptor& Left,
	const FSceneAutoTaggerEvidenceDescriptor& Right);

/** ABI and native policy identity. Kept separate from Unreal capture policy. */
CONVAISCENETAGGING_API FString NativeCorePolicyId();

/** Unreal-side capture/input preparation policy only. */
CONVAISCENETAGGING_API const TCHAR* CapturePolicyVersion();

/** Unreal capture/input policy plus the contact-sheet dimensions. */
CONVAISCENETAGGING_API FString BuildCapturePolicyId(
	int32 EvidenceCaptureResolution,
	int32 ModelCellResolution,
	int32 GridDimension);

#if WITH_DEV_AUTOMATION_TESTS
/** Direct checks for the documented Unreal-left-handed/native-right-handed ABI boundary. */
CONVAISCENETAGGING_API FVector ConvertUnrealToNativeCoordinatesForTesting(const FVector& UnrealVector);
CONVAISCENETAGGING_API FVector ConvertNativeToUnrealCoordinatesForTesting(const FVector& NativeVector);
#endif
}
