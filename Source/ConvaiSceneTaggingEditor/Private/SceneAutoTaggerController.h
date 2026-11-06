// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorUndoClient.h"
#include "SceneAutoTaggerDuplicateClustering.h"
#include "SceneAutoTaggerTypes.h"
#include "SceneAutoTaggerViewSelection.h"

namespace ConvaiSceneAutoTagger
{
	class FSceneAutoTaggerCache;
}
class UWorld;

/** Outcome of appending an explicit Level Editor selection to an existing review. */
struct FSceneAutoTaggerAddToReviewResult
{
	int32 Added = 0;
	int32 Restored = 0;
	int32 Skipped = 0;
};

/** Owns one resumable editor exploration run and is the sole mutation surface used by Slate. */
class FSceneAutoTaggerController : public TSharedFromThis<FSceneAutoTaggerController>, public FEditorUndoClient
{
public:
	FSceneAutoTaggerController();
	~FSceneAutoTaggerController();

	void StartExploration(UWorld* World, const FSceneAutoTaggerRunOptions& InOptions);
	void Cancel();
	void ClearResults();
	void ApplyAccepted();
	bool ShouldGenerateMovementPointsOnApply() const
	{
		return bGenerateMovementPointsOnApply;
	}
	void SetGenerateMovementPointsOnApply(bool bGenerate);
	/** Hides unapplied rows for this review without deleting actors or changing candidate indices. */
	int32 DismissCandidatesFromReviewByActorPath(const TSet<FString>& CandidateActorPaths);

	/** True only when this live, complete, named, unapplied row can enter the included state. */
	bool CanIncludeCandidate(const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate) const;
	/** Sets one review decision across eligible actor paths, broadcasts at most once, and returns changed rows. */
	int32 SetDecisionsByActorPath(
		const TSet<FString>& CandidateActorPaths,
		ESceneAutoTaggerDecision Decision);
	void AcceptAtOrAbove(float MinimumConfidence);
	void SetAllDecisions(ESceneAutoTaggerDecision Decision);
	/** Resets review rows whose previously applied components were removed in the editor. */
	void NotifyAppliedComponentsRemoved(const TSet<FString>& ActorPaths);
	void NotifyCandidateEdited(const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate);
	bool ReplaceCandidateEvidence(
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate,
		const TArray<FColor>& Pixels,
		int32 Resolution,
		const FVector& ViewDirection,
		float DistanceScale,
		const FVector2D& TargetOffset,
		FString& OutError);
	bool RedescribeCandidate(
		const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate,
		FString& OutError);
	/** Analyzes the retained evidence of every eligible selected review row in bounded contact-sheet batches. */
	bool AnalyzeCandidatesByActorPath(
		const TSet<FString>& CandidateActorPaths,
		FString& OutError);
	/** Restores selected automatic low-information skips and analyzes their retained images. */
	bool AnalyzeLowInformationCandidatesByActorPath(
		const TSet<FString>& CandidateActorPaths,
		FString& OutError);
	/** Captures fresh automatic evidence, then analyzes every eligible selected review row in bounded contact-sheet batches. */
	bool RecaptureAndAnalyzeCandidatesByActorPath(
		const TSet<FString>& CandidateActorPaths,
		FString& OutError);
	/**
	 * Discovers and analyzes the supplied actors without clearing the current review.
	 * The active review's frozen context, focus, quality, and existing-object policy are reused.
	 */
	bool AddActorsToCurrentReview(
		const TArray<TWeakObjectPtr<AActor>>& Actors,
		FSceneAutoTaggerAddToReviewResult& OutResult,
		FString& OutError);

