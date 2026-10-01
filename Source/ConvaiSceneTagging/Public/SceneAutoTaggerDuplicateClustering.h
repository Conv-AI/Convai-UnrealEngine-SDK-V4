
// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiSceneAutoTaggerSettings.h"

namespace ConvaiSceneAutoTagger
{
/** Pure, run-local input used to divide one verified duplicate set into spatial groups. */
struct FSceneAutoTaggerDuplicateClusteringItem
{
	/** Stable candidate identifier within the controller's current exploration. Must be unique. */
	int32 CandidateIndex = INDEX_NONE;

	/** DuplicateRepresentative/proof-group index. INDEX_NONE means no verified duplicate proof. */
	int32 ProofGroup = INDEX_NONE;

	/** Stable editor identity used for deterministic ordering and merge-group IDs. */
	FString ActorPath;

	/** Current world-space actor or bounds center. */
	FVector Location = FVector::ZeroVector;

	/** False when Location could not be resolved. Non-finite coordinates are also treated as invalid. */
	bool bHasValidLocation = false;
};

/** One logical object group emitted by the duplicate-clustering policy. */
struct FSceneAutoTaggerDuplicateCluster
{
	int32 ProofGroup = INDEX_NONE;
	TArray<int32> CandidateIndices;
	TArray<FString> ActorPaths;

	/** Stable, positive high-range identity for a merged cluster; zero for a singleton. */
	int32 MergeGroupIndex = 0;

	/** Stable owner token for collision-free name allocation within this plan. */
	FString OwnerKey;

	bool IsMerged() const
	{
		return CandidateIndices.Num() > 1 && MergeGroupIndex != 0;
	}
};

/** Deterministic clustering result consumed by both name allocation and apply requests. */
struct CONVAISCENETAGGING_API FSceneAutoTaggerDuplicateClusteringPlan
{
	TArray<FSceneAutoTaggerDuplicateCluster> Clusters;
	TMap<int32, int32> ClusterIndexByCandidate;

	int32 FindClusterIndexForCandidate(int32 CandidateIndex) const;
	const FSceneAutoTaggerDuplicateCluster* FindClusterForCandidate(int32 CandidateIndex) const;
	bool ShouldMergeCandidate(int32 CandidateIndex) const;
	int32 GetMergeGroupIndexForCandidate(int32 CandidateIndex) const;
	FString GetOwnerKeyForCandidate(int32 CandidateIndex) const;
};

/**
 * Builds a pure duplicate grouping plan.
 *
 * Duplicate proof is an immutable outer boundary: items with different proof groups never merge.
 * NearbyClusters uses deterministic, stable-order complete-link assignment, so every pair in
 * an emitted group satisfies the horizontal limit and the group's full vertical span satisfies
 * the vertical limit. Each prior item is visited at most once per new item (O(n^2)).
 */
CONVAISCENETAGGING_API FSceneAutoTaggerDuplicateClusteringPlan BuildDuplicateClusteringPlan(
	TConstArrayView<FSceneAutoTaggerDuplicateClusteringItem> Items,
	EConvaiSceneAutoTaggerDuplicateGroupingMode Mode,
	double MaxHorizontalSpanCm,
	double MaxVerticalSpanCm);
}
