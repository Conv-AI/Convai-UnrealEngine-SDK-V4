#pragma once

#include "CoreMinimal.h"

class UPrimitiveComponent;
class UTextureRenderTarget2D;
class UWorld;

namespace ConvaiSceneAutoTagger
{
/** Destroys the transient capture actor and lights. Must be called on the game thread. */
CONVAISCENETAGGING_API void CleanupCaptureResources();

class FLiveObjectCaptureSession;

/**
 * Starts a fixed-resolution live isolated capture session.
 *
 * Begin allocates the target once and starts asynchronous every-editor-frame
 * rendering. Interactive Edit View calibrates immediately. Automatic capture can
 * defer calibration until its texture/mesh streaming warm-up has completed.
 * Orbit/pan/zoom updates are allocation-free and do not read pixels; the next
 * synchronous readback occurs only when the user commits.
 */
CONVAISCENETAGGING_API TUniquePtr<FLiveObjectCaptureSession> BeginLiveObjectCapture(
	UWorld* World,
	const TArray<UPrimitiveComponent*>& ObjectComponents,
	const FBox& WorldBounds,
	int32 Resolution,
	FString& OutError,
	const FVector& ViewDirection = FVector(-1.0, -1.0, -0.65),
	float DistanceScale = 1.0f,
	const FVector2D& TargetOffset = FVector2D::ZeroVector,
	bool bSuppressDirectSpecularHighlights = false,
	bool bAllowSuppressedSpecularRecovery = false,
	bool bCalibrateOnStart = true,
	bool bRenderFromCallerTick = false);

/**
 * Fixed-resolution isolated capture session used only by the explicit Edit View UI.
 *
 * The transient capture actor, optional recovery lights, and render target are created once at
 * BeginLiveObjectCapture and remain stable while UpdateView changes only their
 * transforms. The render target is suitable for direct Slate display; callers must
 * ignore its texture alpha because FinalColorLDR defines only RGB.
 *
 * Every method, including destruction, must run on the game thread. End is
 * idempotent and is also called by the destructor.
 */
class CONVAISCENETAGGING_API FLiveObjectCaptureSession final
{
public:
	~FLiveObjectCaptureSession();

	FLiveObjectCaptureSession(const FLiveObjectCaptureSession&) = delete;
	FLiveObjectCaptureSession& operator=(const FLiveObjectCaptureSession&) = delete;
	FLiveObjectCaptureSession(FLiveObjectCaptureSession&&) = delete;
	FLiveObjectCaptureSession& operator=(FLiveObjectCaptureSession&&) = delete;

	/** Returns the stable display-encoded render target updated by the live scene capture. */
	UTextureRenderTarget2D* GetRenderTarget() const;

	/** Returns the fixed square target resolution selected when the session began. */
	int32 GetResolution() const;

	/** Returns the editor world this session is bound to, or null after End. */
	UWorld* GetWorld() const;

	/** True while the actor, target, world, and show-only object remain usable. */
	bool IsValid() const;

	/**
	 * Updates the live camera and fallback-light transforms without allocating or
	 * synchronously capturing. The continuously ticking capture renders the newest
	 * transform on its next editor frame.
	 */
	bool UpdateView(
		const FVector& ViewDirection,
		float DistanceScale,
		const FVector2D& TargetOffset,
		FString& OutError);

	/**
	 * Renders the current view for a caller-driven session. This keeps Edit View
	 * responsive even when the Level viewport is hidden or its world tick is paused.
	 * Multiple calls in one engine frame are coalesced.
	 */
	bool RenderFrame(FString& OutError);

	/**
	 * Produces camera-coherent opaque BGRA bytes when the user commits the view. Continuous
	 * capture is paused, one exact frame is captured/read, then live capture resumes
	 * so a failed downstream save can remain in Edit View.
	 */
	bool ReadCurrentPixels(TArray<FColor>& OutBGRA, FString& OutError);

	/**
	 * Re-runs the bounded lighting/exposure calibration against the session's
	 * current, already-warmed render resources. Automatic capture uses this after
	 * its asynchronous streaming warm-up; Edit View normally calibrates at start.
	 */
	bool CalibrateReadability(FString& OutError);

	/** Stops rendering and destroys all transient world resources. Idempotent. */
	void End();

private:
	struct FImpl;
	explicit FLiveObjectCaptureSession(TUniquePtr<FImpl>&& InImpl);
	friend CONVAISCENETAGGING_API TUniquePtr<FLiveObjectCaptureSession> BeginLiveObjectCapture(
		UWorld*,
		const TArray<UPrimitiveComponent*>&,
		const FBox&,
		int32,
		FString&,
		const FVector&,
		float,
		const FVector2D&,
		bool,
		bool,
		bool,
		bool);

