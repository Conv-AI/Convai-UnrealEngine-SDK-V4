// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "ConvaiCloudProjectsSeams.h"
#include "Containers/Ticker.h"
#include "FileHelpers.h"
#include "HAL/PlatformProcess.h"
#include "Packaging/ConvaiProjectPrepare.h"
#include "UI/SConvaiCloudProjects.h"
#include "UObject/Package.h"

/**
 * Drives one upload at a time: prepare -> package -> reserve -> upload -> build -> activate.
 * Owns the API client, the packager, and the view state. Game thread only.
 */
class FConvaiCloudProjectsController : public TSharedFromThis<FConvaiCloudProjectsController>
{
public:
	TSharedRef<SConvaiCloudProjects> CreateWidget();
	void Refresh();
	void Shutdown();

private:
	/** One upload run. Reset when a run finishes, so the next run never inherits half of this one. */
	struct FRun
	{
		FString ProjectId;
		FString Version;
		/** Every version seen on the server: at the run's start and in the read before the reservation. */
		TArray<FConvaiProjectVersion> KnownVersions;
		FString ArchivePath;
		/** The reserved version and its signed URL; a retried upload goes to the same one. */
		FConvaiProjectReservation Reservation;
		/** PUTs of the archive sent so far, the retries included. */
		int32 UploadAttempts = 0;
		FString BuildId;
		/** The run project's own hosting and runtime as the run began; the activation's fallback. */
		FConvaiProjectHostingSettings Settings;
		/** A run started by Create owns the project record it just made. */
		bool bCreated = false;
		/** A build poll is out; the next tick waits for it instead of sending another. */
		bool bPollInFlight = false;
		/** Activation has begun; however many successes the polls report, a run activates once. */
		bool bActivating = false;
	};

	bool EnsureClient();
	/** For a stage that reaches through the client: fails with the sign-in message when it is gone. */
	bool RequireClient();
	/** Cancels and releases the client, and clears what its dropped requests can no longer clear. */
	void RetireClient();
	void SignIn();

	void Select(const FString& ProjectId);
	void Create(const FString& Name);
	void UploadChanges(const FString& ProjectId);
	void OpenInBrowser(const FString& ProjectId);
	void Cancel();
	/** Make an already-built version the one that serves traffic. */
	void ActivateVersion(const FString& BuildId);
	void ShowLogs(const FString& BuildId);
	void PublishPage(const FConvaiProjectPublishState& Page, bool bPublish);
	/** Saves hosting + runtime settings, optionally activating a build in the same request. */
	void SaveSettings(const FConvaiProjectHostingSettings& Settings, const FString& BuildId);
	/** Fills State.Versions and State.Publish for the current selection. */
	void RefreshSelectionDetails();

	/**
	 * The cook reads packages from disk, so unsaved edits are saved first. False, with the error shown, when any of the
	 * project's own is left unsaved; one outside the project is logged and passed over.
	 */
	bool SaveBeforeCook();

	/** Pipeline stages, each one entered only from the stage before it. */
	void BeginRun(const FString& ProjectId, bool bCreated);
	void PackageProject();
	/** Re-reads the project for the next free version number, reserves it, then uploads. */
	void ReserveVersion();
	/** One PUT of the archive. A failed one is sent again after a wait, up to three attempts in all. */
	void UploadArchive();
	/** What the upload retry wait's ticker runs: the next attempt, unless the run that waited has ended. */
	void UploadRetryTick(uint64 Generation);
	void StartBuild();
	void PollBuild();
	/** One poll of the build: what the ticker runs every interval. False stops the ticker. */
	bool PollTick();
	void ActivateBuild();
	void CompleteRun(const FString& Notice);

