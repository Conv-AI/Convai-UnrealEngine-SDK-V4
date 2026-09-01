// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISubmixBufferListener.h"

/**
 * Issue 05 — Injected Echo.
 *
 * A delayed, attenuated copy of the game's own speaker output mixed into the
 * Virtual Mic feed, standing in for the acoustic path a real room would
 * provide. Without it there is nothing for echo cancellation to cancel, so it
 * is what makes cancellation observable at all.
 *
 * Deliberately a *second*, independent tap on the master submix rather than a
 * share of the one the reference thread uses. Two taps mean a broken reference
 * path leaves the echo still flowing, so the microphone stays dirty and the
 * test fails loudly; one shared tap would silence both together and read as a
 * pass. F15 and F17 both say that path breaks in practice, so this is not
 * hypothetical.
 *
 * Lives on the audio render thread at both ends -- the submix callback fills it
 * and the synth's OnGenerateAudio drains it -- so everything crossing between
 * them is under one lock over a small buffer.
 */
class CONVAITESTS_API FConvaiInjectedEcho : public ISubmixBufferListener
{
public:
    struct FParams
    {
        /** The acoustic path's delay. The DLL repo's ERLE sweep uses 0, 40, 120
         *  and 300 ms; 120 is a room. */
        float DelayMs = 120.0f;

        /** Linear. F12 measured the canceller's behaviour across 0 to 0.5, and
         *  found it worst at 0 -- a headset that leaks nothing. */
        float Gain = 0.5f;

        /** Speaker nonlinearity. Off by default so a failure is not attributed
         *  to it before it has been shown to matter. */
        bool bSoftClip = false;

        /** Additive noise amplitude, 0 to 1. Deterministic, so runs compare. */
        float NoiseAmplitude = 0.0f;

        /** The rate the Virtual Mic renders at; the tap decimates to it. */
        int32 TargetSampleRate = 16000;
    };

    explicit FConvaiInjectedEcho(const FParams& InParams);

    //~ Begin ISubmixBufferListener
    virtual void OnNewSubmixBuffer(const USoundSubmix*, float* AudioData, int32 NumSamples,
                                   int32 InNumChannels, const int32 InSampleRate, double) override;
    virtual const FString& GetListenerName() const override;
    //~ End ISubmixBufferListener

    /** Adds up to NumSamples of echo into Out, consuming them. Short is
     *  silence: the far end not playing is not an error. */
    void MixInto(float* Out, int32 NumSamples);

    /** Samples the tap saw above the silence floor, before gain. The fixture
     *  self-check: zero means the speakers were silent, and "cancellation
     *  worked" and "there was nothing to cancel" are the same measurement. */
    int64 NonSilentSourceSamples() const;

    /** Echo samples handed to the microphone. Zero with a non-zero source count
     *  means the delay swallowed the whole run. */
    int64 InjectedSamples() const;

    /** Peak absolute echo amplitude actually injected, after gain. */
    float PeakInjected() const;

private:
    const FParams Params;

    mutable FCriticalSection Lock;
    TArray<float> Pending;
    double DecimationPhase = 0.0;
    double Accumulator = 0.0;
    int32 AccumulatedCount = 0;
    uint32 NoiseState = 0x1234567u;
    int64 NonSilentSource = 0;
    int64 Injected = 0;
    float Peak = 0.0f;
    bool bDelayPrimed = false;
};
