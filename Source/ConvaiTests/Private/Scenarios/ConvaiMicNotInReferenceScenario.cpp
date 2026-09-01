// Copyright 2022 Convai Inc. All Rights Reserved.

// Issue 03's F7 regression guard.
//
// F7 is resolved: /ConvAI/Submixes/AudioInput has Parent Submix = MuteMic, so
// the player's own voice contributes silence to the master mix and never enters
// Reference Audio. Cancellation is not attacking the near-end talker by that
// route.
//
// But that routing lives in a .uasset no code references by structure.
// Reparenting AudioInput would put the microphone into Reference Audio with no
// compile error and no log line, and F12 shows what a canceller does to a
// player it can hear in its own reference: it destroys them. This is the only
// thing that would catch it.
//
// The guard is a tone rather than speech because a tone is trivially separable
// from the noise the ground-truth listener would otherwise pick up, and the
// question is about level, not intelligibility.
//
// It ships with mic_in_reference_audio_control, which runs the same tone through
// the same tap with the mic's routing removed. Measured -105.9 dB routed against
// -10.1 dB unrouted; without that pairing there is no way to tell a guard that
// passes from a guard that cannot fail, and F18 is what happens when you try.

#include "ConvaiPlayerComponent.h"
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
    constexpr float kToneHz = 1000.0f;
    constexpr float kToneAmplitude = 0.8f;
    constexpr int32 kMasterSampleRateGuess = 48000;

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

    // Energy at one frequency, by direct Goertzel-style correlation. A full FFT
    // would be more than this needs: there is exactly one frequency of interest
    // and the question is only whether it is present.
    double ToneEnergyRatio(const TArray<float>& Mono, int32 SampleRate, float Frequency)
    {
        if (Mono.Num() == 0 || SampleRate <= 0)
        {
            return 0.0;
        }

        double Real = 0.0;
        double Imag = 0.0;
        double Total = 0.0;
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
        // Normalised so a pure tone at this frequency approaches 1.
        const double ToneEnergy = 2.0 * (Real * Real + Imag * Imag) / Mono.Num();
        return FMath::Clamp(ToneEnergy / Total, 0.0, 1.0);
    }

    // ToneEnergyRatio normalises by total energy, so it answers "what fraction of
    // this mix is the tone" and cannot answer "how loud is the tone". Those come
    // apart exactly where this guard has to be sharp: MuteMic's -96 dB is applied
    // to its own buffer before it is mixed into its parent
    // (AudioMixerSubmix.cpp:1756-1783), so a working mute still leaves a tone in
    // the master mix at 1.6e-5 of its emitted amplitude -- and if that tone is the
    // only thing playing, its ratio is ~1 either way. The absolute level is what
    // separates a muted microphone from an audible one.
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

    class FMasterMixTap : public ISubmixBufferListener
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
            // Downmix to mono; the tone is present in every channel or none.
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
            static const FString Name = TEXT("ConvaiMicNotInReferenceTap");
            return Name;
        }

        void Read(TArray<float>& OutMono, int32& OutSampleRate) const
        {
            FScopeLock ScopeLock(&Lock);
            OutMono = Mono;
            OutSampleRate = SampleRate > 0 ? SampleRate : kMasterSampleRateGuess;
        }

        /** Marks where the tone phase ended, so the two phases can be scored
         *  separately from one continuous capture. */
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

class FConvaiMicNotInReferenceScenario : public FConvaiTestScenario
{
public:
    // The negative control the guard needs to mean anything. Unrouted, the
    // Virtual Mic renders into the default chain and therefore into the master
    // mix at full strength, so the control asserts the tone *is* found. If both
    // the guard and its control report a muted microphone, the instrument is
    // deaf and the guard's pass is worthless.
    // The third arm is F19's. The mic starts unrouted exactly as the control
    // does, and then a UConvaiPlayerComponent adopts it through
    // SetAudioCaptureComponent -- the documented extension point, and the whole
    // of the adoption path. The pass condition is the guard's, not the
    // control's: adoption is supposed to route it, so the tone must not reach
    // the master mix. Without this arm the fix is only ever observed as a
    // non-null pointer, and a pointer assigned after the synth has already
    // copied it changes nothing about where the audio goes.
    explicit FConvaiMicNotInReferenceScenario(bool bInUnrouted = false,
                                              bool bInAdoptByPlayer = false)
        : bUnrouted(bInUnrouted)
        , bAdoptByPlayer(bInAdoptByPlayer)
    {
    }