	void Fail(const FString& Error);
	/** Whether an archive this large can be uploaded from this editor. */
	static bool IsUploadableSize(int64 ArchiveBytes);
	/** True for a continuation sent by the run still in progress. */
	bool IsCurrentRun(uint64 Generation) const { return !bShuttingDown && Generation == RunGeneration; }
	/** Entering a new stage restarts its clock; repeating the current one only updates progress. */
	void SetJob(EConvaiProjectStage Stage, const FString& Title, const FString& Detail,
		TOptional<float> Progress = TOptional<float>());
	/** For progress reports: State takes every one, the widget about four a second. A new stage publishes at once. */
	void SetJobProgress(EConvaiProjectStage Stage, const FString& Title, const FString& Detail, TOptional<float> Progress);
	void ClearJob();
	void Publish();
	void RebuildCards(const TArray<FConvaiProject>& InProjects);
	const FConvaiProject* FindProject(const FString& ProjectId) const;
	void StopPolling();
	void StopUploadRetry();
	/** Deletes the archive a finished or failed run left behind. */
	void DiscardArchive();

	TSharedPtr<SConvaiCloudProjects> Widget;
	TSharedPtr<IConvaiCloudOperations> Client;
	TSharedPtr<IConvaiPackagerStep> Packager;
	/** The collaborators a test replaces before a run; each defaults to the real one. */
	TFunction<TSharedPtr<IConvaiCloudOperations>(FString& /*OutError*/)> MakeClient = &FConvaiCloudOperations::Create;
	TFunction<FConvaiProjectPrepareReport()> PrepareProject = &FConvaiProjectPrepare::Run;
	TFunction<void(const FString& /*Url*/)> LaunchBrowser = [](const FString& Url) { FPlatformProcess::LaunchURL(*Url, nullptr, nullptr); };
	/** Where the upload retry wait registers. A test ticks its own, to let the wait's time pass. */
	FTSTicker* RetryTicker = &FTSTicker::GetCoreTicker();
	/**
	 * Saves these packages. PR_Cancelled when the user cancels the dialog, a Save As or the checkout it opens. A file
	 * left unchecked or that failed to save still comes back as success or failure, so what stays dirty afterwards
	 * is what counts.
	 */
	TFunction<FEditorFileUtils::EPromptReturnCode(const TArray<FString>& /*PackageNames*/)> SavePackages = &FConvaiCloudProjectsController::PromptToSavePackages;
	static FEditorFileUtils::EPromptReturnCode PromptToSavePackages(const TArray<FString>& PackageNames);
	/**
	 * Reads packages back into the asset registry, dependencies included. A save updates only their tags there, and
	 * Prepare finds the Blueprints to fix by what they depend on.
	 */
	TFunction<void(const TArray<FString>& /*PackageNames*/)> RescanPackages = &FConvaiCloudProjectsController::RescanSavedPackages;
	static void RescanSavedPackages(const TArray<FString>& PackageNames);
	/** The dirty packages that save works on, by name. Whatever it left here, the cook would read from the old file. */
	TFunction<TArray<FString>()> GetUnsavedPackages = []
	{
		TArray<UPackage*> Dirty;
		FEditorFileUtils::GetDirtyPackages(Dirty);
		TArray<FString> Names;
		for (const UPackage* Package : Dirty) Names.Add(Package->GetName());
		return Names;
	};
	FConvaiCloudProjectsViewState State;
	/** The project State.Publish describes; a Publish for any other selection waits for that one's page. */
	FString PublishProjectId;
	/** The project the build log belongs to, and the build last asked for: any other response is stale. */
	FString LogProjectId;
	FString LogBuildId;
	/** Hosting + runtime for the current selection, as last read from or written to the server. */
	FConvaiProjectHostingSettings Settings;
	TArray<FConvaiProject> Projects;
	FRun Run;
	/** Advanced whenever a run begins or ends, so a late completion can tell its run is gone. */
	uint64 RunGeneration = 0;
	FTSTicker::FDelegateHandle PollHandle;
	/** The wait before the next upload attempt; removed wherever the run ends. */
	FTSTicker::FDelegateHandle RetryHandle;
	/** Poll attempts spent on the current build, so a stuck job stops asking forever. */
	int32 PollAttempts = 0;
	/** FPlatformTime::Seconds() of SetJobProgress's last publish. */
	double LastProgressPublishAt = 0.0;
	bool bShuttingDown = false;

#if WITH_DEV_AUTOMATION_TESTS
	/** Every Publish, with or without a widget, for the rows that count them. */
	int32 PublishCount = 0;
	friend class FConvaiCloudProjectsControllerHarness;
#endif
};
