// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneAutoTaggerTypes.h"
#include "Widgets/SCompoundWidget.h"

class FSceneAutoTaggerController;
class USceneAutoTaggerCharacterCatalog;
class FSceneAutoTaggerSetupModel;
class FDeferredCleanupSlateBrush;
struct FSceneAutoTaggerCharacterOption;
struct FSlateDynamicImageBrush;
class ISceneAutoTaggerRequest;
class ITableRow;
class SButton;
class SComboButton;
class SBox;
class SEditableTextBox;
class SImage;
class SScrollBox;
class SSearchBox;
class STableViewBase;
class STextBlock;
class UTexture2D;
template <typename ItemType> class SListView;
namespace ConvaiSceneAutoTagger { class FLiveObjectCaptureSession; }

/** Native review surface for one editor-only scene exploration controller. */
class SConvaiSceneAutoTagger final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SConvaiSceneAutoTagger) {}
		SLATE_ARGUMENT(TSharedPtr<FSceneAutoTaggerController>, Controller)
	SLATE_END_ARGS()

	SConvaiSceneAutoTagger();
	void Construct(const FArguments& InArgs);
	virtual ~SConvaiSceneAutoTagger() override;
	virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override;
	/** Resolve Slate's requested header state against the currently rendered tri-state value. */
	static bool ShouldCheckAllVisibleRows(ECheckBoxState CurrentState, ECheckBoxState RequestedState);
	/** Apply the header transition only to the eligible paths shown by the current filter. */
	static void ApplyVisibleBatchSelection(
		ECheckBoxState CurrentState,
		ECheckBoxState RequestedState,
		const TSet<FString>& VisibleEligiblePaths,
		TSet<FString>& InOutSelectedPaths);
	/** Resolve a stable right-click target without consulting or mutating checkbox selection. */
	static TSet<FString> ResolveContextActionPaths(
		const TArray<TSharedPtr<FSceneAutoTaggerCandidate>>& HighlightedCandidates,
		const TSharedPtr<FSceneAutoTaggerCandidate>& PressedCandidate);

