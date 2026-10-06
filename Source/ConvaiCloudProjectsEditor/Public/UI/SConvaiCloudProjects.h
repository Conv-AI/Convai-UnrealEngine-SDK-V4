// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Services/ConvaiProjectHostingSettings.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STableViewBase.h"

class ITableRow;
class SBox;
class SEditableTextBox;
template<typename ItemType> class SListView;

/** Presentation only. The controller owns every account, packaging, and filesystem operation. */
struct FConvaiCloudProjectCard
{
	FString Id;
	FString Name;
	/** Latest version label, e.g. "1.0.3". Empty before a first upload. */
	FString Version;
	/** One short line: Live, Activating, Building, Build failed, Uploaded. */
	FString Status;
	FString UpdatedAt;
	bool bLive = false;
	bool bWorking = false;
	bool bFailed = false;
};

/** The upload pipeline, in order. Drives the progress steps the user watches. */
enum class EConvaiProjectStage : uint8
{
	None,
	Prepare,
	Package,
	Compress,
	Upload,
	Build,
	Activate
};

/** One uploaded version of the selected project, as the Versions list shows it. */
struct FConvaiProjectVersionRow
{
	FString Version;
	FString BuildId;
	/** Built / Building / Build failed / Uploaded. */
	FString Status;
	FString Size;
	bool bActive = false;
	/** Rollout state for this version: Active, Preparing, Waiting to start, Activation failed. */
	FString Availability;
	/** Whether the activate button is shown at all, and what it says. */
	bool bCanActivate = false;
	bool bActivateEnabled = true;
	FString ActivateLabel;
	bool bHasLogs = false;
};

/** The experience page that makes a project reachable, and its audience. */
struct FConvaiProjectPublishState
{
	FString ExperienceId;
	FString Name;
	FString Description;
	/** draft | public | unlisted | private. */
	FString Visibility = TEXT("draft");
	bool bLoading = false;
};

struct FConvaiCloudProjectsViewState
{
	TArray<FConvaiCloudProjectCard> Projects;
	FString SelectedId;
	bool bLoading = false;
	/** A packaging or upload run is in flight; every mutating action is disabled. */
	bool bBusy = false;
	bool bNeedsSignIn = false;
	bool bCanCancel = false;
	/** This editor's project name, used to seed the create form and to compare against a selection. */
	FString LocalProjectName;
	/** Set when the selected cloud project was uploaded from a differently named project. */
	FString NameMismatchWarning;
	/** Why packaging is impossible right now, e.g. a missing Linux toolchain. Empty when fine. */
	FString PackagingDisabledReason;
	/** What the last prepare pass changed or could not do. */
	TArray<FString> PrepareNotes;
	FString JobTitle;
	FString JobDetail;
	/** Unset while progress is indeterminate. Never invent a percentage. */
	TOptional<float> JobProgress;
	EConvaiProjectStage Stage = EConvaiProjectStage::None;
	/** FPlatformTime::Seconds() when the run and the current stage began; 0 when idle. */
	double JobStartedAt = 0.0;
	double StageStartedAt = 0.0;
	FString Error;
	FString Notice;

	/** Versions of the selected project, newest first. Empty until one is selected. */
	TArray<FConvaiProjectVersionRow> Versions;
	FConvaiProjectPublishState Publish;
	/** Hosting and runtime settings for the selection, seeded into the Settings form. */
	FConvaiProjectHostingSettings Settings;
	/** Build log for the version whose Logs button was pressed. */
	FString LogVersion;
	TArray<FString> LogLines;
	bool bLogsLoading = false;
};

DECLARE_DELEGATE_OneParam(FOnConvaiCloudProjectAction, const FString& /*ProjectId*/);
DECLARE_DELEGATE_OneParam(FOnConvaiCloudProjectCreate, const FString& /*Name*/);
DECLARE_DELEGATE_OneParam(FOnConvaiCloudProjectBuild, const FString& /*BuildId*/);
/** Page details plus whether this is a draft save or a publish. */
DECLARE_DELEGATE_TwoParams(FOnConvaiCloudProjectPublish, const FConvaiProjectPublishState&, bool /*bPublish*/);
/** Hosting + runtime settings, and a build id when the save should also activate one. */
DECLARE_DELEGATE_TwoParams(FOnConvaiCloudProjectSettings, const FConvaiProjectHostingSettings&, const FString& /*BuildId*/);

