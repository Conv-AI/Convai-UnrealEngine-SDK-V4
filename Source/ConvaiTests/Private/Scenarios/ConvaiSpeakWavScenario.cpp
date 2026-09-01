// Copyright 2022 Convai Inc. All Rights Reserved.

// Issue 04 — does scripted speech survive the seam?
//
// virtual_mic_adoption proves the plugin takes the Virtual Mic. This proves what
// it takes is still the recording: a fixture goes in at 44.1 kHz, through
// UConvaiUtils::ResampleAudio to 16 kHz, through the synth, and out of
// /ConvAI/Submixes/AudioInput at the mixer's rate. Everything issue 05 injects
// rides this path, so a silent or mangled one would make every AEC number below
// it meaningless.
//
// The tap is on AudioInput rather than on the master submix. F18's refutation is
// why: MuteMic attenuates by 96 dB on the way to master, so a master tap
// measures the mute working rather than the audio arriving. AudioInput is the
// last point where the microphone exists at full scale.
//
// Offline. Issue 04's "a live character transcribes it recognisably" is the
// other half and needs a character id; ExpectedTranscript and WordErrorRate are
// built and unit-checked here so that half is a scenario, not a project.

#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTestSteps.h"
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
    const TCHAR* kFixture = TEXT("S1");

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

    class FSubmixTap : public ISubmixBufferListener
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
            static const FString Name = TEXT("ConvaiSpeakWavTap");
            return Name;
        }

        void Read(TArray<float>& OutMono, int32& OutSampleRate) const
        {
            FScopeLock ScopeLock(&Lock);
            OutMono = Mono;
            OutSampleRate = SampleRate;
        }

        void MarkPhaseBoundary()
        {
            FScopeLock ScopeLock(&Lock);
            PhaseBoundary = Mono.Num();
        }

        int32 GetPhaseBoundary() const
        {
            FScopeLock ScopeLock(&Lock);
            return PhaseBoundary;
        }

    private:
        mutable FCriticalSection Lock;
        TArray<float> Mono;
        int32 SampleRate = 0;
        int32 PhaseBoundary = 0;
    };
}

class FConvaiSpeakWavScenario : public FConvaiTestScenario
{
public:
    static const TCHAR* StaticName() { return TEXT("speak_wav_through_virtual_mic"); }
    virtual const TCHAR* Name() const override { return StaticName(); }
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

        UConvaiVirtualMicComponent* Mic =
            NewObject<UConvaiVirtualMicComponent>(SpawnedOwner, TEXT("VirtualMic"));
        Mic->RegisterComponent();
        VirtualMic = Mic;

        USoundSubmix* Assigned = Cast<USoundSubmix>(Mic->GetSubmix());
        if (!Assigned)
        {
            Recorder->Record(TEXT("setup_failed"), TEXT("Virtual Mic has no USoundSubmix"));
            bSetupFailed = true;
            return;
        }
        TappedSubmix = Assigned;

        FString Error;
        if (!ConvaiTestSteps::SpeakWav(Mic, kFixture, Error))
        {
            Recorder->Record(TEXT("setup_failed"), Error);
            bSetupFailed = true;
            return;
        }
        QueuedSamples = Mic->PendingSamples();
        ConvaiTestSteps::Silence(Mic, SilenceSeconds);

        Tap = MakeShared<FSubmixTap, ESPMode::ThreadSafe>();
        MixerDevice->RegisterSubmixBufferListener(Tap.ToSharedRef(), *Assigned);