    static const TCHAR* StaticName() { return TEXT("mic_not_in_reference_audio"); }
    static const TCHAR* ControlName() { return TEXT("mic_in_reference_audio_control"); }
    static const TCHAR* AdoptedName() { return TEXT("adopted_mic_not_in_reference_audio"); }
    virtual const TCHAR* Name() const override
    {
        return bAdoptByPlayer ? AdoptedName() : bUnrouted ? ControlName() : StaticName();
    }
    virtual double DeadlineSeconds() const override { return 20.0; }
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
        if (bUnrouted || bAdoptByPlayer)
        {
            // Cleared before RegisterComponent, because USynthComponent copies
            // SoundSubmix into the sound it plays when it initialises. The
            // adopted arm starts from the same place the control does; what
            // separates them is only whether the plugin gets to adopt it.
            Mic->SoundSubmix = nullptr;
        }
        Mic->RegisterComponent();
        VirtualMic = Mic;

        if (bAdoptByPlayer)
        {
            UConvaiPlayerComponent* Player =
                NewObject<UConvaiPlayerComponent>(SpawnedOwner, TEXT("ConvaiPlayer"));
            Player->RegisterComponent();
            PlayerComponent = Player;

            // SetAudioCaptureComponent rather than StartRecording: it is the
            // function the finding names and the one every adoption route ends
            // at, and it keeps the plugin's own recorder out of a measurement
            // about routing.
            bAdopted = Player->SetAudioCaptureComponent(Mic);
            Recorder->Record(TEXT("adopted"), bAdopted ? TEXT("true") : TEXT("false"));
            Recorder->Record(TEXT("submix_after_adoption"),
                             GetNameSafe(Mic->GetSubmix()));
        }

        // Record the routing the tone is about to travel. Without this a
        // failure cannot distinguish "AudioInput was reparented" from "the
        // SoundSubmix property was set but the engine ignored it", and those
        // have completely different fixes.
        if (USoundSubmixBase* Assigned = Mic->GetSubmix())
        {
            // Every submix in the chain with its own volume. One of them has to
            // be the mute if the routing is doing what its name claims, and
            // which one it is decides whether the fix is an asset edit or a
            // reparent. Output volume is a modulation destination in 5.8; the
            // base value is what the asset was authored with, and a muting
            // submix sits at -96 dB.
            FString Chain;
            USoundSubmixBase* Walk = Assigned;
            for (int32 Depth = 0; Depth < 8 && Walk; ++Depth)
            {
                if (!Chain.IsEmpty())
                {
                    Chain += TEXT(" -> ");
                }
                Chain += Walk->GetName();

                USoundSubmix* AsSubmix = Cast<USoundSubmix>(Walk);
                if (!AsSubmix)
                {
                    break;
                }
                Chain += FString::Printf(TEXT("(%.1fdB)"),
                                         AsSubmix->OutputVolumeModulation.Value);
                Walk = AsSubmix->ParentSubmix;
            }
            // No parent on the last entry means it is a default endpoint submix
            // and is summed into the master output regardless of the chain.
            Recorder->Record(TEXT("mic_submix_chain"), Chain);
            Recorder->Record(TEXT("mic_submix_chain_terminates_at_root"),
                             Walk == nullptr ? TEXT("yes") : TEXT("no"));
        }
        else
        {
            Recorder->Record(TEXT("mic_submix_chain"), TEXT("<none>"));
        }

        Tap = MakeShared<FMasterMixTap, ESPMode::ThreadSafe>();
        MixerDevice->RegisterSubmixBufferListener(Tap.ToSharedRef(),
                                                  MixerDevice->GetMainSubmixObject());

