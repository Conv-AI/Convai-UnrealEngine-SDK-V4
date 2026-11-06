// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneAutoTaggerTypes.h"

class UConvaiSceneAutoTaggerSettings;
class UWorld;
class AActor;

struct FSceneAutoTaggerDiscoveryStats
{
	int32 ScannedActors = 0;
	int32 ExcludedInfrastructure = 0;
	int32 ExcludedNoRenderableGeometry = 0;
	int32 ExcludedBasicShapes = 0;
	int32 ExcludedAlreadyTagged = 0;
	int32 BelowThreshold = 0;
	int32 IncludedActors = 0;
};

/** Deterministic, explainable first-pass filter. Vision is only spent on actors that pass it. */
class CONVAISCENETAGGING_API FSceneAutoTaggerDiscovery
{
public:
	static void Discover(
		UWorld* World,
		const FSceneAutoTaggerRunOptions& Options,
		const UConvaiSceneAutoTaggerSettings& Settings,
		TArray<TSharedPtr<FSceneAutoTaggerCandidate>>& OutCandidates,
		FSceneAutoTaggerDiscoveryStats& OutStats);

	/** Refresh only actor-owned render components and bounds without rerunning eligibility. */
	static bool RefreshCandidateGeometry(
		AActor& Actor,
		FSceneAutoTaggerCandidate& Candidate);

	static FString MakeReadableObjectName(const FSceneAutoTaggerCandidate& Candidate);
	static void ApplyHeuristicFallback(FSceneAutoTaggerCandidate& Candidate, const FString& Reason = FString());

private:
	static bool ContainsAnyFragment(const FString& LowerHaystack, const TArray<FString>& Fragments, FString* OutMatch = nullptr);
};