	TUniquePtr<FImpl> Impl;
};

/** Exact clear color used by isolated captures, for deterministic view-quality analysis. */
CONVAISCENETAGGING_API FColor GetCaptureBackgroundColor();

/**
 * True when a display-encoded pixel belongs to the isolated capture backdrop.
 *
 * Some render paths encode the same neutral near-black backdrop as either the
 * configured clear value or zero. Keeping this predicate shared prevents the
 * lighting evaluator, view selector, and retained-evidence gate from disagreeing
 * about the apparent foreground coverage.
 */
CONVAISCENETAGGING_API bool IsCaptureBackgroundPixel(
	const FColor& Pixel,
	const FColor& EncodedBackdrop,
	int32 ColorEpsilon = 8);

/** Bounded async residency gate used before automatic evidence calibration. */
CONVAISCENETAGGING_API bool ShouldContinueAutomaticCaptureWarmup(
	int32 DistinctFrames,
	double ElapsedSeconds,
	int32 PendingConventionalAssets);

/**
 * Short bounded residency gate before the low-resolution angle probes begin.
 * This prevents the first direction from being judged in the same frame that
 * its mesh, textures, and materials were requested.
 */
CONVAISCENETAGGING_API bool ShouldContinueAutomaticProbeWarmup(
	int32 DistinctFrames,
	double ElapsedSeconds,
	int32 PendingRenderWork);

/** Advances the ordered seed-then-score probe loop; true means scoring is complete. */
CONVAISCENETAGGING_API bool AdvanceAutomaticProbePass(
	int32 ProbeCount,
	int32& InOutProbeCursor,
	bool& bInOutSeedingPass);

/** Conventional capture assets are ready only after the manager reports full residency. */
CONVAISCENETAGGING_API bool IsConventionalCaptureAssetReady(bool bHasPendingUpdate, bool bFullyStreamedIn);

/** True after the selected recovery profile has rendered across two real frames. */
CONVAISCENETAGGING_API bool HasAutomaticCaptureSettledAfterCalibration(int32 DistinctFrames);

/**
 * Display-space measurements used by the bounded capture-light recovery policy.
 *
 * This lives in the module-private header so deterministic automation tests can
 * exercise the decision policy without starting an editor render pass.
 */
struct FCapturePresentationMetrics
{
	int32 ForegroundSamples = 0;
	float ForegroundFraction = 0.0f;
	float ShoulderFraction = 0.0f;
	float ClippedFraction = 0.0f;
	float LowChromaFraction = 0.0f;
	float MeanLuminance = 0.0f;
	float P10Luminance = 0.0f;
	float P25Luminance = 0.0f;
	float MedianLuminance = 0.0f;
	float P90Luminance = 0.0f;
	float ToneSpan = 0.0f;
	float InteriorDetail = 0.0f;
};

/**
 * Accepts a dimensional key-light probe when a near-black foreground mask keeps
 * its median pinned to the backdrop but the probe safely reveals upper-tone
 * structure. Specular-suppressed non-metallic panels are excluded explicitly.
 */
CONVAISCENETAGGING_API bool PreferSevereDarkDimensionalKeyRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery,
	bool bSuppressDirectSpecularHighlights,
	bool bAllowSuppressedSpecularRecovery);

/** True when a retained single-fill image warrants one opposing diffuse probe. */
CONVAISCENETAGGING_API bool ShouldAttemptOpposingDiffuseRecovery(
	const FCapturePresentationMetrics& Metrics,
	bool bSingleDiffuseFillRetained,
	bool bSuppressDirectSpecularHighlights);

/**
 * True only when an opposing diffuse probe lifts the dark quarter while keeping
 * highlights and structural detail within conservative bounds.
 */
CONVAISCENETAGGING_API bool PreferOpposingDiffuseRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery);

/**
 * True when a dimensional subject has a readable highlight range but an
 * underexposed lower quarter, warranting one gentle two-sided balance probe.
 */
CONVAISCENETAGGING_API bool ShouldAttemptDimensionalShadowBalanceRecovery(
	const FCapturePresentationMetrics& Metrics,
	bool bSuppressDirectSpecularHighlights);

/**
 * Retains a two-sided balance probe only when it lifts shadow information while
 * preserving highlight headroom and rendered detail.
 */
CONVAISCENETAGGING_API bool PreferDimensionalShadowBalanceRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery);

/** True when a dimensional subject remains below an absolute readability floor. */
CONVAISCENETAGGING_API bool ShouldAttemptDimensionalReadabilityFloorRecovery(
	const FCapturePresentationMetrics& Metrics,
	bool bSuppressDirectSpecularHighlights);

