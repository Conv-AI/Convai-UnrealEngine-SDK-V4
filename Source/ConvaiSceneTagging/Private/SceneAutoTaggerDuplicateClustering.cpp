
// Copyright Convai. All Rights Reserved.

#include "SceneAutoTaggerDuplicateClustering.h"

#include "Misc/Crc.h"

namespace ConvaiSceneAutoTagger
{
namespace
{
struct FWorkingCluster
{
	int32 ProofGroup = INDEX_NONE;
	/** Indices into the normalized, stably sorted item array. */
	TArray<int32> ItemIndices;
	double MaxHorizontalSquared = 0.0;
	double MinZ = 0.0;
	double MaxZ = 0.0;
};

bool IsValidProofGroup(const int32 ProofGroup)
{
	return ProofGroup >= 0;
}

bool HasUsableLocation(const FSceneAutoTaggerDuplicateClusteringItem& Item)
{
	return Item.bHasValidLocation
		&& FMath::IsFinite(Item.Location.X)
		&& FMath::IsFinite(Item.Location.Y)
		&& FMath::IsFinite(Item.Location.Z);
}

bool StableItemLess(
	const FSceneAutoTaggerDuplicateClusteringItem& Left,
	const FSceneAutoTaggerDuplicateClusteringItem& Right)
{
	if (Left.ProofGroup != Right.ProofGroup)
	{
		return Left.ProofGroup < Right.ProofGroup;
	}

	const int32 PathComparison = Left.ActorPath.Compare(
		Right.ActorPath,
		ESearchCase::CaseSensitive);
	if (PathComparison != 0)
	{
		return PathComparison < 0;
	}
	return Left.CandidateIndex < Right.CandidateIndex;
}

FWorkingCluster MakeSingletonCluster(
	const TArray<FSceneAutoTaggerDuplicateClusteringItem>& Items,
	const int32 ItemIndex)
{
	FWorkingCluster Cluster;
	Cluster.ProofGroup = Items[ItemIndex].ProofGroup;
	Cluster.ItemIndices.Add(ItemIndex);
	Cluster.MinZ = Items[ItemIndex].Location.Z;
	Cluster.MaxZ = Items[ItemIndex].Location.Z;
	return Cluster;
}

void MeasureAppendSpan(
	const FWorkingCluster& Cluster,
	const int32 CandidateItemIndex,
	const TArray<FSceneAutoTaggerDuplicateClusteringItem>& Items,
	double& OutMaxHorizontalSquared,
	double& OutMinZ,
	double& OutMaxZ)
{
	const FVector& CandidateLocation = Items[CandidateItemIndex].Location;
	OutMaxHorizontalSquared = Cluster.MaxHorizontalSquared;
	OutMinZ = FMath::Min(Cluster.MinZ, CandidateLocation.Z);
	OutMaxZ = FMath::Max(Cluster.MaxZ, CandidateLocation.Z);
	for (const int32 ExistingItemIndex : Cluster.ItemIndices)
	{
		const FVector& ExistingLocation = Items[ExistingItemIndex].Location;
		const double DeltaX = ExistingLocation.X - CandidateLocation.X;
		const double DeltaY = ExistingLocation.Y - CandidateLocation.Y;
		OutMaxHorizontalSquared = FMath::Max(
			OutMaxHorizontalSquared,
			DeltaX * DeltaX + DeltaY * DeltaY);
	}
}

bool IsBetterAppend(
	const double CandidateHorizontalSquared,
	const double CandidateVerticalSpan,
	const int32 CandidateClusterIndex,
	const double BestHorizontalSquared,
	const double BestVerticalSpan,
	const int32 BestClusterIndex)
{
	if (BestClusterIndex == INDEX_NONE)
	{
		return true;
	}
	if (CandidateHorizontalSquared != BestHorizontalSquared)
	{
		return CandidateHorizontalSquared < BestHorizontalSquared;
	}
	if (CandidateVerticalSpan != BestVerticalSpan)
	{
		return CandidateVerticalSpan < BestVerticalSpan;
	}
	return CandidateClusterIndex < BestClusterIndex;
}

void BuildNearbyClustersForProofGroup(
	const TArray<int32>& ItemIndices,
	const TArray<FSceneAutoTaggerDuplicateClusteringItem>& Items,
	const double MaxHorizontalSquared,
	const double MaxVerticalSpan,
	TArray<FWorkingCluster>& OutClusters)
{
	TArray<FWorkingCluster> WorkingClusters;
	WorkingClusters.Reserve(ItemIndices.Num());
	for (const int32 ItemIndex : ItemIndices)
	{
		int32 BestClusterIndex = INDEX_NONE;
		double BestHorizontalSquared = 0.0;
		double BestMinZ = 0.0;
		double BestMaxZ = 0.0;
		for (int32 ClusterIndex = 0; ClusterIndex < WorkingClusters.Num(); ++ClusterIndex)
		{
			double CandidateHorizontalSquared = 0.0;
			double CandidateMinZ = 0.0;
			double CandidateMaxZ = 0.0;
			MeasureAppendSpan(
				WorkingClusters[ClusterIndex],
				ItemIndex,
				Items,
				CandidateHorizontalSquared,
				CandidateMinZ,
				CandidateMaxZ);
			const double CandidateVerticalSpan = CandidateMaxZ - CandidateMinZ;
			if (CandidateHorizontalSquared > MaxHorizontalSquared
				|| CandidateVerticalSpan > MaxVerticalSpan)
			{
				continue;
			}
			if (IsBetterAppend(
					CandidateHorizontalSquared,
					CandidateVerticalSpan,
					ClusterIndex,
					BestHorizontalSquared,
					BestMaxZ - BestMinZ,
					BestClusterIndex))
			{
				BestClusterIndex = ClusterIndex;
				BestHorizontalSquared = CandidateHorizontalSquared;
				BestMinZ = CandidateMinZ;
				BestMaxZ = CandidateMaxZ;
			}
		}
		if (BestClusterIndex == INDEX_NONE)
		{
			WorkingClusters.Add(MakeSingletonCluster(Items, ItemIndex));
			continue;
		}
		FWorkingCluster& BestCluster = WorkingClusters[BestClusterIndex];
		BestCluster.ItemIndices.Add(ItemIndex);
		BestCluster.MaxHorizontalSquared = BestHorizontalSquared;
		BestCluster.MinZ = BestMinZ;
		BestCluster.MaxZ = BestMaxZ;
	}

	for (FWorkingCluster& Cluster : WorkingClusters)
	{
		OutClusters.Add(MoveTemp(Cluster));
	}
}

int32 BuildStableMergeGroupIndex(
	const TArray<FString>& SortedActorPaths,
	const int32 CollisionSalt)
{
	FString Identity = FString::Join(SortedActorPaths, TEXT("\n"));
	if (CollisionSalt > 0)
	{
		Identity += FString::Printf(TEXT("\n#collision:%d"), CollisionSalt);
	}
	const uint32 Hash = FCrc::StrCrc32(*Identity);
	return static_cast<int32>(0x40000000u | (Hash & 0x3fffffffu));
}

FString BuildSingletonOwnerKey(const FSceneAutoTaggerDuplicateCluster& Cluster)
{
	check(Cluster.CandidateIndices.Num() == 1);
	check(Cluster.ActorPaths.Num() == 1);
	return FString::Printf(
		TEXT("candidate:%d:%d:%s"),
		Cluster.ProofGroup,
		Cluster.CandidateIndices[0],
		*Cluster.ActorPaths[0]);
}
}

int32 FSceneAutoTaggerDuplicateClusteringPlan::FindClusterIndexForCandidate(
	const int32 CandidateIndex) const
{
	if (const int32* ClusterIndex = ClusterIndexByCandidate.Find(CandidateIndex))
	{
		return *ClusterIndex;
	}
	return INDEX_NONE;
}

const FSceneAutoTaggerDuplicateCluster*
FSceneAutoTaggerDuplicateClusteringPlan::FindClusterForCandidate(
	const int32 CandidateIndex) const
{
	const int32 ClusterIndex = FindClusterIndexForCandidate(CandidateIndex);
	return Clusters.IsValidIndex(ClusterIndex) ? &Clusters[ClusterIndex] : nullptr;
}

bool FSceneAutoTaggerDuplicateClusteringPlan::ShouldMergeCandidate(
	const int32 CandidateIndex) const
{
	const FSceneAutoTaggerDuplicateCluster* Cluster = FindClusterForCandidate(CandidateIndex);
	return Cluster && Cluster->IsMerged();
}

int32 FSceneAutoTaggerDuplicateClusteringPlan::GetMergeGroupIndexForCandidate(
	const int32 CandidateIndex) const
{
	const FSceneAutoTaggerDuplicateCluster* Cluster = FindClusterForCandidate(CandidateIndex);
	return Cluster ? Cluster->MergeGroupIndex : 0;
}

FString FSceneAutoTaggerDuplicateClusteringPlan::GetOwnerKeyForCandidate(
	const int32 CandidateIndex) const
{
	const FSceneAutoTaggerDuplicateCluster* Cluster = FindClusterForCandidate(CandidateIndex);
	return Cluster ? Cluster->OwnerKey : FString();
}

FSceneAutoTaggerDuplicateClusteringPlan BuildDuplicateClusteringPlan(
	const TConstArrayView<FSceneAutoTaggerDuplicateClusteringItem> Items,
	const EConvaiSceneAutoTaggerDuplicateGroupingMode Mode,
	const double MaxHorizontalSpanCm,
	const double MaxVerticalSpanCm)
{
	TArray<FSceneAutoTaggerDuplicateClusteringItem> SortedItems;
	SortedItems.Reserve(Items.Num());
	for (const FSceneAutoTaggerDuplicateClusteringItem& Item : Items)
	{
		SortedItems.Add(Item);
	}
	SortedItems.Sort(StableItemLess);

	TArray<FWorkingCluster> WorkingClusters;
	WorkingClusters.Reserve(SortedItems.Num());

	EConvaiSceneAutoTaggerDuplicateGroupingMode EffectiveMode =
		EConvaiSceneAutoTaggerDuplicateGroupingMode::KeepSeparate;
	switch (Mode)
	{
	case EConvaiSceneAutoTaggerDuplicateGroupingMode::Legacy:
		EffectiveMode = EConvaiSceneAutoTaggerDuplicateGroupingMode::MergeAllVerified;
		break;
	case EConvaiSceneAutoTaggerDuplicateGroupingMode::KeepSeparate:
	case EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters:
	case EConvaiSceneAutoTaggerDuplicateGroupingMode::MergeAllVerified:
		EffectiveMode = Mode;
		break;
	default:
		// Unknown values from a newer or corrupted config fail closed: never
		// combine actors without a policy this build understands.
		break;
	}

	if (EffectiveMode == EConvaiSceneAutoTaggerDuplicateGroupingMode::KeepSeparate)
	{
		for (int32 ItemIndex = 0; ItemIndex < SortedItems.Num(); ++ItemIndex)
		{
			WorkingClusters.Add(MakeSingletonCluster(SortedItems, ItemIndex));
		}
	}
	else
	{
		TMap<int32, TArray<int32>> ItemIndicesByProofGroup;
		for (int32 ItemIndex = 0; ItemIndex < SortedItems.Num(); ++ItemIndex)
		{
			const FSceneAutoTaggerDuplicateClusteringItem& Item = SortedItems[ItemIndex];
			const bool bNeedsLocation =
				EffectiveMode == EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters;
			if (!IsValidProofGroup(Item.ProofGroup)
				|| (bNeedsLocation && !HasUsableLocation(Item)))
			{
				WorkingClusters.Add(MakeSingletonCluster(SortedItems, ItemIndex));
				continue;
			}
			ItemIndicesByProofGroup.FindOrAdd(Item.ProofGroup).Add(ItemIndex);
		}

		TArray<int32> ProofGroups;
		ItemIndicesByProofGroup.GenerateKeyArray(ProofGroups);
		ProofGroups.Sort();
		for (const int32 ProofGroup : ProofGroups)
		{
			const TArray<int32>& GroupItems = ItemIndicesByProofGroup.FindChecked(ProofGroup);
			if (EffectiveMode == EConvaiSceneAutoTaggerDuplicateGroupingMode::MergeAllVerified)
			{
				FWorkingCluster Cluster;
				Cluster.ProofGroup = ProofGroup;
				Cluster.ItemIndices = GroupItems;
				WorkingClusters.Add(MoveTemp(Cluster));
				continue;
			}

			const double HorizontalLimit = FMath::IsFinite(MaxHorizontalSpanCm)
				? FMath::Max(0.0, MaxHorizontalSpanCm)
				: 0.0;
			const double VerticalLimit = FMath::IsFinite(MaxVerticalSpanCm)
				? FMath::Max(0.0, MaxVerticalSpanCm)
				: 0.0;
			BuildNearbyClustersForProofGroup(
				GroupItems,
				SortedItems,
				HorizontalLimit * HorizontalLimit,
				VerticalLimit,
				WorkingClusters);
		}
	}

	// All item indices refer to the same normalized item order, so this final sort is
	// deterministic and independent of map iteration or caller input order.
	WorkingClusters.Sort([](const FWorkingCluster& Left, const FWorkingCluster& Right)
	{
		if (Left.ProofGroup != Right.ProofGroup)
		{
			return Left.ProofGroup < Right.ProofGroup;
		}
		if (Left.ItemIndices.IsEmpty() || Right.ItemIndices.IsEmpty())
		{
			return Left.ItemIndices.Num() < Right.ItemIndices.Num();
		}
		return Left.ItemIndices[0] < Right.ItemIndices[0];
	});

	FSceneAutoTaggerDuplicateClusteringPlan Plan;
	Plan.Clusters.Reserve(WorkingClusters.Num());
	TSet<int32> UsedMergeGroupIndices;
	for (FWorkingCluster& WorkingCluster : WorkingClusters)
	{
		FSceneAutoTaggerDuplicateCluster& Cluster = Plan.Clusters.AddDefaulted_GetRef();
		Cluster.ProofGroup = WorkingCluster.ProofGroup;
		for (const int32 ItemIndex : WorkingCluster.ItemIndices)
		{
			Cluster.CandidateIndices.Add(SortedItems[ItemIndex].CandidateIndex);
			Cluster.ActorPaths.Add(SortedItems[ItemIndex].ActorPath);
		}

		if (Cluster.CandidateIndices.Num() > 1)
		{
			int32 CollisionSalt = 0;
			do
			{
				Cluster.MergeGroupIndex = BuildStableMergeGroupIndex(
					Cluster.ActorPaths,
					CollisionSalt++);
			}
			while (UsedMergeGroupIndices.Contains(Cluster.MergeGroupIndex));
			UsedMergeGroupIndices.Add(Cluster.MergeGroupIndex);
			Cluster.OwnerKey = FString::Printf(
				TEXT("merge:%08X"),
				static_cast<uint32>(Cluster.MergeGroupIndex));
		}
		else
		{
			Cluster.MergeGroupIndex = 0;
			Cluster.OwnerKey = BuildSingletonOwnerKey(Cluster);
		}
	}

	for (int32 ClusterIndex = 0; ClusterIndex < Plan.Clusters.Num(); ++ClusterIndex)
	{
		for (const int32 CandidateIndex : Plan.Clusters[ClusterIndex].CandidateIndices)
		{
			Plan.ClusterIndexByCandidate.Add(CandidateIndex, ClusterIndex);
		}
	}
	return Plan;
}
}
