// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Custom-param timings and rates as the plugin uses them. The Blueprint getters
 * keep returning the raw parsed value; these replace a value the plugin cannot
 * use, and the first replacement of each key in a process logs a Warning naming
 * the key and both values.
 */
namespace ConvaiSettingsBounds
{
	/** At least 0.1 s: a zero delay sends client-ready every frame. */
	float ClientReadyRetrySecs();

	/** At least 5 s: a zero timeout ends every new Session on its first tick. */
	float ClientReadyTimeoutSecs();

	/** Any finite value from 0, else the 0.25 s default. No floor: designers get
	 *  the cadence they ask for. */
	float ObjectPollIntervalSeconds();

	/** Above 0, else the defaults 10 and 90. */
	int32 ChunkSize();
	int32 OutputFPS();
}
