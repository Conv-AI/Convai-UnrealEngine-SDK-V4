// Copyright 2022 Convai Inc. All Rights Reserved.

// Issue 05, first half — does Injected Echo reach the microphone?
//
// The paired AEC run needs a live Connection and a character. This does not, and
// it has to pass before that run means anything: if the echo never arrives, the
// canceller has nothing to cancel and "AEC works" and "the fixture is broken"
// produce the same green tick. The PRD calls that out as the case the
// negative-control rule exists for.
//
// The far end is a second Virtual Mic with no submix assigned, which renders
// into the default chain and therefore into the master mix -- the loudspeaker
// role, measured at -10.1 dB in mic_in_reference_audio_control. The near end is
// a normally routed Virtual Mic that stays silent, so anything appearing in
// /ConvAI/Submixes/AudioInput arrived through Injected Echo and nothing else.
//
// Registered twice. The control runs the identical scenario with gain 0 and
// asserts the tone is *absent*; without it, a tap that silently captured
// nothing would pass the main assertion's mirror image forever.

#include "ConvaiInjectedEcho.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTests.h"
#include "ConvaiVirtualMicComponent.h"

#include "AudioMixerDevice.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ISubmixBufferListener.h"
#include "Sound/SoundSubmix.h"

namespace
{
    constexpr float kFarEndHz = 1000.0f;
    constexpr float kFarEndAmplitude = 0.8f;
    constexpr float kFarEndSeconds = 2.5f;
    constexpr float kEchoGain = 0.5f;
    constexpr float kEchoDelayMs = 120.0f;

    Audio::FMixerDevice* ResolveMixerDevice(UWorld* World)
    {
        FAudioDeviceHandle Handle = World ? World->GetAudioDevice() : FAudioDeviceHandle();
        if (!Handle.IsValid() && GEngine)
        {
            Handle = GEngine->GetMainAudioDevice();
        }
        FAudioDevice* Device = Handle.GetAudioDevice();
        return Device ? static_cast<Audio::FMixerDevice*>(Device) : nullptr;
    }

    // Energy at one frequency by direct correlation, normalised so a pure tone
    // approaches 1. Same instrument as the F7 guard, and used the same way: to
    // separate the far end's tone from whatever else the submix carries, not to
    // judge how loud it is. Level is measured separately, because F18 is what
    // happens when a ratio is asked to answer a level question.
    double ToneEnergyRatio(const TArray<float>& Mono, int32 SampleRate, float Frequency)
    {
        if (Mono.Num() == 0 || SampleRate <= 0)
        {
            return 0.0;
        }
        double Real = 0.0, Imag = 0.0, Total = 0.0;
        for (int32 i = 0; i < Mono.Num(); ++i)
        {
            const double Phase = 2.0 * PI * Frequency * i / SampleRate;
            Real += Mono[i] * FMath::Cos(Phase);
            Imag += Mono[i] * FMath::Sin(Phase);
            Total += static_cast<double>(Mono[i]) * Mono[i];
        }
        if (Total <= 0.0)
        {
            return 0.0;
        }
        return FMath::Clamp(2.0 * (Real * Real + Imag * Imag) / Mono.Num() / Total, 0.0, 1.0);
    }

    double RootMeanSquare(const TArray<float>& Mono)
    {
        if (Mono.Num() == 0)
        {
            return 0.0;
        }
        double Total = 0.0;
        for (const float Sample : Mono)
        {
            Total += static_cast<double>(Sample) * Sample;
        }
        return FMath::Sqrt(Total / Mono.Num());
    }

    class FCaptureTap : public ISubmixBufferListener
    {
    public:
        virtual void OnNewSubmixBuffer(const USoundSubmix*, float* AudioData, int32 NumSamples,
                                       int32 InNumChannels, const int32 InSampleRate,
                                       double) override
        {
            if (!AudioData || NumSamples <= 0 || InNumChannels <= 0)
            {
                return;
            }
            FScopeLock ScopeLock(&Lock);
            SampleRate = InSampleRate;
            for (int32 i = 0; i + InNumChannels - 1 < NumSamples; i += InNumChannels)
            {
                float Sum = 0.0f;
                for (int32 c = 0; c < InNumChannels; ++c)
                {
                    Sum += AudioData[i + c];
                }
                Mono.Add(Sum / InNumChannels);
            }
        }

        virtual const FString& GetListenerName() const override
        {
            static const FString Name = TEXT("ConvaiInjectedEchoCaptureTap");
            return Name;
        }

        void Read(TArray<float>& OutMono, int32& OutSampleRate) const
        {
            FScopeLock ScopeLock(&Lock);
            OutMono = Mono;
            OutSampleRate = SampleRate;
        }

    private:
        mutable FCriticalSection Lock;
        TArray<float> Mono;
        int32 SampleRate = 0;
    };
}

class FConvaiInjectedEchoScenario : public FConvaiTestScenario
{
public:
    explicit FConvaiInjectedEchoScenario(bool bInSilentEcho = false)
        : bSilentEcho(bInSilentEcho)
    {
    }