	const TArray<TSharedPtr<FSceneAutoTaggerCandidate>>& GetCandidates() const { return Candidates; }
	const FString& GetRunVisionCharacterID() const { return RunOptions.VisionCharacterID; }
	const FString& GetRunVisionCharacterName() const { return RunOptions.VisionCharacterName; }
	const FString& GetRunSceneContext() const { return RunOptions.SceneContext; }
	const FString& GetRunDescriptionFocus() const { return RunOptions.DescriptionFocus; }
	/** Capture resolution that avoids upsampling an edited view before provider analysis. */
	int32 GetEditableCaptureResolution() const
	{
		return FMath::Max(GetRunEvidencePreviewResolution(), GetRunAnalysisCellResolution());
	}
	const FSceneAutoTaggerProgress& GetProgress() const { return Progress; }
	const FString& GetLastError() const { return LastError; }
	const FString& GetLastApplySummary() const { return LastApplySummary; }
	/** Live automatic skips, suitable for the completion summary list. */
	TArray<TSharedPtr<FSceneAutoTaggerCandidate>> GetLowInformationFilteredCandidates() const;
	int32 CountLowInformationFilteredCandidates() const;
	/** Visible rows whose capture or provider analysis needs explicit user attention. */
	TArray<TSharedPtr<FSceneAutoTaggerCandidate>> GetAttentionCandidates() const;
	int32 CountAttentionCandidates() const;
	bool ShouldShowCompletionSummary() const;
	void AcknowledgeCompletionSummary();
	void ReopenCompletionSummary();
	/** Keeps same-world review state, hides deleted actors, and clears state from another editor world. */
	void ValidateRetainedReviewState(UWorld* CurrentWorld);
	int32 CountDecision(ESceneAutoTaggerDecision Decision) const;
	/** Included, complete, live rows that have not already been written to the scene. */
	int32 CountReadyToApply() const;
	int32 CountApplied() const;
	bool IsBusy() const;
	bool CanApply() const;
	bool HasReviewResults() const;
	/** True when the pinned native analysis component loaded and passed ABI validation. */
	bool IsNativeCoreReady() const;
	/** Short user-facing recovery text; technical loader details are written to the log. */
	FString GetNativeCoreDiagnostic() const;

	FSimpleMulticastDelegate& OnChanged() { return ChangedDelegate; }

	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FSceneAutoTaggerControllerTestPeer;
#endif
	enum class EReviewBatchMode : uint8
	{
		None,
		RetainedEvidence,
		FreshCapture
	};

	struct FActiveBatch;

	void SetState(ESceneAutoTaggerState State, const FText& Message);
	bool HasMatchingAppliedComponent(
		const FSceneAutoTaggerCandidate& Candidate,
		int32 CandidateIndex) const;
	void ReconcileAppliedComponentsFromEditor();
	void BroadcastChanged();
	bool IsActiveEditorWorldCurrent() const;
	bool EnsureNativeCoreReady(FString& OutError) const;
	void AbortRun(const FString& Error, const FText& Message);
	void BuildFingerprintGroupsAndLoadCache();
	void ProcessNextBatch();
	void ScheduleNextCaptureBatch();
	void CaptureBatch(
		const TArray<int32>& RepresentativeIndices,
		const TArray<int32>& ContextIndices);
	void CaptureNextCell(FGuid CaptureRunId);
	void FinishCaptureBatch();
	void PumpAnalysisQueue();
	void SubmitAnalysisBatch(const TSharedPtr<FActiveBatch>& Batch);
	void HandleVisionResponse(
		FGuid CallbackRunId,
		FGuid CallbackBatchId,
		bool bSuccess,
		int32 ResponseCode,
		struct FSceneAutoTaggerVisionResponse&& Response,
		FString&& Error);
	/** True when this run has an explicit picker-selected character; fills the failure state otherwise. */
	bool EnsureVisionCharacterReady(FString& OutError) const;
	void FinishAnalysisBatchWithFailure(
		const TSharedPtr<FActiveBatch>& Batch,
		const FString& Error,
		bool bTransientRetryEligible = false);
	void CompleteAnalysisBatch(const TSharedPtr<FActiveBatch>& Batch);
	void TryFinishExploration();
	void FinishExploration();
	void ApplyResultToRepresentative(int32 CandidateIndex, const struct FSceneAutoTaggerVisionItem& Item);
	void MarkRepresentativeFailed(int32 CandidateIndex, const FString& Error);
	void FanOutRepresentative(int32 RepresentativeIndex, ESceneAutoTaggerSource DuplicateSource);
	void RebuildDuplicateGroupsFromFingerprints();
	void ReconcileVisualDuplicates();
	/** Converts pending low-information paths into index-preserving hidden review records. */
	void RemoveVisuallyRejectedCandidates();
	bool RestoreSingleCandidateRecaptureSnapshot();
	bool StartReviewBatch(
		const TSet<FString>& CandidateActorPaths,
		EReviewBatchMode Mode,
		bool bReconcileAllVisualDuplicates,
		FString& OutError);
	void MergeDiscoveredCandidatesIntoReview(
		const TArray<TSharedPtr<FSceneAutoTaggerCandidate>>& DiscoveredCandidates,
		TSet<FString>& OutAnalysisActorPaths,
		TArray<int32>& OutAddedCandidateIndices,
		TMap<int32, FSceneAutoTaggerCandidate>& OutRestoredSnapshots,
		FSceneAutoTaggerAddToReviewResult& OutResult);
	bool BuildRetainedAnalysisBatches(
		const TArray<int32>& CandidateIndices,
		TArray<TSharedPtr<FActiveBatch>>& OutBatches,
		TArray<int32>& OutPreparedCandidateIndices,
		FString& OutError) const;
	void SnapshotReviewBatchCandidates(const TArray<int32>& CandidateIndices);
	bool RestoreReviewBatchCandidate(int32 CandidateIndex);
	void RestoreAllReviewBatchCandidates();
	bool CommitReviewBatchResult(
		int32 CandidateIndex,
		uint32 ExpectedPreviewRevision,
		const struct FSceneAutoTaggerVisionItem& Item);
	void PreserveReviewBatchCandidate(int32 CandidateIndex, const FString& Error);
	void FinishReviewBatch();
	void ResetReviewBatchState();
	bool IsReviewBatchRequest() const { return ReviewBatchMode != EReviewBatchMode::None; }
	int32 GetRunAnalysisGridDimension() const;
	int32 GetRunAnalysisCellResolution() const;
	int32 GetRunEvidencePreviewResolution() const;
	void SaveRepresentativeToCache(int32 RepresentativeIndex);
	void EnsureUniqueAcceptedNames();
	void EnsureUniqueAcceptedNames(
		const ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan& ClusteringPlan);
	static FString BuildEchoId(const FSceneAutoTaggerCandidate& Candidate, int32 CellIndex);
	static void BuildPreview(FSceneAutoTaggerCandidate& Candidate, const TArray<FColor>& CellPixels, int32 CellResolution);

