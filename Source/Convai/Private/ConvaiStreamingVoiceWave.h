// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include <atomic>
#include "ConvaiStreamingVoiceWave.generated.h"

class UConvaiAudioStreamer;

/**
 * The voice wave a streamer creates for itself. Once an audio device renders it, the render thread
 * pulls the streamer's AudioRingBuffer directly, so a game-thread hitch cannot starve the voice.
 * USoundWaveProcedural's queue allows one consumer at a time, so the render thread's pull, the
 * tick's push and a stop's flush take the same lock. The render thread only tries it, since the
 * push can reach Play(); it waits for SourceLock, which the game thread holds only briefly.
 */
UCLASS()
class UConvaiStreamingVoiceWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	/** Any thread: an audio mixer source renders the wave, so the render thread pulls it. False
	 *  once the sound is stopped or evicted, and when it never started (a world that does not
	 *  allow audio playback, for one). A paused source still renders. */
	bool IsRendering() const { return ActiveSources.load() > 0; }

	// The mixer calls these around every source that renders the wave: begin on the render thread
	// before its first pull, end once the source is freed after its last one.
	virtual void OnBeginGenerate() override { ++ActiveSources; }
	virtual void OnEndGenerate() override { --ActiveSources; }

	/** Any thread: drops everything queued and what DropSource drops from the streamer's rings, ends
	 *  the reply being pulled or pushed and restarts the pulled count, all before it returns and
	 *  under one lock, so whatever a caller does next applies to what follows. */
	void Flush(TFunctionRef<void()> DropSource);
	/** QueueAudio, serialized with Flush. Marks a pushed reply as in progress until the next Flush. */
	void QueueVoice(const uint8* AudioData, int32 BufferSize);
	/** Game thread: runs Step with no Flush in between, so audio Step takes from the rings and queues
	 *  is either dropped whole by a stop or not taken before it. */
	void HoldFlush(TFunctionRef<void()> Step);
	/** Bumped by every Flush. */
	uint32 GetFlushGeneration() const { return FlushGeneration.load(); }

	/** Game thread: the streamer whose ring the render thread pulls. Detach before it is destroyed. */
	void AttachSource(UConvaiAudioStreamer* Streamer);
	void DetachSource() { AttachSource(nullptr); }

	/** Any thread: the next reply's first pull may start below MinBufferDuration, unless a Flush
	 *  came after Generation: the force then belonged to the reply it cut. */
	void RequestForcedStart(uint32 Generation);
	/** Game thread: a reply the tick began pushing continues through the pull without a second
	 *  start cushion, unless a Flush has ended it. */
	void ContinuePushedReply();
	bool IsReplyOpen() const { return bReplyOpen; }
	/** A reply is being pulled, or the tick has pushed one, since the last Flush. */
	bool IsReplyInProgress() const { return bReplyOpen || bPushedReply; }
	/** FPlatformTime::Seconds() when the current reply's first bytes were pulled. */
	double GetReplyOpenSeconds() const { return ReplyOpenSeconds; }
	/** Game thread: ends the reply unless more audio has arrived, so the next one waits for the
	 *  start cushion again. Returns whether it ended. */
	bool CloseReplyIfDrained();

	/** Bytes pulled since the last flush. */
	uint64 GetPulledBytes() const { return PulledBytes.load(); }
	/** Game thread: runs Inspect while no pull can move the ring, with GetPulledBytes(). The render
	 *  thread waits for it, so Inspect only copies what it needs. */
	void HoldPulls(TFunctionRef<void(uint64 PulledSoFar)> Inspect);

	virtual int32 GeneratePCMData(uint8* PCMData, const int32 SamplesNeeded) override;
	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;

private:
	FCriticalSection SourceLock;
	UConvaiAudioStreamer* Source = nullptr;

	FCriticalSection QueueLock;
	/** Under QueueLock: Flush runs Super's render callback only to apply its deferred reset. */
	bool bFlushing = false;
	/** Set under QueueLock by QueueVoice, cleared under it by Flush: a stop always ends a pushed reply. */
	std::atomic<bool> bPushedReply{false};
	/** Written under QueueLock. */
	std::atomic<uint32> FlushGeneration{0};

	/** What the engine renders on an underrun, for a render callback that would otherwise wait. */
	int32 RenderUnderrun(uint8* PCMData);

	// A count, not a flag: a restarted sound's new source can begin before the old one ends.
	std::atomic<int32> ActiveSources{0};
	std::atomic<bool> bReplyOpen{false};
	std::atomic<bool> bForceStart{false};
	std::atomic<double> ReplyOpenSeconds{0.0};
	std::atomic<uint64> PulledBytes{0};
};
