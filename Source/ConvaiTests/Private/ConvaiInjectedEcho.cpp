// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiInjectedEcho.h"

namespace
{
    // Anything below this is the mixer idling rather than the game playing.
    // Matched to the engine's own silence test (SMALL_NUMBER, AudioMixerSubmix.cpp:250)
    // so "the submix rendered nothing" means the same thing on both sides.
    constexpr float kSilenceFloor = SMALL_NUMBER;

    float SoftClip(float Sample)
    {
        // Bounded, smooth, and monotonic, which is all a stand-in for speaker
        // nonlinearity has to be. tanh would do the same job for more cycles on
        // the audio thread.
        return Sample / (1.0f + FMath::Abs(Sample));
    }
}

FConvaiInjectedEcho::FConvaiInjectedEcho(const FParams& InParams)
    : Params(InParams)
{
}

const FString& FConvaiInjectedEcho::GetListenerName() const
{
    static const FString Name = TEXT("ConvaiInjectedEcho");
    return Name;
}

void FConvaiInjectedEcho::OnNewSubmixBuffer(const USoundSubmix*, float* AudioData, int32 NumSamples,
                                            int32 InNumChannels, const int32 InSampleRate, double)
{
    if (!AudioData || NumSamples <= 0 || InNumChannels <= 0 || InSampleRate <= 0 ||
        Params.TargetSampleRate <= 0)
    {
        return;
    }

    FScopeLock ScopeLock(&Lock);

    if (!bDelayPrimed)
    {
        // The delay is the buffer starting full of silence rather than a read
        // cursor: the microphone drains this from the front, so silence at the
        // front is exactly a delay, and there is no wrap-around to get wrong.
        bDelayPrimed = true;
        const int32 DelaySamples =
            FMath::Max(0, FMath::RoundToInt(Params.DelayMs * 0.001f * Params.TargetSampleRate));
        Pending.AddZeroed(DelaySamples);
    }

    const double Ratio = static_cast<double>(InSampleRate) / Params.TargetSampleRate;

    for (int32 i = 0; i + InNumChannels - 1 < NumSamples; i += InNumChannels)
    {
        float Mono = 0.0f;
        for (int32 c = 0; c < InNumChannels; ++c)
        {
            Mono += AudioData[i + c];
        }
        Mono /= InNumChannels;

        if (FMath::Abs(Mono) > kSilenceFloor)
        {
            ++NonSilentSource;
        }

        // Box-average decimation. The averaging window is the low-pass, which
        // an every-Nth-sample pick would not be, and it handles a non-integer
        // ratio without a resampler -- the master submix is 48 kHz and the mic
        // is 16 kHz today, but neither is guaranteed.
        Accumulator += Mono;
        ++AccumulatedCount;
        DecimationPhase += 1.0;
        if (DecimationPhase < Ratio)
        {
            continue;
        }
        DecimationPhase -= Ratio;

        float Sample = static_cast<float>(Accumulator / AccumulatedCount) * Params.Gain;
        Accumulator = 0.0;
        AccumulatedCount = 0;

        if (Params.bSoftClip)
        {
            Sample = SoftClip(Sample);
        }
        if (Params.NoiseAmplitude > 0.0f)
        {
            // Fixed-seed LCG rather than a std:: distribution, whose
            // consumption is unspecified and would make runs incomparable.
            NoiseState = NoiseState * 1664525u + 1013904223u;
            const float Uniform = (static_cast<float>(NoiseState >> 8) / 16777216.0f) * 2.0f - 1.0f;
            Sample += Uniform * Params.NoiseAmplitude;
        }

        Pending.Add(Sample);
    }
}

void FConvaiInjectedEcho::MixInto(float* Out, int32 NumSamples)
{
    if (!Out || NumSamples <= 0)
    {
        return;
    }

    FScopeLock ScopeLock(&Lock);
    const int32 Available = FMath::Min(NumSamples, Pending.Num());
    for (int32 i = 0; i < Available; ++i)
    {
        Out[i] += Pending[i];
        Peak = FMath::Max(Peak, FMath::Abs(Pending[i]));
    }
    if (Available > 0)
    {
        Injected += Available;
        Pending.RemoveAt(0, Available, EAllowShrinking::No);
    }
}

int64 FConvaiInjectedEcho::NonSilentSourceSamples() const
{
    FScopeLock ScopeLock(&Lock);
    return NonSilentSource;
}

int64 FConvaiInjectedEcho::InjectedSamples() const
{
    FScopeLock ScopeLock(&Lock);
    return Injected;
}

float FConvaiInjectedEcho::PeakInjected() const
{
    FScopeLock ScopeLock(&Lock);
    return Peak;
}