        // Phase one: the tone alone. Nothing else is playing, so if the
        // microphone reaches the master submix the tone is all that will be in
        // it, and if it does not the mix stays silent.
        Mic->EnqueueTone(kToneHz, ToneSeconds, kToneAmplitude);
        Mic->Start();
        Recorder->Record(TEXT("tone_started"));
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }
        Elapsed += DeltaSeconds;

        // Phase boundary: after this the mic emits nothing, so any 1 kHz energy
        // still in the master mix belongs to something else in the scene. The
        // host map plays character audio, and without this control the
        // assertion could not tell that apart from the microphone leaking.
        if (!bPhaseMarked && Elapsed >= ToneSeconds + 0.5f)
        {
            bPhaseMarked = true;
            if (Tap.IsValid())
            {
                Tap->MarkPhaseBoundary();
            }
            if (Recorder)
            {
                Recorder->Record(TEXT("tone_phase_ended"));
            }
        }

        return Elapsed >= ToneSeconds + SilenceSeconds + 0.5f;
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
            if (Tap.IsValid())
            {
                MixerDevice->UnregisterSubmixBufferListener(Tap.ToSharedRef(),
                                                            MixerDevice->GetMainSubmixObject());
            }
        }

        TArray<float> Mono;
        int32 SampleRate = 0;
        if (Tap.IsValid())
        {
            Tap->Read(Mono, SampleRate);
        }

        const int64 Emitted = VirtualMic.IsValid() ? VirtualMic->EmittedSamples() : 0;
        const int32 Pending = VirtualMic.IsValid() ? VirtualMic->PendingSamples() : -1;

        const int32 Boundary = Tap.IsValid() ? Tap->GetPhaseBoundary() : 0;
        TArray<float> TonePhase(Mono.GetData(), FMath::Min(Boundary, Mono.Num()));
        TArray<float> SilencePhase;
        if (Boundary < Mono.Num())
        {
            SilencePhase.Append(Mono.GetData() + Boundary, Mono.Num() - Boundary);
        }

        const double Ratio = ToneEnergyRatio(TonePhase, SampleRate, kToneHz);
        const double ControlRatio = ToneEnergyRatio(SilencePhase, SampleRate, kToneHz);

        Result.Metrics.Add(TEXT("mic_emitted_samples"), static_cast<double>(Emitted));
        Result.Metrics.Add(TEXT("mic_pending_samples"), Pending);
        Result.Metrics.Add(TEXT("master_mono_samples"), Mono.Num());
        Result.Metrics.Add(TEXT("master_sample_rate"), SampleRate);
        Result.Metrics.Add(TEXT("tone_phase_samples"), TonePhase.Num());
        Result.Metrics.Add(TEXT("silence_phase_samples"), SilencePhase.Num());
        Result.Metrics.Add(TEXT("tone_energy_ratio"), Ratio);
        Result.Metrics.Add(TEXT("tone_energy_ratio_control"), ControlRatio);

        // Attenuation from the microphone's own output to the master mix. A
        // sine at amplitude A has RMS A/sqrt(2), so 0 dB here means the whole
        // microphone signal arrives at the master mix unattenuated and -96 dB
        // means MuteMic's authored gain was applied on the way.
        const double TonePhaseRms = RootMeanSquare(TonePhase);
        const double EmittedRms = kToneAmplitude / FMath::Sqrt(2.0);
        Result.Metrics.Add(TEXT("tone_phase_rms"), TonePhaseRms);
        Result.Metrics.Add(TEXT("silence_phase_rms"), RootMeanSquare(SilencePhase));
        Result.Metrics.Add(TEXT("mic_emitted_rms"), EmittedRms);
        // -300 stands in for silence: JSON has no -inf and the Python side
        // parses these with json.loads.
        const double AttenuationDb =
            TonePhaseRms > 0.0
                ? 20.0 * FMath::LogX(10.0f, static_cast<float>(TonePhaseRms / EmittedRms))
                : -300.0;
        Result.Metrics.Add(TEXT("mic_to_master_attenuation_db"), AttenuationDb);

        // Fixture self-check. If the Virtual Mic never rendered, the tone was
        // never emitted and "the tone is absent from the master mix" is true for
        // the wrong reason — the exact shape that would let a reparented
        // AudioInput slip through this guard forever.
        if (Emitted <= 0)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("the Virtual Mic never rendered, so the tone was never emitted — this guard "
                     "cannot distinguish a correctly muted microphone from a silent one");
            return Result;
        }

        if (Mono.Num() == 0)
        {
            Result.bPassed = false;
            Result.FailReason = TEXT("the master submix tap captured nothing");
            return Result;
        }

        const bool bMicRouted = VirtualMic.IsValid() && VirtualMic->GetSubmix() != nullptr;

        if (bAdoptByPlayer)
        {
            Result.Metrics.Add(TEXT("adopted"), bAdopted ? 1.0 : 0.0);
            Result.Metrics.Add(TEXT("adopted_component_routed"), bMicRouted ? 1.0 : 0.0);
            if (!bAdopted)
            {
                Result.bPassed = false;
                Result.FailReason =
                    TEXT("SetAudioCaptureComponent refused the Virtual Mic, so nothing was "
                         "adopted and this arm is not measuring the adoption path");
                return Result;
            }
        }

        // The control run: the microphone is deliberately unrouted, so it renders
        // into the default chain and reaches the master mix at full strength.
        // Its job is to prove the instrument can see that at all. A guard whose
        // control also reports a muted microphone is measuring nothing.
        if (bUnrouted)
        {
            if (bMicRouted)
            {
                Result.bPassed = false;
                Result.FailReason = TEXT("the control kept its SoundSubmix, so it is not a control");
                return Result;
            }

            constexpr double MinControlAttenuationDb = -40.0;
            Result.bPassed = AttenuationDb > MinControlAttenuationDb;
            if (!Result.bPassed)
            {
                Result.FailReason = FString::Printf(
                    TEXT("an unrouted Virtual Mic reached the master submix %.1f dB down, so this "
                         "measurement cannot detect a microphone that is inside Reference Audio — "
                         "the guard it controls proves nothing"),
                    AttenuationDb);
            }
            return Result;
        }

        // Second self-check, and the one that makes this guard mean something.
        // A Virtual Mic with no submix assigned renders into the default chain
        // and therefore into the master mix by construction, which would fire
        // the assertion below on the test's own miswiring rather than on the
        // plugin's routing.
        if (!bMicRouted)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("the Virtual Mic has no SoundSubmix assigned, so it bypasses "
                     "/ConvAI/Submixes/AudioInput entirely — this guard would be measuring the "
                     "test's routing, not the plugin's");
            return Result;
        }

        if (SilencePhase.Num() == 0)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("no silence phase was captured, so the tone cannot be attributed to the "
                     "microphone rather than to anything else in the scene");
            return Result;
        }

        // The assertion is on level, not on tone_energy_ratio. The ratio is
        // normalised by total energy, so it reads ~0.84 both for a microphone
        // muted to nothing and for one at full strength -- the two differ by
        // 95.7 dB here and the ratio cannot see it. F18 was recorded off that
        // ratio and refuted by this measurement.
        //
        // -40 dB sits 65 dB from both measured cases, so it is a threshold
        // neither scene content nor the resample path can drift across.
        constexpr double MaxAttenuationDb = -40.0;
        if (AttenuationDb > MaxAttenuationDb)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("microphone-inside-reference-audio");
            Finding.Summary =
                TEXT("the microphone's own signal is audible in the master submix, so it is inside "
                     "Reference Audio and echo cancellation will attack the player's own voice");
            Finding.Evidence = FString::Printf(
                TEXT("a %.0f Hz tone emitted through the Virtual Mic at RMS %.3g, routed to "
                     "/ConvAI/Submixes/AudioInput, reaches the master submix at RMS %.3g -- "
                     "%.1f dB down, above the %.1f dB threshold. MuteMic's authored -96 dB puts "
                     "a correctly routed mic at about -105 dB and an unrouted one at -10 dB "
                     "(mic_in_reference_audio_control), so AudioInput no longer routes through a "
                     "muting submix"),
                kToneHz, EmittedRms, TonePhaseRms, AttenuationDb, MaxAttenuationDb);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.bPassed = false;
            Result.FailReason = TEXT("microphone signal found in Reference Audio");
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

