// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace ConvaiMoveToArrival
{
	/** Share of Convai's acceptance radius that Move To asks Unreal to stop within. */
	constexpr float EngineAcceptanceRadiusScale = 0.25f;

	struct FEngineStop
	{
		FVector PawnLocation = FVector::ZeroVector;
		/** Colliding-bounds height; the vertical tolerance, floored at 150 uu. */
		float PawnHeight = 0.0f;
		/** Last point of the path from the latest full Convai resolve. */
		FVector PathEnd = FVector::ZeroVector;
		/** The goal's vertical extent: a movement point's Z, or the object's bounds. */
		float GoalMinZ = 0.0f;
		float GoalMaxZ = 0.0f;
		float AcceptanceRadius = 0.0f;
		float CapsuleRadius = 0.0f;
		float NavAgentRadius = 0.0f;
		bool bEngineReportedSuccess = false;
	};

	/**
	 * Whether a successful Unreal stop counts as arrival although the resolver
	 * still finds the pawn short of its tolerance around the goal. On a partial
	 * path UPathFollowingComponent stops within max(engine acceptance -
	 * |path end - goal|, 0.1 x nav agent radius) + 1.1 x capsule radius of the
	 * path end, which can lie outside that tolerance. Dropping the distance term
	 * gives a bound that also covers a full path, so the engine path, which the
	 * owned task drops on finish, is not needed.
	 */
	inline bool IsAtEngineStop(const FEngineStop& Stop)
	{
		// UPathFollowingComponent defaults: MyDefaultAcceptanceRadius and
		// MinAgentRadiusPct.
		constexpr float MinAcceptanceNavAgentRadiusScale = 0.1f;
		constexpr float ReachCapsuleRadiusScale = 1.1f;
		constexpr float StopSlack = 5.0f;

		if (!Stop.bEngineReportedSuccess)
		{
			return false;
		}

		const float StopRadius =
			FMath::Max(
				Stop.AcceptanceRadius * EngineAcceptanceRadiusScale,
				Stop.NavAgentRadius * MinAcceptanceNavAgentRadiusScale) +
			Stop.CapsuleRadius * ReachCapsuleRadiusScale + StopSlack;
		if (FVector::DistSquared2D(Stop.PawnLocation, Stop.PathEnd) >
			FMath::Square(StopRadius))
		{
			return false;
		}

		// The resolver's vertical-span test (WithinVerticalSpan in ConvaiDefinitions.cpp).
		const float PawnZ = static_cast<float>(Stop.PawnLocation.Z);
		const float NearestGoalZ =
			FMath::Clamp(PawnZ, Stop.GoalMinZ, Stop.GoalMaxZ);
		return FMath::Abs(PawnZ - NearestGoalZ) <=
			FMath::Max(Stop.PawnHeight, 150.0f);
	}
}
