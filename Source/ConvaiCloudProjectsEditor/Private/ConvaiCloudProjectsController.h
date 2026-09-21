// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Packaging/ConvaiProjectPackager.h"
#include "Services/ConvaiProjectApiClient.h"
#include "UI/SConvaiCloudProjects.h"

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
	/** One upload run. Reset when a run finishes, so a retry never inherits half a previous one. */
	struct FRun
	{
		FString ProjectId;
		FString Version;
		FString ArchivePath;
		FString AssetVersionId;
		FString BuildId;
		/** A run started by Create owns the project record it just made. */
		bool bCreated = false;
	};

	bool EnsureClient();
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
	/** Fills State.Versions and State.Publish for the current selection. */
	void RefreshSelectionDetails();

	/** Pipeline stages, each one entered only from the stage before it. */
	void BeginRun(const FString& ProjectId, bool bCreated);
	void PackageProject();
	/** Reserves the version and, on its completion, uploads the archive to the signed URL. */
	void ReserveVersion();
	void StartBuild();
	void PollBuild();
	void ActivateBuild();
	void CompleteRun(const FString& Notice);

	void Fail(const FString& Error);
	/** Entering a new stage restarts its clock; repeating the current one only updates progress. */
	void SetJob(EConvaiProjectStage Stage, const FString& Title, const FString& Detail,
		TOptional<float> Progress = TOptional<float>());
	void ClearJob();
	void Publish();
	void RebuildCards(const TArray<FConvaiProject>& InProjects);
	const FConvaiProject* FindProject(const FString& ProjectId) const;
	void StopPolling();
	/** Deletes the archive a finished or failed run left behind. */
	void DiscardArchive();

	TSharedPtr<SConvaiCloudProjects> Widget;
	TSharedPtr<FConvaiProjectApiClient> Client;
	TSharedPtr<FConvaiProjectPackager> Packager;
	FConvaiCloudProjectsViewState State;
	TArray<FConvaiProject> Projects;
	FRun Run;
	FTSTicker::FDelegateHandle PollHandle;
	/** Poll attempts spent on the current build, so a stuck job stops asking forever. */
	int32 PollAttempts = 0;
	bool bShuttingDown = false;
};
