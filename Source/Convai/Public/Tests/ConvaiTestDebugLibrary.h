// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#if WITH_TESTS

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ConvaiTestDebugLibrary.generated.h"

/**
 * Blueprint library for test-time debugging helpers.
 * These are test-only utilities and should not be used in production code.
 */
UCLASS()
class CONVAI_API UConvaiTestDebugLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Debug helper: freeze game and/or audio threads in parallel.
	 * Used to stress-test playback time estimation under hitches.
	 * @param DurationMs Duration of freeze in milliseconds
	 * @param bFreezeGameThread If true, blocks the game thread
	 * @param bFreezeAudioThread If true, blocks the audio thread
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Test|Debug")
	static void FreezeThreads(float DurationMs = 1000.0f, bool bFreezeGameThread = true, bool bFreezeAudioThread = true);
};
#endif // WITH_TESTS
