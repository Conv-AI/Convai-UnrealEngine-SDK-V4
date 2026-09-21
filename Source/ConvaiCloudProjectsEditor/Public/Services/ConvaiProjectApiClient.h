// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"

class FJsonObject;

/** One uploaded archive and the build job that turned it into a runnable image. */
struct FConvaiProjectVersion
{
	FString AssetVersionId;
	FString Version;
	FString FileName;
	FString UploadStatus;
	/** Build job identity and status. Empty JobId means the archive uploaded but no build was started. */
	FString JobId;
	FString JobStatus;
	int64 SizeBytes = -1;
};

/** A ps-application: the cloud record a packaged project is uploaded into. */
struct FConvaiProject
{
	FString Id;
	FString Name;
	FString UpdatedAt;
	TArray<FConvaiProjectVersion> Versions;
	/** Build ids, not version ids. Active is what streams today; Desired is what activation is moving to. */
	FString ActiveBuildId;
	FString DesiredBuildId;
	FString DeploymentStatus;
	/** Experience page created for this application, when the caller has already made one. */
	FString ExperienceId;
};

/** A reserved version plus the signed URL its archive is PUT to. */
struct FConvaiProjectReservation
{
	FString AssetVersionId;
	FString UploadUrl;
	TMap<FString, FString> UploadHeaders;
};

/** A build's status together with whatever log lines the cloud has recorded so far. */
struct FConvaiBuildReport
{
	FString Status;
	TArray<FString> Logs;
};

/** An experience page linked to a project. Visibility is draft/public/unlisted/private. */
struct FConvaiExperienceLink
{
	FString ExperienceId;
	FString Name;
	FString Description;
	FString Visibility;
};

/**
 * The ps-applications control plane, exactly as the web dashboard drives it:
 * create -> reserve version -> PUT archive -> build -> activate -> experience page.
 * Construct and call on the game thread; every callback is delivered there too.
 */
class FConvaiProjectApiClient : public TSharedFromThis<FConvaiProjectApiClient>
{
public:
	using FProjectsCallback = TFunction<void(TArray<FConvaiProject>, FString)>;
	using FProjectCallback = TFunction<void(FConvaiProject, FString)>;
	using FReservationCallback = TFunction<void(FConvaiProjectReservation, FString)>;
	using FStringCallback = TFunction<void(FString /*Value*/, FString /*Error*/)>;
	using FProgress = TFunction<void(int64 /*Sent*/, int64 /*Total*/)>;

	/** Fails when no API key or auth token is stored; the caller shows a sign-in prompt. */
	static TSharedPtr<FConvaiProjectApiClient> Create(FString& OutError);

	void List(FProjectsCallback Completion);
	void Get(const FString& ProjectId, FProjectCallback Completion);
	void CreateProject(const FString& Name, FProjectCallback Completion);
	void ReserveVersion(const FString& ProjectId, const FString& Version, const FString& ArchivePath, FReservationCallback Completion);
	/** PUTs the archive to the signed URL. Storage, not the API: no Convai auth header is sent. */
	void UploadArchive(const FConvaiProjectReservation& Reservation, const FString& ArchivePath, FProgress Progress, FStringCallback Completion);
	/** Returns the created build (job) id. */
	void StartBuild(const FString& ProjectId, const FString& AssetVersionId, FStringCallback Completion);
	/** Returns the job status string. */
	void GetBuildStatus(const FString& JobId, FStringCallback Completion);
	/** Status plus the cloud build log, for the details panel's log view. */
	void GetBuildReport(const FString& JobId, TFunction<void(FConvaiBuildReport, FString)> Completion);
	/** Experience pages linked to this project, with their current audience. */
	void ListExperiences(const FString& ProjectId, TFunction<void(TArray<FConvaiExperienceLink>, FString)> Completion);
	/** Saves page details; bPublish also makes the page live at the chosen visibility. */
	void PublishExperience(const FConvaiExperienceLink& Page, bool bPublish, FStringCallback Completion);
	/** Requests activation of a built version. Confirm by re-reading the project. */
	void Activate(const FString& ProjectId, const FString& BuildId, FStringCallback Completion);
	void SaveHostingDefaults(const FString& ProjectId, FStringCallback Completion);
	/** Creates (or returns) the experience page that makes the build reachable in a browser. */
	void CreateExperience(const FString& ProjectId, FStringCallback Completion);
	/** Opens an edit session against an experience and returns its session id. */
	void StartStreamSession(const FString& ExperienceId, FStringCallback Completion);

	/** Retires every in-flight response. Safe to call from Shutdown. */
	void CancelAll();

	/** Player URL for a session, e.g. https://x.convai.com/stream-v2/<id>/. */
	static FString BuildStreamUrl(const FString& SessionId);
	/** "1.0.0" when the project has no numeric versions yet, otherwise the next patch. */
	static FString NextVersion(const TArray<FConvaiProjectVersion>& Versions);
	static bool IsBuildSucceeded(const FString& Status);
	static bool IsBuildFailed(const FString& Status);
	static bool IsBuildInProgress(const FString& Status);

private:
	FConvaiProjectApiClient(FString InAuthHeader, FString InApiKey, FString InBaseUrl);

	void Post(const FString& Endpoint, const TSharedRef<FJsonObject>& Payload,
		TFunction<void(TSharedPtr<FJsonObject>, FString)> Completion);

	static FConvaiProject ParseProject(const TSharedPtr<FJsonObject>& Object);

	FString AuthHeader;
	FString ApiKey;
	FString BaseUrl;
	/** Bumped by CancelAll so late responses from a retired context are dropped. */
	uint64 Epoch = 0;
	TArray<TWeakPtr<IHttpRequest, ESPMode::ThreadSafe>> Pending;
};
