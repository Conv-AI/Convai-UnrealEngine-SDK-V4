// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiStreamingVoiceWave.h"
#include "ConvaiAudioStreamer.h"
#include "ConvaiUtils.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeTryLock.h"
#include "Utility/ConvaiEngineCompat.h"

void UConvaiStreamingVoiceWave::Flush(TFunctionRef<void()> DropSource)
{
	FScopeLock Lock(&QueueLock);
	// Under the lock the pull and the tick's push take, so neither moves audio out of the rings
	// between their reset and the end of the reply.
	DropSource();
	ResetAudio();
	// ResetAudio leaves AudioBuffer and AvailableByteCount to the next render callback. Running
	// that callback here, with the pull suppressed, applies them now, so audio queued after this
	// stop is neither dropped by it nor left out of the count.
	bFlushing = true;
	uint8 Discarded[512 * sizeof(float)];
	Super::GeneratePCMData(Discarded, 512);
	bFlushing = false;
	bReplyOpen = false;
	bPushedReply = false;
	bForceStart = false;
	PulledBytes = 0;
	++FlushGeneration;
}

void UConvaiStreamingVoiceWave::QueueVoice(const uint8* AudioData, int32 BufferSize)
{
	FScopeLock Lock(&QueueLock);
	QueueAudio(AudioData, BufferSize);
	bPushedReply = true;
}

void UConvaiStreamingVoiceWave::HoldFlush(TFunctionRef<void()> Step)
{
	FScopeLock Lock(&QueueLock);
	Step();
}

void UConvaiStreamingVoiceWave::AttachSource(UConvaiAudioStreamer* Streamer)
{
	FScopeLock Lock(&SourceLock);
	Source = Streamer;
}

void UConvaiStreamingVoiceWave::RequestForcedStart(uint32 Generation)
{
	FScopeLock Lock(&QueueLock);
	if (FlushGeneration.load() == Generation)
	{
		bForceStart = true;
	}
}

void UConvaiStreamingVoiceWave::ContinuePushedReply()
{
	if (bReplyOpen)
	{
		return;
	}
	// Tested and opened under Flush's lock, so a stop that lands meanwhile keeps the reply closed.
	FScopeLock Lock(&QueueLock);
	if (bPushedReply && !bReplyOpen)
	{
		ReplyOpenSeconds = FPlatformTime::Seconds();
		bReplyOpen = true;
	}
}

bool UConvaiStreamingVoiceWave::CloseReplyIfDrained()
{
	FScopeLock Lock(&SourceLock);
	if (Source && !Source->AudioRingBuffer.IsEmpty())
	{
		return false;
	}
	bReplyOpen = false;
	// Nothing is left to force; a force latched now would skip a later reply's cushion.
	bForceStart = false;
	return true;
}

void UConvaiStreamingVoiceWave::HoldPulls(TFunctionRef<void(uint64 PulledSoFar)> Inspect)
{
	FScopeLock Lock(&SourceLock);
	Inspect(GetPulledBytes());
}

int32 UConvaiStreamingVoiceWave::RenderUnderrun(uint8* PCMData)
{
	const int32 Bytes = NumBufferUnderrunSamples * SampleByteSize;
	FMemory::Memzero(PCMData, Bytes);
	return Bytes;
}

int32 UConvaiStreamingVoiceWave::GeneratePCMData(uint8* PCMData, const int32 SamplesNeeded)
{
	// The mixer's render thread calls this, and waiting here for a stop or the tick's push would
	// stall every source it mixes. A busy lock renders an underrun and leaves the queue as it is.
	FScopeTryLock Lock(&QueueLock);
	if (!Lock.IsLocked())
	{
		return RenderUnderrun(PCMData);
	}
	return Super::GeneratePCMData(PCMData, SamplesNeeded);
}

int32 UConvaiStreamingVoiceWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	// Audio a caller queued with PlayVoiceData plays first, in the order it was given.
	if (GetAvailableAudioByteCount() > 0 || bFlushing)
	{
		return 0;
	}

	// Waited for: the game thread holds this only for short copies and never waits on the render
	// thread under it, while giving up here would render a gap mid-word.
	FScopeLock Lock(&SourceLock);
	if (!Source)
	{
		return 0;
	}
	FAudioRingBuffer& Ring = Source->AudioRingBuffer;
	uint32 RingSampleRate = 0;
	uint32 RingChannels = 0;
	Ring.GetFormat(RingSampleRate, RingChannels);
	// Until the game thread configures the wave for this format, its bytes would play at the wrong rate.
	if (RingSampleRate == 0 || RingSampleRate != static_cast<uint32>(GetSampleRateForCurrentPlatform())
		|| RingChannels != static_cast<uint32>(NumChannels))
	{
		return 0;
	}

	if (!bReplyOpen)
	{
		const double Buffered = UConvaiUtils::CalculateAudioDuration(Ring.GetAvailableBytes(), RingChannels, RingSampleRate, 2);
		if (Buffered <= 0.0 || (Buffered < Source->MinBufferDuration && !bForceStart))
		{
			return 0;
		}
		bForceStart = false;
		ReplyOpenSeconds = FPlatformTime::Seconds();
		bReplyOpen = true;
	}

	// AudioBuffer keeps its capacity between callbacks, so this allocates only on the first pull.
	const int32 WantedBytes = NumSamples * static_cast<int32>(sizeof(int16));
	OutAudio.SetNumUninitialized(WantedBytes, CONVAI_ALLOW_SHRINKING_NO);
	const uint32 BytesRead = Ring.Dequeue(OutAudio.GetData(), WantedBytes);
	OutAudio.SetNum(static_cast<int32>(BytesRead), CONVAI_ALLOW_SHRINKING_NO);
	PulledBytes += BytesRead;
	return static_cast<int32>(BytesRead / sizeof(int16));
}
