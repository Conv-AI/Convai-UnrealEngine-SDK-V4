// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Diorama/ConvaiAvatarDiorama.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STableViewBase.h"
#include "Styling/SlateBrush.h"

class SBox;
class SVerticalBox;
class SScrollBox;
template<typename ItemType> class STileView;
struct FAssetData;
class ITableRow;
class STableViewBase;
class FConvaiAvatarThumbnailCache;

/** Presentation data only. The controller owns all account and filesystem operations. */
struct FConvaiAvatarStudioCard
{
	FString AssetId;
	FString Name;
	FString Gender;
	FString Status;
	FString EngineVersion;
	FString ThumbnailUrl;
	FString SquareThumbnailUrl;
	FString ThumbnailLocalPath;
	FString BlueprintPath;
	/** Saved preparation choice for this local avatar; legacy records default off. */
	bool bIncludeConvaiContent = false;
	/** Readiness for Windows streaming. Empty means no warning; unknown is informational. */
	FString HealthWarning;
	bool bHealthUnknown = false;
	/** Sizes of completed cloud files only. Never populate from a pending local upload. */
	TOptional<int64> SourceBytes;
	TOptional<int64> WindowsBytes;
	TOptional<int64> LinuxBytes;
	FString SourceVersion;
	FString WindowsVersion;
	FString LinuxVersion;
	TSharedPtr<FSlateBrush> ThumbnailBrush;
	bool bHasSource = false;
	/** A lightweight library record can declare source without checking its stored file. */
	bool bSourceStatusKnown = true;
	FString SourceStatus;
	bool bDetailsLoading = false;
	FString DetailsError;
	bool bIsLocal = false;
	/** Local pre-upload records only; cloud-bound records use normal cloud actions. */
	bool bIsLocalDraft = false;
	bool bIsIncompleteLocalDraft = false;
	bool bCanResumeDraft = false;
	bool bCanDiscardDraft = false;
	bool bCanDownload = false;
	bool bCanUpload = false;
	bool bCanDelete = false;
	bool bCanEditMetadata = false;
	bool bCanBrowseContent = false;
	bool bCanCapturePortrait = false;
	FString CaptureDisabledReason;
	FString MetadataDisabledReason;
	FString BrowseDisabledReason;
	FString DownloadDisabledReason;
	FString UploadDisabledReason;
	FString DeleteDisabledReason;
};

struct FConvaiAvatarStudioUploadOptions
{
	bool bIncludeSource = true;
	bool bIncludeWindows = true;
	bool bIncludeLinux = false;
	bool bIncludeConvaiContent = false;
};

struct FConvaiAvatarStudioMetadataRequest
{
	FString AssetId;
	FString DisplayName;
	FString Gender;
	/** Empty keeps the existing cloud thumbnail. */
	FString ThumbnailPath;
};

struct FConvaiAvatarStudioCreateRequest
{
	/** Stable across retries of one Blueprint draft; never regenerated on Submit. */
	FString DraftId;
	FString BlueprintPath;
	FString DisplayName;
	/** API gender choice: male or female; empty until explicitly selected. */
	FString Gender;
	FString ThumbnailPath;
	/** Explicit creation choice; retained across retries and recorded with the prepared avatar. */
	bool bIsMetaHuman = true;
	bool bIncludeDiorama = false;
	FString LevelPath;
	bool bIncludeSource = true;
	bool bIncludeWindows = true;
	bool bIncludeLinux = false;
	bool bIncludeConvaiContent = false;
};

