// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_TESTS
#include <atomic>
#endif

/** What the connect reaper shares with the rest of the module and its unit
 *  rows, kept out of the public header. */
namespace ConvaiConnectReaperInternal
{
	/** Runs a job that can block for seconds (a connect attempt's teardown, a
	 *  push-to-talk drain) on a thread of its own. On an engine worker it would
	 *  park that worker for as long as it blocks, and once the few background
	 *  workers are parked every background-priority engine task waits. */
	void RunOnOwnThread(TUniqueFunction<void()> Job);

#if WITH_TESTS
	/** Jobs RunOnOwnThread found running on an engine scheduler worker. */
	extern std::atomic<int32> JobsOnEngineWorkersForTests;

	/** How long a torn-down client waits before it is destroyed; 2 s unless a
	 *  test sets it. Abandon reads it once per attempt, so a scope that puts it
	 *  back never hands an attempt already in flight the full delay. */
	extern std::atomic<float> DestroyDelaySecondsForTests;

	/** Teardowns that found their client still held elsewhere and waited for
	 *  it to be let go. */
	extern std::atomic<int32> RetainWaitsForTests;
#endif
}
