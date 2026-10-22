// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace ConvaiFaceSyncSmoothing
{
	/** One evaluation of the Face Sync node's curve smoothing: Previous moves toward Target by Speed's share of
	 *  the distance per 1/60 s, so the result does not depend on the frame rate. A speed outside 0-1 is clamped:
	 *  above 1 the step overshoots, or is NaN when the frame is not a whole number of sixtieths; below 0 it moves
	 *  away from the target. */
	inline float Step(float Previous, float Target, float Speed, float DeltaTime)
	{
		const float SmoothFactor = 1.0f - FMath::Pow(1.0f - FMath::Clamp(Speed, 0.0f, 1.0f), DeltaTime * 60.0f);
		const float Smoothed = Previous + SmoothFactor * (Target - Previous);
		return FMath::IsFinite(Smoothed) ? Smoothed : Target;
	}
}
