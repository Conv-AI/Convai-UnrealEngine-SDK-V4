// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiAudioChunkTestProxy.h"
#include "ConvaiAudioStreamer.h"
#include "ConvaiUtils.h"
#include "Sound/SoundWave.h"
#include "TimerManager.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY(ConvaiAudioChunkTestLog);

void UConvaiAudioChunkTestProxy::GetTestPlanParameters(EAudioChunkTestPlan TestPlan, TArray<float>& OutChunkDurations, TArray<float>& OutDelays)
{
	OutChunkDurations.Empty();
	OutDelays.Empty();

	switch (TestPlan)
	{
	case EAudioChunkTestPlan::JumpToGear3:
		// Send 2.5s immediately - buffer will be >= 2s, should jump to gear 3 (2s)
		// Then send remaining in smaller chunks to keep feeding
		OutChunkDurations = { 2.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f };
		OutDelays = { 0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
		break;

	case EAudioChunkTestPlan::JumpToGear2:
		// Send 1.2s immediately - buffer will be >= 1s, should jump to gear 2 (1s)
		// Then send more to keep buffer fed
		OutChunkDurations = { 1.2f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f };
		OutDelays = { 0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
		break;

	case EAudioChunkTestPlan::GradualClimb:
		// Start with small chunks, let buffer accumulate to climb gears
		// Chunk 1: 0.3s at t=0 -> buffer=0.3s, gear 0 (0.25s) selected
		// After 0.2s lag threshold, ingestion happens, buffer drops
		// Chunk 2-3: buffer builds up, eventually hits gear 1 threshold
		// Later chunks: buffer continues building, hits higher gears
		OutChunkDurations = { 0.3f, 0.3f, 0.4f, 0.5f, 0.6f, 0.8f, 1.0f, 1.2f };
		OutDelays = { 0.0f, 0.1f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.4f };
		break;

	case EAudioChunkTestPlan::SlowNetwork:
		// Small chunks with delays simulating network jitter
		// Buffer stays small, should stay in low gears
		OutChunkDurations = { 0.2f, 0.15f, 0.25f, 0.2f, 0.3f, 0.2f, 0.25f, 0.2f };
		OutDelays = { 0.0f, 0.25f, 0.2f, 0.3f, 0.15f, 0.35f, 0.2f, 0.25f };
		break;

	case EAudioChunkTestPlan::BurstThenSlow:
		// Large initial burst (1.5s) then small slow chunks
		// Should start at gear 2 (1s), then struggle as buffer depletes
		OutChunkDurations = { 1.5f, 0.2f, 0.2f, 0.2f, 0.2f, 0.2f, 0.2f };
		OutDelays = { 0.0f, 1.2f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f };
		break;

	case EAudioChunkTestPlan::Custom:
	default:
		// Empty - caller should provide their own
		break;
	}
}

UConvaiAudioChunkTestProxy* UConvaiAudioChunkTestProxy::CreateAudioChunkTestProxyWithPlan(
	UObject* WorldContextObject,
	UConvaiAudioStreamer* AudioStreamer,
	USoundWave* AudioToPlay,
	EAudioChunkTestPlan TestPlan)
{
	TArray<float> ChunkDurations;
	TArray<float> Delays;
	GetTestPlanParameters(TestPlan, ChunkDurations, Delays);

	return CreateAudioChunkTestProxy(WorldContextObject, AudioStreamer, AudioToPlay, ChunkDurations, Delays);
}

UConvaiAudioChunkTestProxy* UConvaiAudioChunkTestProxy::CreateAudioChunkTestProxyWithInterval(
	UObject* WorldContextObject,
	UConvaiAudioStreamer* AudioStreamer,
	USoundWave* AudioToPlay,
	float ChunkDurationSeconds,
	float IntervalSeconds)
{
	if (!AudioToPlay)
	{
		UE_LOG(ConvaiAudioChunkTestLog, Error, TEXT("CreateAudioChunkTestProxyWithInterval: AudioToPlay is null"));
		return nullptr;
	}

	// Extract audio duration to calculate how many chunks we need
	float AudioDuration = AudioToPlay->Duration;
	if (AudioDuration <= 0.0f)
	{
		UE_LOG(ConvaiAudioChunkTestLog, Error, TEXT("CreateAudioChunkTestProxyWithInterval: Invalid audio duration"));
		return nullptr;
	}

	// Calculate number of chunks needed to cover the entire audio
	int32 NumChunks = FMath::CeilToInt(AudioDuration / ChunkDurationSeconds);

	// Create arrays with uniform chunk durations and intervals
	TArray<float> ChunkDurations;
	TArray<float> Delays;
	ChunkDurations.Reserve(NumChunks);
	Delays.Reserve(NumChunks);

	for (int32 i = 0; i < NumChunks; ++i)
	{
		ChunkDurations.Add(ChunkDurationSeconds);
		// First chunk has no delay, subsequent chunks use the specified interval
		Delays.Add(i == 0 ? 0.0f : IntervalSeconds);
	}

	float TotalPlannedDuration = NumChunks * ChunkDurationSeconds;
	UE_LOG(ConvaiAudioChunkTestLog, Log, TEXT("CreateAudioChunkTestProxyWithInterval: Audio duration=%.2fs, ChunkDuration=%.3fs, Interval=%.3fs, NumChunks=%d, TotalPlannedDuration=%.2fs"),
		AudioDuration, ChunkDurationSeconds, IntervalSeconds, NumChunks, TotalPlannedDuration);

	return CreateAudioChunkTestProxy(WorldContextObject, AudioStreamer, AudioToPlay, ChunkDurations, Delays);
}

UConvaiAudioChunkTestProxy* UConvaiAudioChunkTestProxy::CreateAudioChunkTestProxy(
	UObject* WorldContextObject,
	UConvaiAudioStreamer* AudioStreamer,
	USoundWave* AudioToPlay,
	const TArray<float>& ChunkDurations,
	const TArray<float>& DelaysBetweenChunks)
{
	UConvaiAudioChunkTestProxy* Proxy = NewObject<UConvaiAudioChunkTestProxy>();
	Proxy->AudioStreamerPtr = AudioStreamer;
	Proxy->SourceAudio = AudioToPlay;
	Proxy->ChunkDurationsSeconds = ChunkDurations;
	Proxy->DelaysSeconds = DelaysBetweenChunks;
	Proxy->CurrentChunkIndex = 0;
	Proxy->CurrentByteOffset = 0;

	if (WorldContextObject)
	{
		Proxy->WorldPtr = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	}

	return Proxy;
}

void UConvaiAudioChunkTestProxy::Activate()
{
	// Validate inputs
	if (!AudioStreamerPtr.IsValid())
	{
		Fail(TEXT("AudioStreamer is null or invalid"));
		return;
	}

	if (!SourceAudio)
	{
		Fail(TEXT("SourceAudio is null"));
		return;
	}

	if (ChunkDurationsSeconds.Num() == 0)
	{
		Fail(TEXT("ChunkDurations array is empty"));
		return;
	}

	// Extract PCM data from sound wave
	FullPCMData = UConvaiUtils::ExtractPCMDataFromSoundWave(SourceAudio, SampleRate, NumChannels);
	
	if (FullPCMData.Num() == 0)
	{
		Fail(TEXT("Failed to extract PCM data from SoundWave"));
		return;
	}

	// Calculate bytes per second (16-bit samples = 2 bytes per sample)
	BytesPerSecond = SampleRate * NumChannels * 2;

	UE_LOG(ConvaiAudioChunkTestLog, Log, TEXT("Starting chunked audio test: %d bytes total, %d chunks, SampleRate=%d, Channels=%d"),
		FullPCMData.Num(), ChunkDurationsSeconds.Num(), SampleRate, NumChannels);

	// Start sending chunks
	ScheduleNextChunk();
}

void UConvaiAudioChunkTestProxy::ScheduleNextChunk()
{
	// Check if we've sent all planned chunks
	if (CurrentChunkIndex >= ChunkDurationsSeconds.Num())
	{
		// Send any remaining audio as final chunk
		int32 RemainingBytes = FullPCMData.Num() - CurrentByteOffset;
		if (RemainingBytes > 0)
		{
			UE_LOG(ConvaiAudioChunkTestLog, Log, TEXT("Sending remaining audio: %d bytes (%.3fs)"),
				RemainingBytes, (float)RemainingBytes / BytesPerSecond);

			USoundWave* FinalChunkWave = UConvaiUtils::PCMDataToSoundWav(
				TArray<uint8>(FullPCMData.GetData() + CurrentByteOffset, RemainingBytes),
				NumChannels,
				SampleRate);

			if (FinalChunkWave && AudioStreamerPtr.IsValid())
			{
				AudioStreamerPtr->ForcePlayVoice(FinalChunkWave);
			}
			CurrentByteOffset += RemainingBytes;
		}

		// Mark end of audio on the streamer after a small delay
		if (AudioStreamerPtr.IsValid() && WorldPtr.IsValid())
		{
			TWeakObjectPtr<UConvaiAudioStreamer> WeakStreamer = AudioStreamerPtr;
			WorldPtr->GetTimerManager().SetTimer(
				ChunkTimerHandle,
				[WeakStreamer]()
				{
					if (WeakStreamer.IsValid())
					{
						WeakStreamer->MarkEndOfAudio();
					}
				},
				3.0f,
				false);
		}

		Complete();
		return;
	}

	// Get delay for this chunk (default to 0 if not specified)
	float Delay = 0.0f;
	if (DelaysSeconds.IsValidIndex(CurrentChunkIndex))
	{
		Delay = DelaysSeconds[CurrentChunkIndex];
	}

	if (Delay > 0.0f && WorldPtr.IsValid())
	{
		// Schedule with delay
		WorldPtr->GetTimerManager().SetTimer(
			ChunkTimerHandle,
			this,
			&UConvaiAudioChunkTestProxy::SendNextChunk,
			Delay,
			false);
	}
	else
	{
		// Send immediately
		SendNextChunk();
	}
}

void UConvaiAudioChunkTestProxy::SendNextChunk()
{
	if (!AudioStreamerPtr.IsValid())
	{
		Fail(TEXT("AudioStreamer became invalid during test"));
		return;
	}

	if (CurrentChunkIndex >= ChunkDurationsSeconds.Num())
	{
		Complete();
		return;
	}

	float ChunkDuration = ChunkDurationsSeconds[CurrentChunkIndex];
	int32 ChunkBytes = FMath::RoundToInt(ChunkDuration * BytesPerSecond);
	
	// Align to frame boundary (2 bytes per sample * NumChannels)
	int32 FrameSize = 2 * NumChannels;
	ChunkBytes = (ChunkBytes / FrameSize) * FrameSize;

	// Clamp to remaining data
	int32 RemainingBytes = FullPCMData.Num() - CurrentByteOffset;
	ChunkBytes = FMath::Min(ChunkBytes, RemainingBytes);

	if (ChunkBytes <= 0)
	{
		UE_LOG(ConvaiAudioChunkTestLog, Warning, TEXT("No more audio data to send at chunk %d"), CurrentChunkIndex);
		Complete();
		return;
	}

	float ActualChunkDuration = (float)ChunkBytes / BytesPerSecond;
	UE_LOG(ConvaiAudioChunkTestLog, Log, TEXT("Sending chunk %d: requested=%.3fs, actual=%.3fs (%d bytes) at offset %d"),
		CurrentChunkIndex, ChunkDuration, ActualChunkDuration, ChunkBytes, CurrentByteOffset);

	// Create a temporary SoundWave for this chunk
	USoundWave* ChunkWave = UConvaiUtils::PCMDataToSoundWav(
		TArray<uint8>(FullPCMData.GetData() + CurrentByteOffset, ChunkBytes),
		NumChannels,
		SampleRate);

	if (ChunkWave)
	{
		AudioStreamerPtr->ForcePlayVoice(ChunkWave);
	}

	// Move to next chunk
	CurrentByteOffset += ChunkBytes;
	CurrentChunkIndex++;

	// Schedule next chunk
	ScheduleNextChunk();
}

void UConvaiAudioChunkTestProxy::Complete()
{
	float TotalDurationSent = (float)CurrentByteOffset / BytesPerSecond;
	float SourceDuration = (float)FullPCMData.Num() / BytesPerSecond;
	UE_LOG(ConvaiAudioChunkTestLog, Log, TEXT("Chunked audio test completed: sent %d chunks, %d bytes total (%.3fs sent vs %.3fs source)"),
		CurrentChunkIndex, CurrentByteOffset, TotalDurationSent, SourceDuration);
	OnComplete.Broadcast();
}

void UConvaiAudioChunkTestProxy::Fail(const FString& ErrorMessage)
{
	UE_LOG(ConvaiAudioChunkTestLog, Error, TEXT("Chunked audio test failed: %s"), *ErrorMessage);
	OnFailed.Broadcast(ErrorMessage);
}