    static const TCHAR* StaticName() { return TEXT("injected_echo_reaches_the_mic"); }
    static const TCHAR* ControlName() { return TEXT("injected_echo_silent_control"); }
    virtual const TCHAR* Name() const override
    {
        return bSilentEcho ? ControlName() : StaticName();
    }
    virtual double DeadlineSeconds() const override { return 25.0; }
    virtual bool RequiresLiveConnection() const override { return false; }

    virtual void Start(UWorld* InWorld, FConvaiTestEventRecorder& InRecorder) override
    {
        World = InWorld;
        Recorder = &InRecorder;

        Audio::FMixerDevice* MixerDevice = ResolveMixerDevice(World);
        if (!MixerDevice || !World)
        {
            Recorder->Record(TEXT("setup_failed"),
                             MixerDevice ? TEXT("no world") : TEXT("no mixer device"));
            bSetupFailed = true;
            return;
        }

        AActor* SpawnedOwner = World->SpawnActor<AActor>();
        if (!SpawnedOwner)
        {
            Recorder->Record(TEXT("setup_failed"), TEXT("could not spawn owner actor"));
            bSetupFailed = true;
            return;
        }
        Owner = SpawnedOwner;

        USceneComponent* Root = NewObject<USceneComponent>(SpawnedOwner, TEXT("Root"));
        SpawnedOwner->SetRootComponent(Root);
        Root->RegisterComponent();

        // The far end: unrouted, so it renders into the default chain and lands
        // in the master mix. This is the game's speakers for the purposes of
        // this scenario.
        UConvaiVirtualMicComponent* Speaker =
            NewObject<UConvaiVirtualMicComponent>(SpawnedOwner, TEXT("FarEndSpeaker"));
        Speaker->SoundSubmix = nullptr;
        Speaker->RegisterComponent();
        FarEnd = Speaker;

        // The near end: routed the way the plugin routes real capture, and
        // silent. Everything the capture tap sees arrives through the echo.
        UConvaiVirtualMicComponent* Mic =
            NewObject<UConvaiVirtualMicComponent>(SpawnedOwner, TEXT("NearEndMic"));
        Mic->RegisterComponent();
        NearEnd = Mic;

        USoundSubmix* Assigned = Cast<USoundSubmix>(Mic->GetSubmix());
        if (!Assigned)
        {
            Recorder->Record(TEXT("setup_failed"), TEXT("near-end mic has no USoundSubmix"));
            bSetupFailed = true;
            return;
        }
        CaptureSubmix = Assigned;

        FConvaiInjectedEcho::FParams EchoParams;
        EchoParams.DelayMs = kEchoDelayMs;
        EchoParams.Gain = bSilentEcho ? 0.0f : kEchoGain;
        EchoParams.TargetSampleRate = UConvaiVirtualMicComponent::CaptureSampleRate();
        Echo = MakeShared<FConvaiInjectedEcho, ESPMode::ThreadSafe>(EchoParams);

        MixerDevice->RegisterSubmixBufferListener(Echo.ToSharedRef(),
                                                  MixerDevice->GetMainSubmixObject());
        Mic->SetEchoSource(Echo);

        Tap = MakeShared<FCaptureTap, ESPMode::ThreadSafe>();
        MixerDevice->RegisterSubmixBufferListener(Tap.ToSharedRef(), *Assigned);

        // The near end emits silence rather than nothing: a stopped synth is a
        // different code path from a quiet microphone, and the echo has to be
        // mixed into something.
        Mic->EnqueueSilence(kFarEndSeconds + 1.5f);
        Mic->Start();

        Speaker->EnqueueTone(kFarEndHz, kFarEndSeconds, kFarEndAmplitude);
        Speaker->Start();

        Recorder->Record(TEXT("far_end_started"),
                         FString::Printf(TEXT("gain=%.2f delay=%.0fms"), EchoParams.Gain,
                                         EchoParams.DelayMs));
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }
        Elapsed += DeltaSeconds;
        return Elapsed >= kFarEndSeconds + 1.5f;
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;
        if (bSetupFailed)
        {
            Result.bPassed = false;
            Result.bSetupFailed = true;
            Result.FailReason = TEXT("setup failed; see trace");
            return Result;
        }

        if (Audio::FMixerDevice* MixerDevice = ResolveMixerDevice(World))
        {
            if (Echo.IsValid())
            {
                MixerDevice->UnregisterSubmixBufferListener(
                    Echo.ToSharedRef(), MixerDevice->GetMainSubmixObject());
            }
            if (Tap.IsValid() && CaptureSubmix.IsValid())
            {
                MixerDevice->UnregisterSubmixBufferListener(Tap.ToSharedRef(),
                                                            *CaptureSubmix.Get());
            }
        }
        if (NearEnd.IsValid())
        {
            NearEnd->SetEchoSource(nullptr);
        }

