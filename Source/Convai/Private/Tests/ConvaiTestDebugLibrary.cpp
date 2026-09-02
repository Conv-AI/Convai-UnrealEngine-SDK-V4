// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "Tests/ConvaiTestDebugLibrary.h"
#include "HAL/PlatformProcess.h"
#include "AudioThread.h"
#include "Utility/Log/ConvaiLogger.h"

DEFINE_LOG_CATEGORY_STATIC(ConvaiTestDebugLog, Log, All);

void UConvaiTestDebugLibrary::FreezeThreads(float DurationMs, bool bFreezeGameThread, bool bFreezeAudioThread)
{
	const float DurationSec = FMath::Max(0.0f, DurationMs) / 1000.0f;

	// Dispatch audio thread freeze *before* blocking game thread, so they run in parallel.
	if (bFreezeAudioThread)
	{
		FAudioThread::RunCommandOnAudioThread([DurationSec, DurationMs]()
		{
			UE_LOG(ConvaiTestDebugLog, Warning, TEXT("FreezeThreads: blocking audio thread for %.0f ms"), DurationMs);
			FPlatformProcess::Sleep(DurationSec);
		});
	}

	// Block game thread after dispatching audio thread work.
	if (bFreezeGameThread)
	{
		UE_LOG(ConvaiTestDebugLog, Warning, TEXT("FreezeThreads: blocking game thread for %.0f ms"), DurationMs);
		FPlatformProcess::Sleep(DurationSec);
	}
}
 
#endif // WITH_TESTS