        Mic->Start();
        Recorder->Record(TEXT("speech_started"),
                         FString::Printf(TEXT("%s, %d samples queued"), kFixture, QueuedSamples));
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }
        Elapsed += DeltaSeconds;

        // The boundary is the queue draining, not a clock: the audio thread sets
        // the pace, and a wall-clock split would measure the scheduler and put
        // speech into the control window on a slow frame.
        if (!bPhaseMarked && VirtualMic.IsValid() && VirtualMic->PendingSamples() <= SilenceTail())
        {
            bPhaseMarked = true;
            DrainedAtSeconds = Elapsed;
            if (Tap.IsValid())
            {
                Tap->MarkPhaseBoundary();
            }
            Recorder->Record(TEXT("speech_drained"));
        }

        return bPhaseMarked && Elapsed >= DrainedAtSeconds + SilenceSeconds;
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
            if (Tap.IsValid() && TappedSubmix.IsValid())
            {
                MixerDevice->UnregisterSubmixBufferListener(Tap.ToSharedRef(),
                                                            *TappedSubmix.Get());
            }
        }

        TArray<float> Mono;
        int32 SampleRate = 0;
        if (Tap.IsValid())
        {
            Tap->Read(Mono, SampleRate);
        }

        const int32 Boundary = Tap.IsValid() ? Tap->GetPhaseBoundary() : 0;
        TArray<float> SpeechPhase(Mono.GetData(), FMath::Min(Boundary, Mono.Num()));
        TArray<float> SilencePhase;
        if (Boundary < Mono.Num())
        {
            SilencePhase.Append(Mono.GetData() + Boundary, Mono.Num() - Boundary);
        }

        const double SpeechRms = RootMeanSquare(SpeechPhase);
        const double SilenceRms = RootMeanSquare(SilencePhase);
        const int64 Emitted = VirtualMic.IsValid() ? VirtualMic->EmittedSamples() : 0;

        Result.Metrics.Add(TEXT("queued_samples"), QueuedSamples);
        Result.Metrics.Add(TEXT("emitted_samples"), static_cast<double>(Emitted));
        Result.Metrics.Add(TEXT("submix_mono_samples"), Mono.Num());
        Result.Metrics.Add(TEXT("submix_sample_rate"), SampleRate);
        Result.Metrics.Add(TEXT("speech_phase_samples"), SpeechPhase.Num());
        Result.Metrics.Add(TEXT("silence_phase_samples"), SilencePhase.Num());
        Result.Metrics.Add(TEXT("speech_phase_rms"), SpeechRms);
        Result.Metrics.Add(TEXT("silence_phase_rms"), SilenceRms);

        // WordErrorRate has no caller until issue 04's live half exists, so it
        // is exercised here at both ends: a transcript against itself must be 0,
        // and against an unrelated fixture must not be. The identity case alone
        // would pass on an implementation that always returns 0, which is the
        // shape a broken Levenshtein takes.
        //
        // The unrelated figure is above 1 and that is correct, not a bug: WER
        // normalises edits by the *reference* length, so a hypothesis longer
        // than the reference is unbounded. S1's 7 words against M1's 27 is 27
        // edits over 7, which is what the metric reports.
        const FString Spoken = ConvaiTestSteps::ExpectedTranscript(kFixture);
        const FString Unrelated = ConvaiTestSteps::ExpectedTranscript(TEXT("M1"));
        Result.Metrics.Add(TEXT("wer_identical"),
                           ConvaiTestSteps::WordErrorRate(Spoken, Spoken));
        Result.Metrics.Add(TEXT("wer_unrelated"),
                           ConvaiTestSteps::WordErrorRate(Spoken, Unrelated));
        Result.Metrics.Add(TEXT("transcript_chars"), Spoken.Len());

        if (QueuedSamples <= 0)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("SpeakWav queued nothing, so this measures the fixture loader rather than the "
                     "audio path");
            return Result;
        }

        if (Spoken.IsEmpty() || Unrelated.IsEmpty())
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("STT.json did not yield a transcript for the fixture, so the word error rate "
                     "the live half of issue 04 depends on has no reference");
            return Result;
        }

        if (Mono.Num() == 0)
        {
            Result.bPassed = false;
            Result.FailReason = TEXT("the AudioInput submix tap captured nothing");
            return Result;
        }

        if (SilencePhase.Num() == 0)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("no silence phase was captured, so the speech level cannot be attributed to "
                     "the fixture rather than to whatever else the submix carries");
            return Result;
        }

        // The assertion, against the scenario's own silent tail rather than
        // against zero. The submix idles between buffers and the mixer is not
        // required to hand back digital silence; what matters is that speaking
        // is loud and not speaking is not.
        constexpr double MinSpeechRms = 0.01;
        constexpr double MinSpeechOverSilenceDb = 20.0;
        const double SeparationDb =
            SilenceRms > 0.0
                ? 20.0 * FMath::LogX(10.0f, static_cast<float>(SpeechRms / SilenceRms))
                : 300.0;
        Result.Metrics.Add(TEXT("speech_over_silence_db"), SeparationDb);

        if (SpeechRms < MinSpeechRms || SeparationDb < MinSpeechOverSilenceDb)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("scripted-speech-lost-before-submix");
            Finding.Summary =
                TEXT("scripted audio queued into the Virtual Mic did not arrive at "
                     "/ConvAI/Submixes/AudioInput, so nothing downstream of the capture seam is "
                     "carrying the player's voice");
            Finding.Evidence = FString::Printf(
                TEXT("fixture %s queued %d samples at %d Hz and emitted %lld; the submix carried "
                     "RMS %.4g while speaking against %.4g after the queue drained (%.1f dB apart, "
                     "%d vs %d mono samples at %d Hz)"),
                kFixture, QueuedSamples, UConvaiVirtualMicComponent::CaptureSampleRate(), Emitted,
                SpeechRms, SilenceRms, SeparationDb, SpeechPhase.Num(), SilencePhase.Num(),
                SampleRate);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.bPassed = false;
            Result.FailReason = TEXT("scripted speech did not reach the capture submix");
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

private:
    // Everything after the fixture is the control tail, so "drained" means only
    // the tail is left rather than the queue being empty.
    int32 SilenceTail() const
    {
        return FMath::RoundToInt(SilenceSeconds *
                                 UConvaiVirtualMicComponent::CaptureSampleRate());
    }

    static constexpr float SilenceSeconds = 1.5f;

    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    TWeakObjectPtr<AActor> Owner;
    TWeakObjectPtr<UConvaiVirtualMicComponent> VirtualMic;
    TWeakObjectPtr<USoundSubmix> TappedSubmix;
    TSharedPtr<FSubmixTap, ESPMode::ThreadSafe> Tap;

    bool bSetupFailed = false;
    bool bPhaseMarked = false;
    int32 QueuedSamples = 0;
    float Elapsed = 0.0f;
    float DrainedAtSeconds = 0.0f;
};

CONVAI_REGISTER_SCENARIO(FConvaiSpeakWavScenario)