/** Retains the one bounded floor probe only when it safely reveals more subject detail. */
CONVAISCENETAGGING_API bool PreferDimensionalReadabilityFloorRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery);

/**
 * True when a specular-suppressed broad face remains too dark for useful visual
 * evidence and warrants one diffuse-fill plus bounded-exposure probe.
 */
CONVAISCENETAGGING_API bool ShouldAttemptPlanarDiffuseExposureRecovery(
	const FCapturePresentationMetrics& Metrics,
	bool bSuppressDirectSpecularHighlights,
	bool bAllowSuppressedSpecularRecovery);

/** Exposure offset for the planar diffuse probe, bounded by measured highlight headroom. */
CONVAISCENETAGGING_API float ComputePlanarDiffuseExposureBias(const FCapturePresentationMetrics& Metrics);

/** Accepts a planar diffuse-exposure probe only when it lifts lower tones safely. */
CONVAISCENETAGGING_API bool PreferPlanarDiffuseExposureRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery);

/** Default contact-sheet layout used by the vision request controller. */
inline constexpr int32 DefaultGridDimension = 4;
inline constexpr int32 DefaultGridCellPixels = 384;
inline constexpr int32 DefaultGridOutputPixels = 1536;

/**
 * Validate, downsample, and encode a rendered Level Editor viewport buffer for
 * the explicit scene-context draft action. The image remains in memory only.
 */
CONVAISCENETAGGING_API bool PrepareViewportContextPng(
	const TArray<FColor>& SourceBGRA,
	int32 SourceWidth,
	int32 SourceHeight,
	int32 MaximumLongEdge,
	TArray64<uint8>& OutPngBytes,
	FString& OutError);

/**
 * Capture only the active Level Editor viewport render target (never Slate or
 * the desktop), then prepare an in-memory PNG with a maximum 1024-pixel edge.
 */
CONVAISCENETAGGING_API bool CaptureActiveLevelViewportPng(
	TArray64<uint8>& OutPngBytes,
	FString& OutError,
	int32 MaximumLongEdge = 1024);

/**
 * Captures only the supplied primitives into a square BGRA8 cell.
 *
 * The editor viewport is not touched and no actor/component visibility or material is changed.
 * This function is synchronous and must be called on the game thread.
 */
CONVAISCENETAGGING_API bool CaptureObjectCell(
	UWorld* World,
	const TArray<UPrimitiveComponent*>& ObjectComponents,
	const FBox& WorldBounds,
	int32 CellPixels,
	TArray<FColor>& OutBGRA,
	FString& OutError,
	const FVector& ViewDirection = FVector(-1.0, -1.0, -0.65),
	float DistanceScale = 1.0f,
	const FVector2D& TargetOffset = FVector2D::ZeroVector,
	bool bSuppressDirectSpecularHighlights = false,
	bool bAllowSuppressedSpecularRecovery = false,
	bool bReadinessPrimeOnly = false,
	TArray<uint8>* OutGeometryMask = nullptr);

#if WITH_DEV_AUTOMATION_TESTS
/** Game-thread test seam, scoped to World. End that world's live sessions before switching; nullptr clears the override. */
CONVAISCENETAGGING_API void SetNeutralCaptureLightingForTests(UWorld* World, bool bEnabled);
#endif

/**
 * Builds a numbered square contact sheet and encodes it as PNG.
 *
 * OccupiedCells contains occupied cells only, in prompt order. Its Num() is FilledCount.
 * Cells are labeled 1..FilledCount; all remaining slots through GridN*GridN stay blank.
 */
CONVAISCENETAGGING_API bool CompositeGridPng(
	const TArray<TArray<FColor>>& OccupiedCells,
	int32 GridN,
	int32 CellPixels,
	int32 OutputPixels,
	TArray64<uint8>& OutPngBytes,
	FString& OutError);

/** Encodes a tightly packed FColor/BGRA8 pixel buffer as PNG. */
CONVAISCENETAGGING_API bool EncodePng(
	const TArray<FColor>& BGRA,
	int32 Width,
	int32 Height,
	TArray64<uint8>& OutPngBytes,
	FString& OutError);

/** Writes already encoded PNG bytes, creating the parent directory when necessary. */
CONVAISCENETAGGING_API bool SavePngDebug(
	const FString& AbsoluteFilePath,
	const TArray64<uint8>& PngBytes,
	FString& OutError);

/** Encodes and writes a BGRA8 buffer for capture diagnostics. */
CONVAISCENETAGGING_API bool SavePixelsPngDebug(
	const FString& AbsoluteFilePath,
	const TArray<FColor>& BGRA,
	int32 Width,
	int32 Height,
	FString& OutError);
}