struct FConvaiAvatarStudioViewState
{
	TArray<FConvaiAvatarStudioCard> Avatars;
	FString SelectedAssetId;
	bool bLoading = false;
	bool bBusy = false;
	bool bCanCreate = true;
	bool bCanCancel = false;
	bool bNeedsSignIn = false;
	bool bCanApplyConfiguration = true;
	bool bCanRestoreConfiguration = false;
	bool bCanCapturePortrait = false;
	FString CaptureDisabledReason;
	bool bSourceAvailable = true;
	bool bWindowsAvailable = true;
	bool bLinuxAvailable = false;
	bool bDefaultIncludeSource = true;
	bool bDefaultIncludeWindows = true;
	bool bDefaultIncludeLinux = false;
	bool bUploadDefaultsLoading = false;
	FString UploadDefaultsStatus;
	FString DioramaStatus;
	int32 DioramaErrorCount = 0;
	int32 DioramaWarningCount = 0;
	TArray<FConvaiAvatarDioramaIssue> DioramaIssues;
	bool bDioramaLimitsRead = false;
	FString SourceDisabledReason;
	FString WindowsDisabledReason;
	FString LinuxDisabledReason;
	bool bCanOpenLog = false;
	FString DiagnosticReference;
	FString ConfigurationStatus;
	FString EngineSummary;
	FString EngineWarning;
	FString RemoteConfigurationStatus;
	FString DependencySummary;
	bool bRemoteConfigurationLoading = false;
	bool bEngineMismatch = false;
	bool bEngineMismatchAcknowledged = false;
	FString CreateDisabledReason;
	FString Error;
	FString Notice;
	FString JobTitle;
	FString JobDetail;
	/** Unset while progress is indeterminate. Never invent percentages. */
	TOptional<float> JobProgress;
	/** Current transfer stage only. Unset counts stay undisclosed rather than guessed. */
	TOptional<int64> JobBytesCompleted;
	TOptional<int64> JobBytesTotal;
};

DECLARE_DELEGATE_OneParam(FOnConvaiAvatarStudioAssetAction, const FString&);
DECLARE_DELEGATE_TwoParams(FOnConvaiAvatarStudioUpload, const FString&, const FConvaiAvatarStudioUploadOptions&);
DECLARE_DELEGATE_OneParam(FOnConvaiAvatarStudioCreate, const FConvaiAvatarStudioCreateRequest&);
DECLARE_DELEGATE_OneParam(FOnConvaiAvatarStudioMetadata, const FConvaiAvatarStudioMetadataRequest&);

/** Searchable library with selection details and a single create form. */
class CONVAIAVATARSTUDIOEDITOR_API SConvaiAvatarStudio : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SConvaiAvatarStudio) {}
		SLATE_EVENT(FSimpleDelegate, OnRefresh)
		SLATE_EVENT(FOnConvaiAvatarStudioAssetAction, OnSelect)
		SLATE_EVENT(FOnConvaiAvatarStudioAssetAction, OnDownload)
		SLATE_EVENT(FOnConvaiAvatarStudioUpload, OnUploadChanges)
		SLATE_EVENT(FOnConvaiAvatarStudioAssetAction, OnDelete)
		SLATE_EVENT(FOnConvaiAvatarStudioAssetAction, OnResumeLocalDraft)
		SLATE_EVENT(FOnConvaiAvatarStudioAssetAction, OnDiscardLocalDraft)
		SLATE_EVENT(FOnConvaiAvatarStudioCreate, OnCreate)
		SLATE_EVENT(FOnConvaiAvatarStudioCreate, OnScanDiorama)
		SLATE_EVENT(FOnConvaiAvatarStudioCreate, OnCaptureCreate)
		SLATE_EVENT(FOnConvaiAvatarStudioMetadata, OnSaveMetadata)
		SLATE_EVENT(FOnConvaiAvatarStudioMetadata, OnCaptureMetadata)
		SLATE_EVENT(FOnConvaiAvatarStudioAssetAction, OnBrowseContent)
		SLATE_EVENT(FOnConvaiAvatarStudioAssetAction, OnOpenLog)
		SLATE_EVENT(FSimpleDelegate, OnOpenDocumentation)
		SLATE_EVENT(FSimpleDelegate, OnCancelJob)
		SLATE_EVENT(FSimpleDelegate, OnSignIn)
		SLATE_EVENT(FSimpleDelegate, OnApplyConfiguration)
		SLATE_EVENT(FSimpleDelegate, OnRestoreConfiguration)
		SLATE_EVENT(FSimpleDelegate, OnContinueEngineMismatch)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SConvaiAvatarStudio() override;
	virtual void Tick(const FGeometry& AllottedGeometry, double CurrentTime, float DeltaTime) override;
	/** Call on the game thread. Active drafts are retained across refreshes/errors. */
	void SetState(const FConvaiAvatarStudioViewState& InState);
	void SetSelectedAvatar(const FString& AssetId);
	/** A successful create clears its draft. Pass false when navigating away temporarily. */
	void ShowLibrary(bool bDiscardDraft = true);
	/** Resume a local pre-upload draft. Caller supplies identity/source; unset fields require selection. */
	void ShowDraft(const FConvaiAvatarStudioCreateRequest& Request);
	/** Re-read the active create draft after a choice, explicit rescan, or policy refresh. */
	void RescanDiorama();
	/** Call only after metadata save is confirmed. Errors retain the matching edit draft. */
	void FinishMetadataEdit(const FString& AssetId);
	/** Capture updates a matching local draft only. It does not submit or save metadata. */
	bool SetCapturedCreateThumbnail(const FConvaiAvatarStudioCreateRequest& Requested, const FString& File);
	bool SetCapturedMetadataThumbnail(const FConvaiAvatarStudioMetadataRequest& Requested, const FString& File);