	TArray<TSharedPtr<FSceneAutoTaggerCandidate>> Candidates;
	TArray<int32> PendingRepresentatives;
	TSharedPtr<FActiveBatch> ActiveBatch;
	TArray<TSharedPtr<FActiveBatch>> PendingAnalysisBatches;
	TMap<FGuid, TSharedPtr<FActiveBatch>> InFlightAnalysisBatches;
	/** Actor paths rejected by the conservative retained-image information gate during this run. */
	TSet<FString> VisuallyRejectedActorPaths;
	TMap<FString, ESceneAutoTaggerLowInformationReason> VisuallyRejectedReasonsByActorPath;
	/** Core-evaluated evidence descriptors, reusable only while the stored preview revision matches. */
	TMap<FString, TPair<uint32, ConvaiSceneAutoTagger::FSceneAutoTaggerEvidenceDescriptor>>
		EvidenceDescriptorsByActorPath;
	/** Rows this controller applied or explicitly observed being removed; limits Undo/Redo reconciliation. */
	TSet<FString> UndoTrackedAppliedActorPaths;
	/** Frozen on the first successful apply boundary so later partial applies cannot regroup old rows. */
	TUniquePtr<ConvaiSceneAutoTagger::FSceneAutoTaggerDuplicateClusteringPlan>
		FrozenDuplicateClusteringPlan;
	TUniquePtr<ConvaiSceneAutoTagger::FSceneAutoTaggerCache> Cache;
	TWeakObjectPtr<UWorld> ActiveWorld;
	FSceneAutoTaggerRunOptions RunOptions;
	FSceneAutoTaggerProgress Progress;
	FGuid ActiveRunId;
	FString LastError;
	FString LastApplySummary;
	int32 NextRepresentative = 0;
	int32 CompletedRepresentatives = 0;
	bool bCancelRequested = false;
	bool bCaptureComplete = false;
	/** Review-local, opt-in Apply companion; Include itself remains non-mutating. */
	bool bGenerateMovementPointsOnApply = false;
	bool bCompletionSummaryAcknowledged = false;
	bool bSingleCandidateRequest = false;
	int32 SingleCandidateIndex = INDEX_NONE;
	/** Atomic rollback for fresh single-row capture/analysis; ordinary Analyze again does not use it. */
	TUniquePtr<FSceneAutoTaggerCandidate> SingleCandidateRecaptureSnapshot;
	/** Review-only execution mode; unlike exploration, it never reconciles, filters, caches, or fans out duplicates. */
	EReviewBatchMode ReviewBatchMode = EReviewBatchMode::None;
	/** Per-row rollback snapshots retained until that row succeeds or is preserved after failure/cancellation. */
	TMap<FString, TUniquePtr<FSceneAutoTaggerCandidate>> ReviewBatchSnapshots;
	/** Evidence revision submitted for each row; prevents a stale callback from overwriting newer local evidence. */
	TMap<FString, uint32> ReviewBatchExpectedPreviewRevisions;
	TSet<FString> CompletedReviewBatchActorPaths;
	int32 ReviewBatchSucceededCount = 0;
	int32 ReviewBatchPreservedCount = 0;
	/** Incremental additions cannot rewrite prior reviewed/applied rows through global visual reconciliation. */
	bool bReviewBatchReconcileAllVisualDuplicates = true;
	FSimpleMulticastDelegate ChangedDelegate;
};
