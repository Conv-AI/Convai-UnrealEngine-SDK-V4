// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Packaging/ConvaiProjectPackager.h"
#include "Services/ConvaiProjectApiClient.h"

/**
 * Every cloud call the controller makes. The production adapter owns one API client, so
 * releasing the adapter releases the client and drops its in-flight completions.
 */
class IConvaiCloudOperations
{
public:
	using FClient = FConvaiProjectApiClient;

	virtual ~IConvaiCloudOperations() = default;

	virtual void List(FClient::FProjectsCallback Completion) = 0;
	virtual void Get(const FString& ProjectId, FClient::FProjectCallback Completion) = 0;
	virtual void CreateProject(const FString& Name, FClient::FProjectCallback Completion) = 0;
	virtual void ReserveVersion(const FString& ProjectId, const FString& Version, const FString& ArchivePath,
		FClient::FReservationCallback Completion) = 0;
	virtual void UploadArchive(const FConvaiProjectReservation& Reservation, const FString& ArchivePath,
		FClient::FProgress Progress, FClient::FStringCallback Completion) = 0;
	virtual void StartBuild(const FString& ProjectId, const FString& AssetVersionId, FClient::FStringCallback Completion) = 0;
	virtual void GetBuildStatus(const FString& JobId, FClient::FStringCallback Completion) = 0;
	virtual void GetBuildReport(const FString& JobId, TFunction<void(FConvaiBuildReport, FString)> Completion) = 0;
	virtual void ListExperiences(const FString& ProjectId,
		TFunction<void(TArray<FConvaiExperienceLink>, FString)> Completion) = 0;
	virtual void PublishExperience(const FConvaiExperienceLink& Page, bool bPublish, FClient::FStringCallback Completion) = 0;
	virtual void SaveSettings(const FString& ProjectId, const FConvaiProjectHostingSettings& Settings,
		const FString& BuildId, FClient::FStringCallback Completion) = 0;
	virtual void CreateExperience(const FString& ProjectId, FClient::FStringCallback Completion) = 0;
	virtual void StartStreamSession(const FString& ExperienceId, FClient::FStringCallback Completion) = 0;
	virtual void CancelAll() = 0;
	virtual FString BuildStreamUrl(const FString& SessionId) const = 0;
};

class FConvaiCloudOperations final : public IConvaiCloudOperations
{
public:
	/** Null, with the sign-in message, when no API key or auth token is stored. */
	static TSharedPtr<IConvaiCloudOperations> Create(FString& OutError)
	{
		const TSharedPtr<FConvaiProjectApiClient> Client = FConvaiProjectApiClient::Create(OutError);
		if (!Client) return nullptr;
		return MakeShared<FConvaiCloudOperations>(Client.ToSharedRef());
	}

	explicit FConvaiCloudOperations(TSharedRef<FConvaiProjectApiClient> InClient) : Client(MoveTemp(InClient)) {}

	virtual void List(FClient::FProjectsCallback Completion) override { Client->List(MoveTemp(Completion)); }
	virtual void Get(const FString& ProjectId, FClient::FProjectCallback Completion) override
	{
		Client->Get(ProjectId, MoveTemp(Completion));
	}
	virtual void CreateProject(const FString& Name, FClient::FProjectCallback Completion) override
	{
		Client->CreateProject(Name, MoveTemp(Completion));
	}
	virtual void ReserveVersion(const FString& ProjectId, const FString& Version, const FString& ArchivePath,
		FClient::FReservationCallback Completion) override
	{
		Client->ReserveVersion(ProjectId, Version, ArchivePath, MoveTemp(Completion));
	}
	virtual void UploadArchive(const FConvaiProjectReservation& Reservation, const FString& ArchivePath,
		FClient::FProgress Progress, FClient::FStringCallback Completion) override
	{
		Client->UploadArchive(Reservation, ArchivePath, MoveTemp(Progress), MoveTemp(Completion));
	}
	virtual void StartBuild(const FString& ProjectId, const FString& AssetVersionId, FClient::FStringCallback Completion) override
	{
		Client->StartBuild(ProjectId, AssetVersionId, MoveTemp(Completion));
	}
	virtual void GetBuildStatus(const FString& JobId, FClient::FStringCallback Completion) override
	{
		Client->GetBuildStatus(JobId, MoveTemp(Completion));
	}
	virtual void GetBuildReport(const FString& JobId, TFunction<void(FConvaiBuildReport, FString)> Completion) override
	{
		Client->GetBuildReport(JobId, MoveTemp(Completion));
	}
	virtual void ListExperiences(const FString& ProjectId,
		TFunction<void(TArray<FConvaiExperienceLink>, FString)> Completion) override
	{
		Client->ListExperiences(ProjectId, MoveTemp(Completion));
	}
	virtual void PublishExperience(const FConvaiExperienceLink& Page, bool bPublish, FClient::FStringCallback Completion) override
	{
		Client->PublishExperience(Page, bPublish, MoveTemp(Completion));
	}
	virtual void SaveSettings(const FString& ProjectId, const FConvaiProjectHostingSettings& Settings,
		const FString& BuildId, FClient::FStringCallback Completion) override
	{
		Client->SaveSettings(ProjectId, Settings, BuildId, MoveTemp(Completion));
	}
	virtual void CreateExperience(const FString& ProjectId, FClient::FStringCallback Completion) override
	{
		Client->CreateExperience(ProjectId, MoveTemp(Completion));
	}
	virtual void StartStreamSession(const FString& ExperienceId, FClient::FStringCallback Completion) override
	{
		Client->StartStreamSession(ExperienceId, MoveTemp(Completion));
	}
	virtual void CancelAll() override { Client->CancelAll(); }
	virtual FString BuildStreamUrl(const FString& SessionId) const override { return Client->BuildStreamUrl(SessionId); }

private:
	const TSharedRef<FConvaiProjectApiClient> Client;
};

/** Package and zip: Start reports one completion, Cancel asks the run to stop. */
class IConvaiPackagerStep
{
public:
	virtual ~IConvaiPackagerStep() = default;

	virtual void Start(FConvaiProjectPackager::FCompletion Completion, FConvaiProjectPackager::FProgress Progress) = 0;
	virtual void Cancel() = 0;
};

class FConvaiPackagerStep final : public IConvaiPackagerStep
{
public:
	virtual void Start(FConvaiProjectPackager::FCompletion Completion, FConvaiProjectPackager::FProgress Progress) override
	{
		Packager->Start(MoveTemp(Completion), MoveTemp(Progress));
	}
	virtual void Cancel() override { Packager->Cancel(); }

private:
	const TSharedRef<FConvaiProjectPackager> Packager = MakeShared<FConvaiProjectPackager>();
};