#if WITH_DEV_AUTOMATION_TESTS
	/** Real-tab journey observation/input. Does not replace state, delegates, or controller behavior. */
	const FConvaiAvatarStudioViewState& GetAutomationState() const { return State; }
	const FConvaiAvatarStudioCreateRequest& GetAutomationDraft() const { return Draft; }
	const FConvaiAvatarStudioMetadataRequest& GetAutomationMetadataDraft() const { return MetadataDraft; }
	bool AutomationChooseBlueprint(const FAssetData& Asset);
	bool AutomationChooseLevel(const FAssetData& Asset);
	bool AutomationChooseThumbnail(const FString& File);
	bool AutomationChooseMetadataThumbnail(const FString& File);
	bool AutomationSelectAvatar(const FString& AssetId);
	/** Observation only; local previews do not count as a resolved cloud image. */
	bool AutomationHasRemoteThumbnail(const FString& AssetId) const;
	/** MD5 of received compressed HTTPS image bytes, only after successful decoding. */
	bool AutomationRemoteThumbnailMatches(const FString& AssetId, const FString& ExpectedHash) const;
#endif

private:
	TSharedRef<SWidget> BuildLibrary();
	TSharedRef<SWidget> BuildDetails();
	TSharedRef<SWidget> BuildCreate();
	TSharedRef<SWidget> BuildCreateFields();
	TSharedRef<SWidget> BuildThumbnailEditor(bool bForCreate);
	TSharedRef<SWidget> BuildThumbnailPreview(bool bForCreate);
	TSharedRef<SWidget> BuildThumbnailActions(bool bForCreate, bool bCompact = false);
	FString GetPortraitFile(bool bForCreate) const;
	FReply OpenPortraitPreview(bool bForCreate);
	TSharedRef<SWidget> BuildSourceChoice(bool bForCreate);
	TSharedRef<SWidget> BuildMetadata();
	TSharedRef<SWidget> BuildMetadataFooter();
	TSharedRef<SWidget> BuildProjectSettings();
	TSharedRef<SWidget> BuildLogActions();
	FReply ShowAbout();
	FReply BeginMetadataEdit();
	FReply SaveMetadata();
	FReply BrowseMetadataThumbnail();
	FReply CaptureCreatePortrait();
	FReply CaptureMetadataPortrait();
	void MetadataThumbnailChosen(const FString& File);
	FReply BrowseContent();
	FConvaiAvatarStudioUploadOptions UploadOptions(bool bForCreate) const;
	void ResetUploadOptions(bool bForCreate);
	void SyncConvaiContentChoice();
	FText UploadSummary(bool bForCreate) const;
	FText UploadValidation(bool bForCreate) const;
	FText MetadataValidation() const;
	FText ValidateThumbnail(const FString& Path, bool bRequired) const;
	TSharedRef<ITableRow> GenerateCard(TSharedPtr<FConvaiAvatarStudioCard> Item, const TSharedRef<STableViewBase>& Owner);
	void RebuildDetails();
	void FilterCards();
	void SelectionChanged(TSharedPtr<FConvaiAvatarStudioCard> Item, ESelectInfo::Type SelectInfo);
	const FConvaiAvatarStudioCard* SelectedCard() const;
	FReply BeginCreate();
	FReply SubmitCreate();
	FReply BrowseThumbnail();
	void BlueprintChosen(const FAssetData& Asset);
	void LevelChosen(const FAssetData& Asset);
	void RefreshDioramaIssues();
	void ThumbnailChosen(const FString& File);
	FReply DeleteSelected();
	FReply UploadSelected();
	FReply DownloadSelected();
	FReply ResumeSelectedDraft();
	FReply DiscardSelectedDraft();
	bool CanSubmitCreate() const;
	FText GetCreateValidation() const;
	FText GetThumbnailValidation() const;
	FText GetEmptyStateText() const;
	EVisibility GetEmptyStateVisibility() const;
	FText GetActionReason(bool bDownload) const;
	const FSlateBrush* GetThumbnail(const FConvaiAvatarStudioCard& Card) const;
	const FSlateBrush* GetCardThumbnail(const FConvaiAvatarStudioCard& Card) const;
	bool IsCardThumbnailLoading(const FConvaiAvatarStudioCard& Card) const;

	FConvaiAvatarStudioViewState State;
	float LibraryItemWidth = 180.f;
	FConvaiAvatarStudioCreateRequest Draft;
	FConvaiAvatarStudioMetadataRequest MetadataDraft;
	FConvaiAvatarStudioUploadOptions UpdateOptions;
	FString UpdateContentChoiceAssetId;
	bool bUpdateContentChoiceEdited = false;
	mutable FString ValidatedThumbnailPath;
	mutable int64 ValidatedThumbnailSize = -1;
	mutable FDateTime ValidatedThumbnailTime;
	mutable FText ThumbnailValidation;
	TArray<TSharedPtr<FConvaiAvatarStudioCard>> FilteredCards;
	TSharedPtr<STileView<TSharedPtr<FConvaiAvatarStudioCard>>> TileView;
	TSharedPtr<SBox> DetailsHost;
	TSharedPtr<SVerticalBox> DioramaIssuesBox;
	TSharedPtr<SScrollBox> DioramaIssuesScroll;
	TSharedPtr<FConvaiAvatarThumbnailCache> ThumbnailCache;
	TMap<FString, FString> PendingThumbnailPreviews;
	/** Value becomes true after the post-save library refresh starts. */
	TMap<FString, bool> PendingThumbnailRefresh;
	FString SearchText;
	/** Only captures assigned by this view are cleared when its Blueprint changes. */
	FString CapturedDraftThumbnail;
	FString MetadataCaptureAssetId;
	FString MetadataCaptureBlueprint;
	bool bCreating = false;
	bool bCreateNarrowLayout = false;
	TWeakPtr<class SWindow> PortraitWindow;
	bool bEditingMetadata = false;
	bool bUploadOptionsInitialized = false;
	bool bUpdatingSelection = false;
	FSimpleDelegate OnRefresh;
	FOnConvaiAvatarStudioAssetAction OnSelect;
	FOnConvaiAvatarStudioAssetAction OnDownload;
	FOnConvaiAvatarStudioUpload OnUploadChanges;
	FOnConvaiAvatarStudioAssetAction OnDelete;
	FOnConvaiAvatarStudioAssetAction OnResumeLocalDraft;
	FOnConvaiAvatarStudioAssetAction OnDiscardLocalDraft;
	FOnConvaiAvatarStudioCreate OnCreate;
	FOnConvaiAvatarStudioCreate OnScanDiorama;
	FOnConvaiAvatarStudioCreate OnCaptureCreate;
	FOnConvaiAvatarStudioMetadata OnSaveMetadata;
	FOnConvaiAvatarStudioMetadata OnCaptureMetadata;
	FOnConvaiAvatarStudioAssetAction OnBrowseContent;
	FOnConvaiAvatarStudioAssetAction OnOpenLog;
	FSimpleDelegate OnOpenDocumentation;
	FSimpleDelegate OnCancelJob;
	FSimpleDelegate OnSignIn;
	FSimpleDelegate OnApplyConfiguration;
	FSimpleDelegate OnRestoreConfiguration;
	FSimpleDelegate OnContinueEngineMismatch;
};