/** Library of uploaded projects, with one create form and one update action. */
class SConvaiCloudProjects : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SConvaiCloudProjects) {}
		SLATE_EVENT(FSimpleDelegate, OnRefresh)
		SLATE_EVENT(FSimpleDelegate, OnSignIn)
		SLATE_EVENT(FSimpleDelegate, OnCancelJob)
		SLATE_EVENT(FOnConvaiCloudProjectAction, OnSelect)
		SLATE_EVENT(FOnConvaiCloudProjectCreate, OnCreate)
		SLATE_EVENT(FOnConvaiCloudProjectAction, OnUploadChanges)
		SLATE_EVENT(FOnConvaiCloudProjectAction, OnOpenInBrowser)
		SLATE_EVENT(FOnConvaiCloudProjectBuild, OnActivateVersion)
		SLATE_EVENT(FOnConvaiCloudProjectBuild, OnShowLogs)
		SLATE_EVENT(FOnConvaiCloudProjectPublish, OnPublish)
		SLATE_EVENT(FOnConvaiCloudProjectSettings, OnSaveSettings)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	/** Repaints while a job runs so the elapsed clocks advance. */
	virtual void Tick(const FGeometry& AllottedGeometry, double CurrentTime, float DeltaTime) override;

	/** Call on the game thread. An in-progress create draft survives a refresh. */
	void SetState(const FConvaiCloudProjectsViewState& InState);

private:
	TSharedRef<SWidget> BuildLibrary();
	TSharedRef<SWidget> BuildDetails();
	TSharedRef<SWidget> BuildCreate();
	TSharedRef<SWidget> BuildJobBanner();
	TSharedRef<SWidget> BuildMessages();
	/** Sections of the right-hand workspace, in the order they appear. */
	TSharedRef<SWidget> BuildActions();
	TSharedRef<SWidget> BuildVersions();
	TSharedRef<SWidget> BuildLogs();
	TSharedRef<SWidget> BuildPublish();
	/** Hosting capacity plus the runtime_config the streamed process is launched with. */
	TSharedRef<SWidget> BuildSettings();
	/** A labelled text field with its validation message underneath. */
	TSharedRef<SWidget> SettingsField(const FText& Label, const FString& ErrorKey,
		TFunction<FString()> Get, TFunction<void(const FString&)> Set, const FText& Hint = FText::GetEmpty());
	/**
	 * A three-option select for one ps-frontend-config boolean, matching the dashboard:
	 * Use runtime default (key omitted) / On / Off.
	 */
	TSharedRef<SWidget> FrontendChoice(const FString& Key, const FText& Label);
	/** A small caps label that opens a section. Sections are separated by space, not boxes. */
	TSharedRef<SWidget> SectionHeading(const FText& Title);

	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FConvaiCloudProjectCard> Item, const TSharedRef<STableViewBase>& Owner);
	void SelectionChanged(TSharedPtr<FConvaiCloudProjectCard> Item, ESelectInfo::Type SelectInfo);
	const FConvaiCloudProjectCard* SelectedCard() const;
	void RebuildRows();

	FReply BeginCreate();
	FReply SubmitCreate();
	FReply CancelCreate();
	FReply UploadSelected();
	FReply OpenSelected();

	bool CanMutate() const;
	/** Build id currently serving traffic, or empty. Redeploy re-sends this to trigger a rollout. */
	FString ActiveBuildId() const;
	FText CreateValidation() const;

	FConvaiCloudProjectsViewState State;
	TArray<TSharedPtr<FConvaiCloudProjectCard>> Rows;
	TSharedPtr<SListView<TSharedPtr<FConvaiCloudProjectCard>>> ListView;
	TSharedPtr<SBox> DetailsHost;
	TSharedPtr<SEditableTextBox> NameBox;
	FString SearchText;
	FString DraftName;
	bool bCreating = false;
	bool bUpdatingSelection = false;

	FSimpleDelegate OnRefresh;
	FSimpleDelegate OnSignIn;
	FSimpleDelegate OnCancelJob;
	FOnConvaiCloudProjectAction OnSelect;
	FOnConvaiCloudProjectCreate OnCreate;
	FOnConvaiCloudProjectAction OnUploadChanges;
	FOnConvaiCloudProjectAction OnOpenInBrowser;
	FOnConvaiCloudProjectBuild OnActivateVersion;
	FOnConvaiCloudProjectBuild OnShowLogs;
	FOnConvaiCloudProjectPublish OnPublish;
	FOnConvaiCloudProjectSettings OnSaveSettings;

	/** Publish form edits, kept separate from State so a refresh cannot discard them mid-edit. */
	FConvaiProjectPublishState PublishDraft;
	FString PublishDraftFor;

	/** Settings form edits, seeded once per project so a poll cannot discard them mid-edit. */
	FConvaiProjectHostingSettings SettingsDraft;
	FString SettingsDraftFor;
	TMap<FString, FString> SettingsErrors;
	bool bShowAdvancedJson = false;
	/** Option strings backing the frontend selects; SComboBox needs stable shared pointers. */
	TArray<TSharedPtr<FString>> FrontendOptions;
};