        TArray<float> Mono;
        int32 SampleRate = 0;
        if (Tap.IsValid())
        {
            Tap->Read(Mono, SampleRate);
        }

        const int64 EchoSource = Echo.IsValid() ? Echo->NonSilentSourceSamples() : 0;
        const int64 EchoInjected = Echo.IsValid() ? Echo->InjectedSamples() : 0;
        const float EchoPeak = Echo.IsValid() ? Echo->PeakInjected() : 0.0f;
        const double Ratio = ToneEnergyRatio(Mono, SampleRate, kFarEndHz);
        const double Rms = RootMeanSquare(Mono);

        Result.Metrics.Add(TEXT("echo_source_nonsilent_samples"), static_cast<double>(EchoSource));
        Result.Metrics.Add(TEXT("echo_injected_samples"), static_cast<double>(EchoInjected));
        Result.Metrics.Add(TEXT("echo_peak_amplitude"), EchoPeak);
        Result.Metrics.Add(TEXT("capture_mono_samples"), Mono.Num());
        Result.Metrics.Add(TEXT("capture_sample_rate"), SampleRate);
        Result.Metrics.Add(TEXT("capture_tone_ratio"), Ratio);
        Result.Metrics.Add(TEXT("capture_rms"), Rms);
        Result.Metrics.Add(TEXT("far_end_emitted"),
                           static_cast<double>(FarEnd.IsValid() ? FarEnd->EmittedSamples() : 0));

        // Fixture self-check, and the one the PRD names explicitly: the far end
        // has to have been audible. Without this, "no audio device" and
        // "cancellation worked" are the same measurement -- and it applies to
        // the control run too, which is otherwise satisfied by a dead machine.
        if (EchoSource <= 0)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("the echo tap saw no non-silent audio on the master submix, so the far end "
                     "never played and there was nothing to echo");
            return Result;
        }

        if (Mono.Num() == 0)
        {
            Result.bPassed = false;
            Result.FailReason = TEXT("the capture submix tap captured nothing");
            return Result;
        }

        constexpr double MinToneRatio = 0.25;
        constexpr double MinEchoRms = 1.0e-3;
        const bool bEchoPresent = Ratio > MinToneRatio && Rms > MinEchoRms;

        if (bSilentEcho)
        {
            // The control. Gain 0, so the far end played, the tap saw it, and
            // the microphone still must not carry it.
            Result.bPassed = !bEchoPresent;
            if (!Result.bPassed)
            {
                Result.FailReason = FString::Printf(
                    TEXT("the near-end capture carried the far end's tone at ratio %.3f / RMS %.4g "
                         "with the echo gain set to zero, so the capture path is picking it up by "
                         "some route other than Injected Echo and the paired run would credit the "
                         "canceller for removing something it never added"),
                    Ratio, Rms);
            }
            return Result;
        }

        if (!bEchoPresent)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("injected-echo-not-reaching-capture");
            Finding.Summary =
                TEXT("Injected Echo did not reach the capture path, so echo cancellation has "
                     "nothing to cancel and any AEC result measured on top of it is vacuous");
            Finding.Evidence = FString::Printf(
                TEXT("a %.0f Hz far end played into the master submix and the echo tap saw %lld "
                     "non-silent samples, injecting %lld at peak %.4g with gain %.2f and %.0f ms "
                     "delay; the capture submix then carried tone ratio %.3f at RMS %.4g over %d "
                     "mono samples at %d Hz"),
                kFarEndHz, EchoSource, EchoInjected, EchoPeak, kEchoGain, kEchoDelayMs, Ratio, Rms,
                Mono.Num(), SampleRate);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.bPassed = false;
            Result.FailReason = TEXT("Injected Echo did not reach the capture path");
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

private:
    const bool bSilentEcho;

    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    TWeakObjectPtr<AActor> Owner;
    TWeakObjectPtr<UConvaiVirtualMicComponent> FarEnd;
    TWeakObjectPtr<UConvaiVirtualMicComponent> NearEnd;
    TWeakObjectPtr<USoundSubmix> CaptureSubmix;
    TSharedPtr<FConvaiInjectedEcho, ESPMode::ThreadSafe> Echo;
    TSharedPtr<FCaptureTap, ESPMode::ThreadSafe> Tap;

    bool bSetupFailed = false;
    float Elapsed = 0.0f;
};

CONVAI_REGISTER_SCENARIO(FConvaiInjectedEchoScenario)

// By hand rather than through the macro, which builds one registrar per class
// and this is the same class run with the echo muted.
static struct FConvaiInjectedEchoControlRegistrar
{
    FConvaiInjectedEchoControlRegistrar()
    {
        ConvaiTestRegistry::Register(
            FConvaiInjectedEchoScenario::ControlName(),
            []() -> TSharedRef<FConvaiTestScenario>
            { return MakeShared<FConvaiInjectedEchoScenario>(/*bSilentEcho=*/true); });
    }
} GConvaiInjectedEchoControlRegistrar;