private:
	using FCandidatePtr = TSharedPtr<FSceneAutoTaggerCandidate>;

	enum class EExplorationScope : uint8
	{
		CurrentLevel,
		SelectedActors
	};

	TSharedRef<SWidget> BuildCompactSetupPanel();
	TSharedRef<SWidget> BuildScopeTogglePair(FName CurrentLevelTag, FName SelectedActorsTag);
	TSharedRef<SWidget> BuildAnalysisQualityTogglePair();
	TSharedRef<SWidget> BuildSettingsMenu();
	TSharedRef<SWidget> BuildSceneContextMenu();
	TSharedRef<SWidget> BuildBusyOverlay();
	TSharedRef<SWidget> BuildCompletionSummaryOverlay();
	TSharedRef<SWidget> BuildConsentOverlay();
	TSharedRef<SWidget> BuildReviewPanel();
	TSharedRef<SWidget> BuildEmptyStatePanel();
	TSharedRef<SWidget> BuildCharacterSelectionPanel();
	TSharedRef<SWidget> BuildGuidancePanel();
	TSharedRef<SWidget> BuildCharacterGrid();
	TSharedRef<SWidget> BuildCharacterCard(const FSceneAutoTaggerCharacterOption& Character);
	const FSlateBrush* GetCharacterAvatarBrush(const FString& AvatarUrl) const;
	TSharedRef<SWidget> BuildCandidateListPanel();
	TSharedRef<SWidget> BuildEvidencePanel();
	TSharedRef<SWidget> BuildCandidateDetailsPanel();
	TSharedRef<SWidget> BuildRefinementPanel();
	TSharedRef<SWidget> BuildBatchActionsMenu();
	TSharedPtr<SWidget> BuildCandidateContextMenu();
	TSharedRef<SWidget> BuildErrorBanner();
	TSharedRef<SWidget> BuildStickyFooter();

	TSharedRef<ITableRow> GenerateCandidateRow(FCandidatePtr Candidate, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> GenerateLowInformationCandidateRow(
		FCandidatePtr Candidate,
		const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> GenerateAttentionCandidateRow(
		FCandidatePtr Candidate,
		const TSharedRef<STableViewBase>& OwnerTable);
	FReply HandleCandidateRowPressed(FCandidatePtr Candidate, const FPointerEvent& PointerEvent);
	void HandleCandidateClicked(FCandidatePtr Candidate);
	FReply HandleCandidateListKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	void HandleCandidateSelectionChanged(FCandidatePtr Candidate, ESelectInfo::Type SelectInfo);
	void HandleCandidateDoubleClicked(FCandidatePtr Candidate);
	void HandleControllerChanged();
	void HandleSearchChanged(const FText& NewText);
	void RefreshCandidateList();
	void RefreshCompletionSummaryLists();
	bool CandidateMatchesSearch(const FSceneAutoTaggerCandidate& Candidate) const;

	FReply HandleExplore();
	FReply HandleRefreshCharacters();
	FReply HandleLoadMoreCharacters();
	FReply HandleOpenCharacterInConvai();
	void HandleCharacterSearchChanged(const FText& NewText);
	void HandleCharacterSelected(FString CharacterID);
	void RefreshCharacterCatalog();
	void RefreshCharacterGrid();
	FReply HandleCancel();
	FReply HandleReturnToSetup();
	FReply HandleClearResults();
	FReply HandleManageAppliedObjects();
	FReply HandleDraftSceneContextFromViewport();
	FReply HandleCancelSceneContextDraft();
	FReply HandleConfigureAgent();
	FReply HandleRefreshAgentStatus();
	FReply HandleAcceptPrivacy();
	FReply HandleDeclinePrivacy();
	FReply HandleReopenCompletionSummary();
	FReply HandleContinueFromCompletionSummary();
	FReply HandleAnalyzeLowInformationSelection();
	FReply HandleFocusLowInformationCandidate(FCandidatePtr Candidate);
	FReply HandleInspectAttentionCandidate(FCandidatePtr Candidate);
	void HandleLowInformationSelectionChanged(FCandidatePtr Candidate, ECheckBoxState NewState);
	FReply HandleSelectActor();
	FReply HandleFocusActor();
	FReply HandleBeginEditView();
	FReply HandleCancelEditView();
	FReply HandleResetEditView();
	FReply HandleUseEditedView();
	FReply HandleUseAndAnalyzeEditedView();
	void HandleEditViewOrbit(FVector2D DragDelta);
	void HandleEditViewPan(FVector2D DragDelta);
	void HandleEditViewZoom(float WheelDelta);
	void HandleCandidateBatchSelectionChanged(FCandidatePtr Candidate, ECheckBoxState NewState);
	void HandleVisibleBatchSelectionChanged(ECheckBoxState NewState);
	FReply HandleBatchDecision(ESceneAutoTaggerDecision Decision);
	FReply HandleAnalyzeBatchSelection();
	FReply HandleRecaptureAndAnalyzeBatchSelection();
	FReply HandleClearBatchSelection();
	FReply HandleAddSelectedActorsToReview();
	FReply HandleDecisionForActorPaths(
		const TSet<FString>& ActorPaths,
		ESceneAutoTaggerDecision Decision,
		bool bClearMatchingChecks);
	FReply HandleAnalyzeActorPaths(const TSet<FString>& ActorPaths);
	FReply HandleRecaptureAndAnalyzeActorPaths(const TSet<FString>& ActorPaths);
	FReply HandleRemoveCandidatesFromReview(const TSet<FString>& ActorPaths);
	FReply HandleAcceptAtOrAbove();
	FReply HandleResetAll();
	FReply HandleApplyAccepted();
	FReply HandleAnalyzeWithRefinementNote();
	FReply HandleClearRefinementNote();

	void HandleNameChanged(const FText& NewText);
	void HandleNameCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void HandleDescriptionChanged(const FText& NewText);
	void HandleDescriptionCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void HandleRefinementNoteChanged(const FText& NewText);
	void CommitPendingCandidateEdit();
	void RefreshAgentStatus();
	void HandleSceneContextMenuOpenChanged(bool bIsOpen);
	void HandleSceneContextDraftChanged(const FText& NewText);
	void HandleDescriptionFocusDraftChanged(const FText& NewText);
	bool SyncSceneContextEditorToCurrentMap();
	void SubmitSceneContextDraftRequest(uint64 RequestGeneration);
	void SetPrivacyAcknowledged(bool bAccepted);
	void RefreshSelectedPreview();
	void UpdatePreviewImageBrush();
	void WaitForPreviewResource(uint64 ResourceGeneration);
	bool StartLiveEditableCapture();
	bool UpdateLiveEditableCapture();
	bool CommitEditedView(FCandidatePtr& OutCandidate);
	void EndEditView();
	void SetScope(ECheckBoxState NewState, EExplorationScope NewScope);
	ECheckBoxState GetScopeCheckState(EExplorationScope Scope) const;
	UWorld* GetEditorWorld() const;
	int32 GetSelectedActorCount() const;
	TArray<TWeakObjectPtr<AActor>> GetSelectedEditorActors() const;
	bool IsAgentReady() const;
	bool CanExplore() const;
	bool CanAnalyzeLowInformationSelection() const;
	bool CanEditSceneContext() const;
	bool CanDraftSceneContext() const;
	bool CanClearResults() const;
	bool CanModifySelected() const;
	bool CanEditRefinementNote() const;
	bool CanAnalyzeWithRefinementNote() const;
	bool CanRunBulkActions() const;
	bool CanBatchSelectCandidate(const FCandidatePtr& Candidate) const;
	bool CanRunBatchSelectionActions() const;
	bool CanIncludeBatchSelection() const;
	bool CanClearBatchDecision() const;
	bool CanAnalyzeBatchSelection() const;
	bool CanRecaptureBatchSelection() const;
	int32 GetIncludeEligibleCountForPaths(const TSet<FString>& ActorPaths) const;
	int32 GetClearDecisionEligibleCountForPaths(const TSet<FString>& ActorPaths) const;
	int32 GetAnalyzeEligibleCountForPaths(const TSet<FString>& ActorPaths) const;
	TSet<FString> GetRefinementNoteActorPaths(const TSet<FString>& ActorPaths) const;
	int32 GetRecaptureEligibleCountForPaths(const TSet<FString>& ActorPaths) const;
	int32 GetRemovableCandidateCountForPaths(const TSet<FString>& ActorPaths) const;
	int32 GetBatchAnalyzeEligibleCount() const;
	int32 GetBatchRecaptureEligibleCount() const;
	ECheckBoxState GetCandidateBatchSelectionState(const FCandidatePtr& Candidate) const;
	ECheckBoxState GetVisibleBatchSelectionState() const;

	FText GetScopeDetailText() const;
	FText GetCharacterCountText() const;
	FText GetCharacterCatalogFeedbackText() const;
	FText GetSelectedCharacterNameText() const;
	FText GetSelectedCharacterDescriptionText() const;
	FText GetSceneContextButtonText() const;
	FText GetSceneContextDraftText() const;
	FText GetDescriptionFocusDraftText() const;
	bool HasSceneContextForCurrentReview() const;
	bool HasDescriptionFocusForCurrentReview() const;
	bool HasGuidanceForCurrentReview() const;
	FText GetSceneContextCharacterCountText() const;
	FText GetDescriptionFocusCharacterCountText() const;
	FText GetSceneContextDraftFeedbackText() const;
	FText GetSceneContextDraftToolTipText() const;
	FText GetAnalysisQualitySummaryText() const;
	FText GetReviewSummaryText() const;
	FText GetAttentionChipText() const;
	FText GetLowInformationChipText() const;
	FText GetCompletionSummaryCountText() const;
	FText GetCompletionSummaryBodyText() const;
	FText GetAnalyzeLowInformationButtonText() const;
	FText GetReadyAgentText() const;
	FText GetEmptyStateTitleText() const;
	FText GetEmptyStateBodyText() const;
	FText GetAgentStatusText() const;
	FSlateColor GetAgentStatusColor() const;
	FText GetPrivacyDisclosureText() const;
	FText GetExploreButtonText() const;
	FText GetExploreToolTip() const;
	FText GetPipelineStateText() const;
	FText GetProgressMessageText() const;
	FText GetProgressCountText() const;
	FText GetVisibleCandidateCountText() const;
	FText GetBatchSelectionCountText() const;
	FText GetEmptyResultsText() const;
	FText GetSelectedActorTitle() const;
	FText GetSelectedActorSubtext() const;
	FText GetSelectedNameText() const;
	FText GetSelectedDescriptionText() const;
	FText GetSelectedRefinementNoteText() const;
	FText GetRefinementNoteCharacterCountText() const;
	FText GetRefinementNoteFooterText() const;
	FText GetSelectedScoreText() const;
	FText GetSelectedReasonText() const;
	FText GetSelectedErrorText() const;
	FText GetDecisionCountsText() const;
	FText GetApplyButtonText() const;
	FText GetApplyToolTip() const;
	FText GetErrorBannerText() const;
	FText GetApplySummaryText() const;
	FText GetPreviewPlaceholderText() const;
	FText GetEvidenceCaptionText() const;
	FSlateColor GetSelectedErrorColor() const;

	EVisibility GetReviewVisibility() const;
	EVisibility GetEmptyStateVisibility() const;
	EVisibility GetEmptyResultsVisibility() const;
	EVisibility GetSelectedErrorVisibility() const;
	EVisibility GetRefinementSectionVisibility() const;
	EVisibility GetClearRefinementNoteVisibility() const;
	EVisibility GetErrorBannerVisibility() const;
	EVisibility GetApplySummaryVisibility() const;
	EVisibility GetAgentRecoveryVisibility() const;
	EVisibility GetBusyOverlayVisibility() const;
	EVisibility GetCompletionSummaryVisibility() const;
	EVisibility GetAttentionChipVisibility() const;
	EVisibility GetLowInformationChipVisibility() const;
	EVisibility GetAttentionSummarySectionVisibility() const;
	EVisibility GetLowInformationSummarySectionVisibility() const;
	EVisibility GetConsentOverlayVisibility() const;
	EVisibility GetEditViewVisibility() const;
	EVisibility GetStaticViewVisibility() const;
	FString GetCurrentMapPackageName() const;
	FString GetSceneContextForCurrentMap() const;
	void SetSceneContextForCurrentMap(const FString& SceneContext);
	FString GetDescriptionFocusForCurrentMap() const;
	void SetDescriptionFocusForCurrentMap(const FString& DescriptionFocus);

	TSharedPtr<FSceneAutoTaggerController> Controller;
	TStrongObjectPtr<USceneAutoTaggerCharacterCatalog> CharacterCatalog;
	TUniquePtr<FSceneAutoTaggerSetupModel> CharacterSetupModel;
	TSharedPtr<SScrollBox> SetupScrollBox;
	TSharedPtr<SBox> CharacterGridContainer;
	TSharedPtr<SScrollBox> CharacterScrollBox;
	TSharedPtr<SScrollBox> GuidanceScrollBox;
	TSharedPtr<SSearchBox> CandidateSearchBox;
	TSharedPtr<SListView<FCandidatePtr>> CandidateListView;
	TSharedPtr<SListView<FCandidatePtr>> AttentionListView;
	TSharedPtr<SListView<FCandidatePtr>> LowInformationListView;
	TSharedPtr<SButton> CompletionContinueButton;
	TSharedPtr<SBox> ReviewInspectorViewport;
	TSharedPtr<SComboButton> SceneContextComboButton;
	TSharedPtr<SImage> PreviewImageWidget;
	TSharedPtr<SWidget> LivePreviewImageWidget;
	TSharedPtr<STextBlock> PreviewPlaceholderWidget;
	TSharedPtr<FSlateDynamicImageBrush> PreviewBrush;
	TSharedPtr<FDeferredCleanupSlateBrush> LivePreviewBrush;
	TUniquePtr<ConvaiSceneAutoTagger::FLiveObjectCaptureSession> LiveCaptureSession;
	TWeakPtr<FSceneAutoTaggerCandidate> PreviewSourceCandidate;
	TWeakPtr<FSceneAutoTaggerCandidate> EditSourceCandidate;
	FIntPoint PreviewSourceSize = FIntPoint::ZeroValue;
	int32 PreviewSourceByteCount = 0;
	uint32 PreviewSourceRevision = 0;
	TArray<FColor> EditableCapturePixels;
	int32 EditableCaptureResolution = 0;
	int32 EditViewWarmupFrames = 0;
	uint64 EditViewLastObservedFrame = 0;
	double EditViewWarmupStartedAtSeconds = 0.0;
	float EditViewYaw = 0.0f;
	float EditViewPitch = 0.0f;
	float EditViewDistanceScale = 1.0f;
	FVector2D EditViewTargetOffset = FVector2D::ZeroVector;
	uint64 PreviewResourceGeneration = 0;
	TArray<FCandidatePtr> FilteredCandidates;
	TArray<FCandidatePtr> AttentionCandidates;
	TArray<FCandidatePtr> LowInformationCandidates;
	TSet<FString> BatchSelectedActorPaths;
	TSet<FString> LowInformationSelectedActorPaths;
	FCandidatePtr PendingMouseSelectionCandidate;
	/** Set only by a row right-click; null means the context menu opened over list whitespace. */
	FCandidatePtr PendingContextMenuCandidate;
	FCandidatePtr CandidateSelectionAnchor;
	FCandidatePtr SelectedCandidate;
	FCandidatePtr PendingEditedCandidate;
	FString SearchText;
	FString CharacterCatalogError;
	FString DraftRequestCharacterID;
	FString LocalError;
	FString SceneContextDraft;
	FString DescriptionFocusDraft;
	FString SceneContextDraftFeedback;
	FString SceneContextDraftError;
	FString SceneContextEditorMapKey;
	FString SceneContextDraftMapKey;
	TArray64<uint8> SceneContextDraftViewportPng;
	struct FCharacterAvatarCacheEntry
	{
		TSharedPtr<FSlateBrush> Brush;
		TStrongObjectPtr<UTexture2D> Texture;
	};
	mutable TMap<FString, FCharacterAvatarCacheEntry> CharacterAvatarCache;
	mutable TSet<FString> PendingCharacterAvatarDownloads;
	mutable TSet<FString> FailedCharacterAvatarDownloads;
	TWeakObjectPtr<UWorld> SceneContextEditorWorld;
	TWeakObjectPtr<UWorld> SceneContextDraftWorld;
	uint64 SceneContextDraftGeneration = 0;
	FString SessionSceneContext;
	FString SessionDescriptionFocus;
	FString SessionSceneContextMapKey;
	TWeakObjectPtr<UWorld> SessionSceneContextWorld;
	FText AgentStatusDetail;
	FText AgentDisplayName;
	FText AgentAnalysisPolicyText;
	EExplorationScope ExplorationScope = EExplorationScope::CurrentLevel;
	float SignificanceThreshold = 0.52f;
	float BulkConfidenceThreshold = 0.75f;
	bool bIncludeAlreadyTaggedActors = false;
	bool bSetupUsesWideLayout = false;
	bool bPrivacyAcknowledged = false;
	bool bSceneContextDraftDirty = false;
	bool bDescriptionFocusDraftDirty = false;
	bool bSceneContextDraftBusy = false;
	bool bCharacterCatalogLoading = false;
	bool bAgentReady = false;
	bool bSuppressCandidateSelectionChanged = false;
	bool bEditingView = false;
	bool bEditViewReadabilityCalibrated = false;
	bool bPreviewResourceReady = false;
	bool bEditablePreviewResourceReady = false;
	bool bEditableCaptureMatchesCamera = false;
	bool bCompletionSummaryWasVisible = false;
};
