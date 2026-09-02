// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SynthComponent.h"
#include "Interface/ConvaiAudioCaptureInterface.h"
#include "ConvaiVirtualMicComponent.generated.h"

class FConvaiInjectedEcho;

/**
 * Issue 04 — the Virtual Mic.
 *
 * A test-supplied stand-in for the player's microphone: pre-recorded audio
 * delivered at the same seam and cadence real capture uses, so resampling,
 * mute, streaming state and Talk Target fan-out all run unchanged.
 *
 * Departure from the PRD, which specifies deriving from
 * UConvaiAudioCaptureComponent. That class is UCLASS() with no CONVAI_API, so
 * it cannot be subclassed from another module. Implementing
 * IConvaiAudioCaptureInterface on a plain USynthComponent reaches the same seam
 * — UConvaiPlayerComponent::FindFirstAudioCaptureComponent discovers by
 * interface, not by type (ConvaiPlayerComponent.cpp:467-473) — and is closer to
 * what ADR-0005 describes as plugging in the way a third party would, since a
 * third party could not subclass that component either.
 */
UCLASS(ClassGroup = Synth, meta = (BlueprintSpawnableComponent))
class CONVAITESTS_API UConvaiVirtualMicComponent : public USynthComponent,
                                                   public IConvaiAudioCaptureInterface
{
    GENERATED_BODY()

public:
    UConvaiVirtualMicComponent(const FObjectInitializer& ObjectInitializer);

    //~ Begin IConvaiAudioCaptureInterface
    virtual void Start() override;
    virtual void Stop() override;
    virtual void SetVolumeMultiplier(float InVolumeMultiplier) override;
    //~ End IConvaiAudioCaptureInterface

    /** Queue mono 16 kHz PCM to be emitted at real-time pace. Thread-safe. */
    void EnqueueSamples(const TArray<int16>& Samples);

    /** Queue `Seconds` of silence, so a scenario can script a pause without
     *  stopping the stream — a stopped stream is a different code path. */
    void EnqueueSilence(float Seconds);

    /** Queue a sine tone. Used by issue 03's F7 regression guard: a tone the
     *  microphone emits must not appear in Reference Audio. */
    void EnqueueTone(float Frequency, float Seconds, float Amplitude = 0.5f);

    /** Samples still waiting to be emitted. Zero means the scripted audio has
     *  been fully delivered to the plugin's capture path. */
    int32 PendingSamples() const;

    /** How many samples OnGenerateAudio has emitted since Start(). A scenario
     *  asserts on this rather than on wall-clock time, because the audio thread
     *  sets the pace and a wall-clock assertion would measure the scheduler. */
    int64 EmittedSamples() const;

    /** RMS and peak of what this mic actually put on the wire, scripted audio
     *  and Injected Echo together.
     *
     *  The ground truth for every microphone-side measurement, and it exists
     *  because there turned out to be a gap: the canceller reported a busy
     *  microphone on a run where Injected Echo had delivered a peak of 9e-06.
     *  Whatever the plugin captured on that run was not what this component
     *  emitted, and no metric on either side of that gap could see it. */
    double EmittedRms() const;
    float EmittedPeak() const;

    /** Cumulative sum of squared emitted samples, so a scenario can difference
     *  two snapshots and get the level over its own window rather than over the
     *  whole run. */
    double EmittedEnergy() const;

    bool IsRunning() const { return bRunning; }

    /** The submix this mic renders into. Null means it is falling through to
     *  the master mix, which makes it a loudspeaker rather than a microphone —
     *  scenarios asserting on routing check this first. */
    USoundSubmixBase* GetSubmix() const { return SoundSubmix; }

    /** The rate scripted audio has to arrive at. Exposed so the step library
     *  resamples fixtures to it rather than assuming a number. */
    static constexpr int32 CaptureSampleRate() { return kSampleRate; }

    /** Mixes Injected Echo into whatever this mic is emitting, scripted audio
     *  or silence alike — a real microphone hears the room whether or not the
     *  player is talking. Null detaches. */
    void SetEchoSource(TSharedPtr<FConvaiInjectedEcho, ESPMode::ThreadSafe> InEchoSource);

protected:
    //~ Begin USynthComponent
    virtual bool Init(int32& SampleRate) override;
    virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override;
    //~ End USynthComponent

private:
    // The plugin captures at 16 kHz mono; matching it means the scripted audio
    // travels the same resample path real microphone audio does.
    static constexpr int32 kSampleRate = 16000;

    mutable FCriticalSection QueueLock;
    TArray<int16> Queue;
    TSharedPtr<FConvaiInjectedEcho, ESPMode::ThreadSafe> EchoSource;
    int64 Emitted = 0;
    double EmittedEnergy_ = 0.0;
    float EmittedPeakAbs = 0.0f;

    FThreadSafeBool bRunning = false;
    float VolumeMultiplier = 1.0f;
};
