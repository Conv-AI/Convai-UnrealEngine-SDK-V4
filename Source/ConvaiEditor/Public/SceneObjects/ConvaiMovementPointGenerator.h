// Copyright Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UConvaiObjectComponent;

enum class EConvaiMovementPointGenerationOperation : uint8
{
	Generate,
	Regenerate,
	ClearGenerated,
};

struct FConvaiMovementPointGenerationResult
{
	int32 RequestedComponents = 0;
	int32 ChangedComponents = 0;
	int32 SkippedComponents = 0;
	int32 GeneratedPoints = 0;
	int32 RemovedGeneratedPoints = 0;
	int32 PreservedManualPoints = 0;
	int32 ExistingPointComponents = 0;
	int32 NoGeneratedPointComponents = 0;
	int32 InvalidComponents = 0;
	int32 NonEditableComponents = 0;
	int32 MissingNavigationComponents = 0;
	int32 NoValidCandidateComponents = 0;
};

/**
 * Editor-only generation of plausible standing locations for saved Convai objects.
 * Runtime reachability remains authoritative because it depends on the character
 * that eventually performs the action.
 */
class CONVAIEDITOR_API FConvaiMovementPointGenerator
{
public:
	/** Set bCreateTransaction=false only when the caller already owns the editor transaction. */
	static FConvaiMovementPointGenerationResult Apply(
		const TArray<TWeakObjectPtr<UConvaiObjectComponent>>& Components,
		EConvaiMovementPointGenerationOperation Operation,
		bool bCreateTransaction = true);

	static int32 CountGeneratedPoints(const UConvaiObjectComponent& Component);
	static int32 CountManualPoints(const UConvaiObjectComponent& Component);
	static bool CanEditMovementPoints(
		const UConvaiObjectComponent& Component,
		FText& OutReason);

#if WITH_DEV_AUTOMATION_TESTS
	static FName BuildGeneratedMarkerForTesting(const struct FConvaiMovementPoint& Point);
	static int32 StripGeneratedPointsForTesting(
		TArray<struct FConvaiMovementPoint>& Points,
		TArray<FName>& ComponentTags);
	static void SelectCandidateIndicesForTesting(
		const TArray<FVector2D>& Directions,
		const TArray<float>& Scores,
		TArray<int32>& OutIndices);
	static bool IsVisibilityHitAcceptedForTesting(
		bool bComponentScoped,
		bool bHitOwner,
		bool bHitTargetComponent);
	static float ComputeSurfaceStandOffDistanceForTesting(
		const FVector& TargetExtent,
		float AgentRadius);
	static FVector ComputeFootprintSeedLocationForTesting(
		const FBox& TargetBounds,
		const FVector2D& Direction,
		float StandOffDistance);
	static FVector2D ComputeCandidateDirectionForTesting(
		const FVector& TargetCenter,
		const FVector& CandidateLocation);
	static bool IsProjectedSurfaceGapAcceptedForTesting(
		float SurfaceGap,
		float StandOffDistance,
		float AgentRadius);
	static void BuildVisibilityTargetSamplesForTesting(
		const FBox& TargetBounds,
		const FVector& EyeLocation,
		TArray<FVector>& OutSamples);
	static bool IsVisibilityCoverageAcceptedForTesting(
		bool bCenterVisible,
		int32 VisibleSamples,
		int32 TotalSamples);
#endif
};
