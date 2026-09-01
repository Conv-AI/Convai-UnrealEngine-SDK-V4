// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiVirtualMicComponent.h"

#include "ConvaiInjectedEcho.h"
#include "ConvaiTests.h"

#include "Sound/SoundSubmix.h"
#include "UObject/ConstructorHelpers.h"

UConvaiVirtualMicComponent::UConvaiVirtualMicComponent(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryComponentTick.bCanEverTick = false;
    bAutoActivate = true;
    NumChannels = 1;

    // The Virtual Mic has to route itself. UConvaiPlayerComponent assigns
    // /ConvAI/Submixes/AudioInput only to the UConvaiAudioCaptureComponent it
    // creates in OnComponentCreated (ConvaiPlayerComponent.cpp:128) — an
    // adopted third-party component never receives it. Left unassigned, a synth
    // renders into the default chain and straight into the master mix, which
    // means audibly into Reference Audio; F6 records that exact mechanism.
    //
    // Without this the Virtual Mic is not a stand-in for the microphone, it is
    // a loudspeaker, and every routing assertion built on it would be measuring
    // the test's own miswiring.
    static ConstructorHelpers::FObjectFinder<USoundSubmixBase> AudioInputSubmix(
        TEXT("/ConvAI/Submixes/AudioInput.AudioInput"));
    if (AudioInputSubmix.Succeeded())
    {
        SoundSubmix = AudioInputSubmix.Object;
    }
}

bool UConvaiVirtualMicComponent::Init(int32& SampleRate)
{
    SampleRate = kSampleRate;
    NumChannels = 1;
    return true;
}

void UConvaiVirtualMicComponent::Start()
{
    if (bRunning)
    {
        return;
    }
    bRunning = true;
    Super::Start();
    UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS virtual_mic=started"));
}

void UConvaiVirtualMicComponent::Stop()
{
    if (!bRunning)
    {
        return;
    }
    bRunning = false;
    Super::Stop();
    UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS virtual_mic=stopped"));
}

void UConvaiVirtualMicComponent::SetVolumeMultiplier(float InVolumeMultiplier)
{
    VolumeMultiplier = InVolumeMultiplier;
}

void UConvaiVirtualMicComponent::EnqueueSamples(const TArray<int16>& Samples)
{
    FScopeLock ScopeLock(&QueueLock);
    Queue.Append(Samples);
}

void UConvaiVirtualMicComponent::EnqueueSilence(float Seconds)
{
    const int32 Count = FMath::Max(0, FMath::RoundToInt(Seconds * kSampleRate));
    FScopeLock ScopeLock(&QueueLock);
    Queue.AddZeroed(Count);
}

void UConvaiVirtualMicComponent::EnqueueTone(float Frequency, float Seconds, float Amplitude)
{
    const int32 Count = FMath::Max(0, FMath::RoundToInt(Seconds * kSampleRate));
    TArray<int16> Tone;
    Tone.Reserve(Count);
    for (int32 i = 0; i < Count; ++i)
    {
        const double Phase = 2.0 * PI * Frequency * i / kSampleRate;
        Tone.Add(static_cast<int16>(FMath::Clamp(FMath::Sin(Phase) * Amplitude, -1.0, 1.0) * 32767.0));
    }
    EnqueueSamples(Tone);
}

void UConvaiVirtualMicComponent::SetEchoSource(
    TSharedPtr<FConvaiInjectedEcho, ESPMode::ThreadSafe> InEchoSource)
{
    FScopeLock ScopeLock(&QueueLock);
    EchoSource = MoveTemp(InEchoSource);
}

int32 UConvaiVirtualMicComponent::PendingSamples() const
{
    FScopeLock ScopeLock(&QueueLock);
    return Queue.Num();
}

int64 UConvaiVirtualMicComponent::EmittedSamples() const
{
    FScopeLock ScopeLock(&QueueLock);
    return Emitted;
}

double UConvaiVirtualMicComponent::EmittedRms() const
{
    FScopeLock ScopeLock(&QueueLock);
    return Emitted > 0 ? FMath::Sqrt(EmittedEnergy_ / static_cast<double>(Emitted)) : 0.0;
}

float UConvaiVirtualMicComponent::EmittedPeak() const
{
    FScopeLock ScopeLock(&QueueLock);
    return EmittedPeakAbs;
}

double UConvaiVirtualMicComponent::EmittedEnergy() const
{
    FScopeLock ScopeLock(&QueueLock);
    return EmittedEnergy_;
}

int32 UConvaiVirtualMicComponent::OnGenerateAudio(float* OutAudio, int32 NumSamples)
{
    FScopeLock ScopeLock(&QueueLock);

    const int32 Available = FMath::Min(NumSamples, Queue.Num());
    for (int32 i = 0; i < Available; ++i)
    {
        OutAudio[i] = (Queue[i] / 32768.0f) * VolumeMultiplier;
    }

    // Underrun is silence, not a short buffer. Returning fewer samples than
    // asked for makes the synth chain treat the stream as ended, which is a
    // different code path from a quiet microphone and would stop the capture
    // the scenario is measuring.
    for (int32 i = Available; i < NumSamples; ++i)
    {
        OutAudio[i] = 0.0f;
    }

    if (Available > 0)
    {
        Queue.RemoveAt(0, Available, EAllowShrinking::No);
    }

    // After the scripted audio, not instead of it: a microphone in a room hears
    // the speakers whether or not the player is talking, and the double-talk
    // case is exactly the two overlapping.
    if (EchoSource.IsValid())
    {
        EchoSource->MixInto(OutAudio, NumSamples);
    }

    // Measured after the echo is mixed in, so this is the whole microphone
    // signal and can be compared directly against what the canceller reports
    // arriving. The two disagreeing is the only way to catch the plugin
    // capturing something other than this component.
    for (int32 i = 0; i < NumSamples; ++i)
    {
        const double Sample = OutAudio[i];
        EmittedEnergy_ += Sample * Sample;
        EmittedPeakAbs = FMath::Max(EmittedPeakAbs, FMath::Abs(OutAudio[i]));
    }

    Emitted += NumSamples;
    return NumSamples;
}
