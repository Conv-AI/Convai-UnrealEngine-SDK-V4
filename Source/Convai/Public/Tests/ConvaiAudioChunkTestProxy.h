// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Net/OnlineBlueprintCallProxyBase.h"
#include "Engine/TimerHandle.h"
#include "ConvaiAudioChunkTestProxy.generated.h"

class UConvaiAudioStreamer;
class USoundWave;

DECLARE_LOG_CATEGORY_EXTERN(ConvaiAudioChunkTestLog, Log, All);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnAudioChunkTestComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAudioChunkTestFailed, FString, ErrorMessage);

/**
 * Preset test plans for testing the tiered gear system.
 * Gears: 0.25s, 0.5s, 1.0s, 2.0s
 * Time lag threshold: 0.2s (if processed_time - elapsed > 0.2s, wait)
 */
UENUM(BlueprintType)
enum class EAudioChunkTestPlan : uint8
{
	/** Custom - use provided ChunkDurations and DelaysBetweenChunks */
	Custom UMETA(DisplayName = "Custom"),

	/** Jump to Gear 3 (2s) immediately - send 2.5s chunk at start */
	JumpToGear3 UMETA(DisplayName = "Jump to Gear 3 (2s) Immediately"),

	/** Jump to Gear 2 (1s) immediately - send 1.2s chunk at start */
	JumpToGear2 UMETA(DisplayName = "Jump to Gear 2 (1s) Immediately"),

	/** Stay in Gear 0 then gradually climb - small chunks with delays matching playback */
	GradualClimb UMETA(DisplayName = "Gradual Climb Through Gears"),

	/** Simulate slow network - small chunks with variable delays */
	SlowNetwork UMETA(DisplayName = "Slow Network Simulation"),

	/** Simulate fast burst then slow - large initial chunk then small delayed ones */
	BurstThenSlow UMETA(DisplayName = "Fast Burst Then Slow"),
};

/**
 * Test proxy for simulating chunked audio streaming to test the tiered gear system.
 * Takes audio, breaks it into specified durations, and sends chunks at specified intervals.
 */
UCLASS()
class CONVAI_API UConvaiAudioChunkTestProxy : public UOnlineBlueprintCallProxyBase
{
	GENERATED_BODY()

public:
	// Called when all chunks have been sent
	UPROPERTY(BlueprintAssignable)
	FOnAudioChunkTestComplete OnComplete;

	// Called if there's an error
	UPROPERTY(BlueprintAssignable)
	FOnAudioChunkTestFailed OnFailed;

	/**
	 * Test chunked audio streaming with specified chunk sizes and delays.
	 *
	 * @param AudioStreamer		The audio streamer to send chunks to
	 * @param AudioToPlay		The source audio to break into chunks
	 * @param ChunkDurations	Array of chunk durations in seconds (e.g., 0.1, 0.2, 0.5, 0.3, 0.8)
	 * @param DelaysBetweenChunks	Array of delays before sending each chunk in seconds (e.g., 0, 0.1, 0.05, 0.2)
	 *                              First value is delay before first chunk, etc.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", DisplayName = "Test Chunked Audio Streaming (Custom)", WorldContext = "WorldContextObject"), Category = "Convai|Debug|Tests")
	static UConvaiAudioChunkTestProxy* CreateAudioChunkTestProxy(
		UObject* WorldContextObject,
		UConvaiAudioStreamer* AudioStreamer,
		USoundWave* AudioToPlay,
		const TArray<float>& ChunkDurations,
		const TArray<float>& DelaysBetweenChunks);

	/**
	 * Test chunked audio streaming using a preset test plan.
	 *
	 * @param AudioStreamer		The audio streamer to send chunks to
	 * @param AudioToPlay		The source audio (should be at least 5s for full test)
	 * @param TestPlan			The preset test plan to use
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", DisplayName = "Test Chunked Audio Streaming (Preset)", WorldContext = "WorldContextObject"), Category = "Convai|Debug|Tests")
	static UConvaiAudioChunkTestProxy* CreateAudioChunkTestProxyWithPlan(
		UObject* WorldContextObject,
		UConvaiAudioStreamer* AudioStreamer,
		USoundWave* AudioToPlay,
		EAudioChunkTestPlan TestPlan);

	virtual void Activate() override;

	/** Get chunk durations and delays for a given test plan */
	static void GetTestPlanParameters(EAudioChunkTestPlan TestPlan, TArray<float>& OutChunkDurations, TArray<float>& OutDelays);

private:
	void SendNextChunk();
	void ScheduleNextChunk();
	void Complete();
	void Fail(const FString& ErrorMessage);

	// Inputs
	UPROPERTY()
	TWeakObjectPtr<UConvaiAudioStreamer> AudioStreamerPtr;
	
	UPROPERTY()
	USoundWave* SourceAudio;
	
	TArray<float> ChunkDurationsSeconds;
	TArray<float> DelaysSeconds;

	// State
	TArray<uint8> FullPCMData;
	int32 SampleRate;
	int32 NumChannels;
	int32 BytesPerSecond;
	int32 CurrentChunkIndex;
	int32 CurrentByteOffset;

	// Timer handle for scheduling chunks
	FTimerHandle ChunkTimerHandle;

	// Pointer to the world
	TWeakObjectPtr<UWorld> WorldPtr;
};