private:
    const bool bUnrouted;
    const bool bAdoptByPlayer;

    static constexpr float ToneSeconds = 2.0f;
    static constexpr float SilenceSeconds = 2.0f;

    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    TWeakObjectPtr<AActor> Owner;
    TWeakObjectPtr<UConvaiVirtualMicComponent> VirtualMic;
    TWeakObjectPtr<UConvaiPlayerComponent> PlayerComponent;
    TSharedPtr<FMasterMixTap, ESPMode::ThreadSafe> Tap;

    bool bAdopted = false;
    bool bSetupFailed = false;
    bool bPhaseMarked = false;
    float Elapsed = 0.0f;
};

CONVAI_REGISTER_SCENARIO(FConvaiMicNotInReferenceScenario)

// Registered by hand rather than through the macro, which builds one registrar
// per class and this is the same class run the other way round.
static struct FConvaiMicInReferenceControlRegistrar
{
    FConvaiMicInReferenceControlRegistrar()
    {
        ConvaiTestRegistry::Register(
            FConvaiMicNotInReferenceScenario::ControlName(),
            []() -> TSharedRef<FConvaiTestScenario>
            { return MakeShared<FConvaiMicNotInReferenceScenario>(/*bUnrouted=*/true); });

        ConvaiTestRegistry::Register(
            FConvaiMicNotInReferenceScenario::AdoptedName(),
            []() -> TSharedRef<FConvaiTestScenario>
            {
                return MakeShared<FConvaiMicNotInReferenceScenario>(/*bUnrouted=*/false,
                                                                    /*bAdoptByPlayer=*/true);
            });
    }
} GConvaiMicInReferenceControlRegistrar;
