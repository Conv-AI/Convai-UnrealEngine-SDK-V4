// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "Services/ConvaiProjectApiClient.h"

/** Pure presentation status rules shared by the controller and its offline automation tests. */
namespace ConvaiCloudProjectsStatus
{
inline bool IsRolloutPending(const FString& Status)
{
	const FString Lower = Status.ToLower();
	return Lower == TEXT("queued") || Lower == TEXT("pending_wakeup") || Lower == TEXT("preparing")
		|| Lower == TEXT("checking_readiness") || Lower == TEXT("waiting_for_capacity") || Lower == TEXT("draining");
}

inline bool IsRolloutFailed(const FString& Status)
{
	const FString Lower = Status.ToLower();
	return Lower == TEXT("failed") || Lower == TEXT("error");
}

/** A pending rollout outranks the steady Active state for the same build. */
inline FString AvailabilityFor(const FConvaiProject& Project, const FString& BuildId)
{
	if (BuildId.IsEmpty()) return TEXT("Not active");

	if (BuildId == Project.DesiredBuildId)
	{
		if (Project.DeploymentStatus.Equals(TEXT("pending_wakeup"), ESearchCase::IgnoreCase)) return TEXT("Waiting to start");
		if (IsRolloutFailed(Project.DeploymentStatus)) return TEXT("Activation failed");
		if (IsRolloutPending(Project.DeploymentStatus)) return TEXT("Preparing");
	}
	return BuildId == Project.ActiveBuildId ? TEXT("Active") : TEXT("Not active");
}

inline FString StatusFor(const FConvaiProject& Project, bool& bOutLive, bool& bOutWorking, bool& bOutFailed)
{
	bOutLive = bOutWorking = bOutFailed = false;

	// The API returns versions newest first (ps_applications/db.py: ORDER BY av.created_at DESC).
	const FConvaiProjectVersion* Latest = Project.Versions.IsEmpty() ? nullptr : &Project.Versions[0];
	const FString BuildId = Latest ? Latest->JobId : FString();

	const FString Availability = AvailabilityFor(Project, BuildId);
	if (Availability == TEXT("Preparing") || Availability == TEXT("Waiting to start"))
	{
		bOutWorking = true;
		return Availability;
	}
	if (Availability == TEXT("Activation failed"))
	{
		bOutFailed = true;
		return Availability;
	}
	if (Availability == TEXT("Active"))
	{
		bOutLive = true;
		return TEXT("Live");
	}
	if (Latest && FConvaiProjectApiClient::IsBuildFailed(Latest->JobStatus))
	{
		bOutFailed = true;
		return TEXT("Build failed");
	}
	if (Latest && FConvaiProjectApiClient::IsBuildInProgress(Latest->JobStatus))
	{
		bOutWorking = true;
		return TEXT("Building");
	}
	if (!BuildId.IsEmpty() && BuildId == Project.DesiredBuildId)
	{
		bOutWorking = true;
		return TEXT("Activating");
	}
	if (!Project.ActiveBuildId.IsEmpty())
	{
		bOutLive = true;
		return TEXT("Live");
	}
	return Latest ? TEXT("Uploaded") : TEXT("Empty");
}
}
