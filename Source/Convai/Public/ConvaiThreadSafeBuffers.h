// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RingBuffer.h"
#include "ConvaiDefinitions.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiThreadSafeBuffersLog, Log, All);

// ============================================================================
// Thread-Safe Audio Ring Buffer
// Thread-safe ring buffer for audio data (transport thread → game thread)
// Data and format operations are thread-safe, duration is not (game thread only)
// ============================================================================
struct FAudioRingBuffer
{
	static constexpr uint32 BufferCapacity = 1024 * 1024 * 4; // 4MB

	TRingBuffer<uint8> Data;
	uint32 SampleRate;
	uint32 NumChannels;
	double DurationSeconds;
	mutable FCriticalSection DataMutex;

	// Storage is allocated by the first Enqueue, so class defaults, templates and streamers that
	// never play audio hold none.
	FAudioRingBuffer() : SampleRate(0), NumChannels(0), DurationSeconds(0.0) {}

	// Format operations (THREAD-SAFE)
	inline void SetFormat(uint32 InSampleRate, uint32 InNumChannels)
	{
		FScopeLock Lock(&DataMutex);
		SampleRate = InSampleRate;
		NumChannels = InNumChannels;
	}

	inline uint32 GetSampleRate() const
	{
		FScopeLock Lock(&DataMutex);
		return SampleRate;
	}

	inline uint32 GetNumChannels() const
	{
		FScopeLock Lock(&DataMutex);
		return NumChannels;
	}

	inline void GetFormat(uint32& OutSampleRate, uint32& OutNumChannels) const
	{
		FScopeLock Lock(&DataMutex);
		OutSampleRate = SampleRate;
		OutNumChannels = NumChannels;
	}

	// Duration (no thread safety - accessed from game thread only)
	inline void SetTotalDuration(double Seconds)
	{
		FScopeLock Lock(&DataMutex);
		DurationSeconds = Seconds;
	}

	inline void AppendToTotalDuration(double Seconds)
	{
		FScopeLock Lock(&DataMutex);
		DurationSeconds += Seconds;
	}

	inline double GetTotalDuration() const
	{
		FScopeLock Lock(&DataMutex);
		return DurationSeconds;
	}

	// Data operations (THREAD-SAFE)
	inline bool Enqueue(const uint8* AudioData, uint32 Size)
	{
		if (!AudioData || Size == 0)
		{
			return false;
		}

		FScopeLock Lock(&DataMutex);
		const uint32 Needed = Data.RingDataUsage() + Size;
		if (Needed > BufferCapacity)
		{
			return false;
		}
		// Grown here, so TRingBuffer's own doubling never runs past the capacity.
		if (Needed > Data.RingDataSize())
		{
			constexpr uint32 FirstGrowthBytes = 1024 * 1024;
			Data.Resize(FMath::Min(FMath::Max3(Data.RingDataSize() * 2, Needed, FirstGrowthBytes), BufferCapacity));
		}
		Data.Enqueue(AudioData, Size);
		return true;
	}

	inline uint32 Dequeue(uint8* OutData, uint32 MaxSize)
	{
		FScopeLock Lock(&DataMutex);
		return Data.Dequeue(OutData, MaxSize);
	}

	inline void AppendData(const uint8* AudioData, uint32 Size)
	{
		Enqueue(AudioData, Size);
	}

	inline uint32 GetData(uint8* OutBuffer, uint32 Size) const
	{
		FScopeLock Lock(&DataMutex);
		return Data.Peek(OutBuffer, Size);
	}

	inline void RemoveData(uint32 BytesToRemove)
	{
		FScopeLock Lock(&DataMutex);
		Data.Dequeue(nullptr, FMath::Min(BytesToRemove, Data.RingDataUsage()));
	}

	inline uint32 GetAvailableBytes() const
	{
		FScopeLock Lock(&DataMutex);
		return Data.RingDataUsage();
	}

	inline bool IsEmpty() const
	{
		FScopeLock Lock(&DataMutex);
		return Data.RingDataUsage() == 0;
	}

	inline void Reset()
	{
		FScopeLock Lock(&DataMutex);
		// Drops the contents in O(1): Peek and Dequeue never read past the usage, so the old
		// bytes need no zeroing (a 4 MB memset under the lock, once per stop).
		Data.Dequeue(nullptr, Data.RingDataUsage());
		SampleRate = 0;
		NumChannels = 0;
		DurationSeconds = 0.0;
	}
};

// ============================================================================
// Thread-Safe LipSync Buffer
// Thread-safe accumulator for lipsync frames (transport thread → game thread)
// Only Enqueue/Dequeue are thread-safe, metadata is not
// ============================================================================
struct FLipSyncBuffer
{
	FAnimationSequence Sequence; // Single sequence for accumulation
	int32 FrameRate;
	bool bHasNewData;
	mutable FCriticalSection SequenceMutex; // Only for sequence operations

	FLipSyncBuffer() : FrameRate(0), bHasNewData(false)
	{
		// Pre-allocate space for animation frames (done once)
		Sequence.AnimationFrames.Reserve(1024);
	}

	inline void SetFrameRate(int32 InFrameRate)
	{
		FScopeLock Lock(&SequenceMutex);
		FrameRate = InFrameRate;
	}

	inline int32 GetFrameRate() const
	{
		FScopeLock Lock(&SequenceMutex);
		return FrameRate;
	}

	// Enqueue new sequence - appends frames to single sequence (THREAD-SAFE)
	inline void Enqueue(const FAnimationSequence& InSequence)
	{
		FScopeLock Lock(&SequenceMutex);

		// Append all frames from the new sequence
		Sequence.AnimationFrames.Append(InSequence.AnimationFrames);
		Sequence.Duration += InSequence.Duration;

		// Update frame rate if not set
		if (FrameRate == 0 && InSequence.FrameRate > 0)
		{
			FrameRate = InSequence.FrameRate;
			Sequence.FrameRate = InSequence.FrameRate;
		}

		bHasNewData = true;
	}

	// Dequeue - returns the entire accumulated sequence and clears it (THREAD-SAFE)
	inline bool Dequeue(FAnimationSequence& OutSequence)
	{
		FScopeLock Lock(&SequenceMutex);

		if (!bHasNewData)
		{
			return false;
		}

		OutSequence = MoveTemp(Sequence);

		Sequence.AnimationFrames.Reset();
		Sequence.Duration = 0.0f;
		Sequence.FrameRate = FrameRate;

		bHasNewData = false;
		return true;
	}

	inline bool HasData() const
	{
		FScopeLock Lock(&SequenceMutex);
		return bHasNewData;
	}

	inline void Reset()
	{
		FScopeLock Lock(&SequenceMutex);
		Sequence.AnimationFrames.Reset();
		Sequence.Duration = 0.0f;
		FrameRate = 0;
		bHasNewData = false;
	}
};

