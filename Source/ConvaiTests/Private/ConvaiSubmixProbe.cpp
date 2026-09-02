// Copyright 2022 Convai Inc. All Rights Reserved.

// Issue 01 — does the master submix actually render on this machine?
//
// Everything downstream assumes StartRecordingOutput / StopRecording on the
// master submix return non-silent buffers in the configurations the suite runs
// in. If a headless run gets a renderer that skips submix rendering, the machine
// requirements for every later issue change, and discovering that at issue 05 is
// expensive.
//
// Two independent taps, deliberately. The recorder is what the plugin's
// Reference Audio path uses; an ISubmixBufferListener is what issue 03 needs as
// ground truth. Measuring both in one run is what distinguishes "the submix did
// not render" from "the submix rendered and the recorder API returned nothing",
// which are different problems with different fixes.
//
// Deviation from the issue as written, and why: the issue says drive a character
// to speak. That needs a backend and credentials, which the headless
// configurations are meant to run without. An engine sound answers the same
// question with nothing but the engine. Whether Reference Audio reaches
// FanAudioChunkToClients is issue 03's measurement, not this one's.
//
// Output is one CONVAI_PROBE line per fact so a runner can grep it without
// parsing prose.

#include "ConvaiTests.h"

#include "AudioDevice.h"
#include "AudioMixerBlueprintLibrary.h"
#include "AudioThread.h"
#include "AudioMixerDevice.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ISubmixBufferListener.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundSubmix.h"

namespace
{
    // One key=value per line. Prose in a log is not a result; this is.
    void Emit(const TCHAR* Key, const FString& Value)
    {
        UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_PROBE %s=%s"), Key, *Value);
    }

    // Counts what the mixer actually rendered. Independent of the recorder so a
    // dead recorder and a dead submix cannot look the same.
    class FProbeSubmixListener : public ISubmixBufferListener
    {
    public:
        virtual void OnNewSubmixBuffer(const USoundSubmix*, float* AudioData, int32 NumSamples,
                                       int32 InNumChannels, const int32 InSampleRate, double) override
        {
            if (!AudioData || NumSamples <= 0)
            {
                return;
            }

            float LocalPeak = 0.0f;
            double LocalSumSquares = 0.0;
            for (int32 i = 0; i < NumSamples; ++i)
            {
                LocalPeak = FMath::Max(LocalPeak, FMath::Abs(AudioData[i]));
                LocalSumSquares += static_cast<double>(AudioData[i]) * AudioData[i];
            }

            FScopeLock Lock(&StateLock);
            ++Buffers;
            Samples += NumSamples;
            SumSquares += LocalSumSquares;
            Peak = FMath::Max(Peak, LocalPeak);
            NumChannels = InNumChannels;
            SampleRate = InSampleRate;
        }

        virtual const FString& GetListenerName() const override
        {
            static const FString Name = TEXT("ConvaiProbeSubmixListener");
            return Name;
        }

        void Report() const
        {
            FScopeLock Lock(&StateLock);
            Emit(TEXT("listener_buffers"), FString::FromInt(Buffers));
            Emit(TEXT("listener_samples"), FString::FromInt(Samples));
            Emit(TEXT("listener_peak"), FString::SanitizeFloat(Peak));
            Emit(TEXT("listener_rms"),
                 FString::SanitizeFloat(Samples > 0 ? FMath::Sqrt(SumSquares / Samples) : 0.0));
            Emit(TEXT("listener_channels"), FString::FromInt(NumChannels));
            Emit(TEXT("listener_sample_rate"), FString::FromInt(SampleRate));
        }

        bool SawNonSilentAudio() const
        {
            FScopeLock Lock(&StateLock);
            return Buffers > 0 && Peak > 1e-5f;
        }

    private:
        mutable FCriticalSection StateLock;
        int32 Buffers = 0;
        int64 Samples = 0;
        double SumSquares = 0.0;
        float Peak = 0.0f;
        int32 NumChannels = 0;
        int32 SampleRate = 0;
    };

    Audio::FMixerDevice* ResolveMixerDevice(UWorld* World)
    {
        FAudioDeviceHandle Handle = World ? World->GetAudioDevice() : FAudioDeviceHandle();
        if (!Handle.IsValid() && GEngine)
        {
            Handle = GEngine->GetMainAudioDevice();
        }

        FAudioDevice* Device = Handle.GetAudioDevice();
        if (!Device)
        {
            return nullptr;
        }

        // FAudioDevice::IsAudioMixerEnabled() is gone in 5.8 — the mixer is the
        // only backend now. The plugin's reference thread already casts
        // unconditionally on 5.3+
        // (ConvaiReferenceAudioThread.cpp:187-195); mirror that so the probe
        // reports what the plugin would get.
        return static_cast<Audio::FMixerDevice*>(Device);
    }

    // Returns whether the recording came back non-empty.
    bool ReportRecordedBuffer(const TCHAR* Tag, const Audio::FAlignedFloatBuffer& Buffer,
                              float NumChannels, float SampleRate)
    {
        float Peak = 0.0f;
        double SumSquares = 0.0;
        for (const float Sample : Buffer)
        {
            Peak = FMath::Max(Peak, FMath::Abs(Sample));
            SumSquares += static_cast<double>(Sample) * Sample;
        }

        Emit(*FString::Printf(TEXT("recorded_%s_samples"), Tag), FString::FromInt(Buffer.Num()));
        Emit(*FString::Printf(TEXT("recorded_%s_sample_rate"), Tag),
             FString::SanitizeFloat(SampleRate));
        Emit(*FString::Printf(TEXT("recorded_%s_channels"), Tag),
             FString::SanitizeFloat(NumChannels));
        Emit(*FString::Printf(TEXT("recorded_%s_peak"), Tag), FString::SanitizeFloat(Peak));
        Emit(*FString::Printf(TEXT("recorded_%s_rms"), Tag),
             FString::SanitizeFloat(Buffer.Num() > 0 ? FMath::Sqrt(SumSquares / Buffer.Num()) : 0.0));

        return Buffer.Num() > 0 && Peak > 1e-5f;
    }

    // The probe finishes on a ticker, so -ExecCmds cannot append "quit" — that
    // would fire before the capture window closes. The command takes the quit
    // itself instead, which is also what a headless runner needs.
    void FinishProbe(bool bQuitWhenDone)
    {
        Emit(TEXT("done"), TEXT("1"));
        if (bQuitWhenDone)
        {
            FPlatformMisc::RequestExit(/*Force=*/false);
        }
    }

    void RunSubmixProbe(const TArray<FString>& Args, UWorld* World, FOutputDevice&)
    {
        const float DurationSeconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 1.5f;
        const bool bQuitWhenDone = Args.ContainsByPredicate(
            [](const FString& Arg) { return Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase); });

        // -nosound gives a null device and silences the whole Reference Audio
        // path. The PRD forbids it for suite runs; report it rather than let a
        // run that used it look like a machine capability problem.
        Emit(TEXT("cmdline_nosound"),
             FParse::Param(FCommandLine::Get(), TEXT("nosound")) ? TEXT("yes") : TEXT("no"));
        Emit(TEXT("can_render_audio"), FApp::CanEverRenderAudio() ? TEXT("yes") : TEXT("no"));
        Emit(TEXT("has_world"), World ? TEXT("yes") : TEXT("no"));

        Audio::FMixerDevice* MixerDevice = ResolveMixerDevice(World);
        Emit(TEXT("mixer_device"), MixerDevice ? TEXT("yes") : TEXT("no"));
        if (!MixerDevice || !World)
        {
            Emit(TEXT("submix_renders"), TEXT("no"));
            Emit(TEXT("failure"), MixerDevice ? TEXT("no_world") : TEXT("no_mixer_device"));
            FinishProbe(bQuitWhenDone);
            return;
        }

        Emit(TEXT("device_sample_rate"), FString::SanitizeFloat(MixerDevice->GetSampleRate()));
        Emit(TEXT("device_output_channels"), FString::FromInt(MixerDevice->GetNumDeviceChannels()));

        USoundBase* Probe = LoadObject<USoundBase>(nullptr, TEXT("/Engine/EngineSounds/WhiteNoise"));
        Emit(TEXT("probe_sound"), Probe ? TEXT("loaded") : TEXT("missing"));
        if (!Probe)
        {
            Emit(TEXT("submix_renders"), TEXT("no"));
            Emit(TEXT("failure"), TEXT("probe_sound_missing"));
            FinishProbe(bQuitWhenDone);
            return;
        }
        const float ProbeDuration = Probe->GetDuration();
        Emit(TEXT("probe_sound_duration"), FString::SanitizeFloat(ProbeDuration));

        TSharedRef<FProbeSubmixListener, ESPMode::ThreadSafe> Listener =
            MakeShared<FProbeSubmixListener, ESPMode::ThreadSafe>();
        USoundSubmix& MainSubmix = MixerDevice->GetMainSubmixObject();
        MixerDevice->RegisterSubmixBufferListener(Listener, MainSubmix);

        // FMixerSubmix::ProcessAudio early-outs on auto-disable
        // (AudioMixerSubmix.cpp:1410) *before* it appends to the recording
        // buffer (:1686), so a silent master submix yields zero recorded
        // samples rather than zeros. The probe therefore has to keep something
        // audible playing for the whole window, or it measures the auto-disable
        // rather than the machine.
        Emit(TEXT("engine_use_sound"), GEngine && GEngine->UseSound() ? TEXT("yes") : TEXT("no"));
        Emit(TEXT("app_volume_multiplier"), FString::SanitizeFloat(FApp::GetVolumeMultiplier()));

        UAudioComponent* Component = UGameplayStatics::SpawnSound2D(World, Probe);
        Emit(TEXT("spawned_audio_component"), Component ? TEXT("yes") : TEXT("no"));

        const float RetriggerInterval = FMath::Max(0.05f, ProbeDuration * 0.8f);
        FTSTicker::FDelegateHandle RetriggerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([World, Probe](float) -> bool
            {
                if (IsValid(World) && IsValid(Probe))
                {
                    UGameplayStatics::SpawnSound2D(World, Probe);
                }
                return true;
            }),
            RetriggerInterval);

        // The master submix auto-disables when it has no active sources, and
        // the disabled path returns before the recorder append. Sampling the
        // source count mid-window says whether the sound ever became a source
        // at all, which is a different failure from it being inaudible.
        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([World](float) -> bool
            {
                if (Audio::FMixerDevice* Device = ResolveMixerDevice(World))
                {
                    Emit(TEXT("active_sources_midwindow"),
                         FString::FromInt(Device->GetNumActiveSources()));
                }
                return false;
            }),
            0.5f);

        // The A/B that matters. FConvaiReferenceAudioThread passes nullptr for
        // the submix and relies on "null means the master submix"
        // (ConvaiReferenceAudioThread.cpp:318, :216). In 5.8 that resolves
        // through FMixerDevice::GetSubmixInstance -> GetRequiredSubmixInstance,
        // which compares InSubmix against each entry of RequiredSubmixes — so a
        // null entry in that array makes nullptr match a submix that is not
        // Main. Recording both ways in one run says whether the plugin's
        // argument is capturing the right thing.
        //
        // Reserve past the measurement window so a slow stop cannot truncate the
        // capture; this probe is not measuring the reserve (see F11).
        UAudioMixerBlueprintLibrary::StartRecordingOutput(World, DurationSeconds + 1.0f, nullptr);
        UAudioMixerBlueprintLibrary::StartRecordingOutput(World, DurationSeconds + 1.0f,
                                                          &MainSubmix);

        // FMixerDevice::StartRecording marshals to the audio thread when called
        // from the game thread. If that queue never drains, bIsRecording is
        // never set and the capture is empty for a reason that has nothing to do
        // with the submix. Confirm the queue runs.
        Emit(TEXT("start_called_from_audio_thread"), IsInAudioThread() ? TEXT("yes") : TEXT("no"));
        FAudioThread::RunCommandOnAudioThread(
            []() { Emit(TEXT("audio_thread_command_ran"), TEXT("yes")); });

        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [World, Listener, RetriggerHandle, bQuitWhenDone](float) -> bool
                {
                    FTSTicker::GetCoreTicker().RemoveTicker(RetriggerHandle);

                    bool bNullArgWorks = false;
                    bool bExplicitArgWorks = false;
                    if (Audio::FMixerDevice* Device = ResolveMixerDevice(World))
                    {
                        USoundSubmix& Main = Device->GetMainSubmixObject();

                        // StopRecording writes NumChannels/SampleRate through
                        // out-params and returns the buffer by reference.
                        // Argument evaluation order is unspecified, so the call
                        // has to complete before the values are read.
                        float NumChannels = 0.0f;
                        float SampleRate = 0.0f;
                        const Audio::FAlignedFloatBuffer NullArgBuffer =
                            Device->StopRecording(nullptr, NumChannels, SampleRate);
                        bNullArgWorks =
                            ReportRecordedBuffer(TEXT("null_submix"), NullArgBuffer, NumChannels,
                                                 SampleRate);

                        NumChannels = 0.0f;
                        SampleRate = 0.0f;
                        const Audio::FAlignedFloatBuffer MainArgBuffer =
                            Device->StopRecording(&Main, NumChannels, SampleRate);
                        bExplicitArgWorks =
                            ReportRecordedBuffer(TEXT("main_submix"), MainArgBuffer, NumChannels,
                                                 SampleRate);

                        Device->UnregisterSubmixBufferListener(Listener, Main);
                    }
                    else
                    {
                        Emit(TEXT("failure"), TEXT("mixer_device_vanished_during_probe"));
                    }

                    Listener->Report();

                    const bool bSubmixRendered = Listener->SawNonSilentAudio();
                    Emit(TEXT("submix_renders"), bSubmixRendered ? TEXT("yes") : TEXT("no"));
                    Emit(TEXT("recorder_works_null_submix"), bNullArgWorks ? TEXT("yes") : TEXT("no"));
                    Emit(TEXT("recorder_works_main_submix"),
                         bExplicitArgWorks ? TEXT("yes") : TEXT("no"));

                    // The taps disagreeing is the interesting outcome: the mixer
                    // rendered audio and the API the plugin's Reference Audio
                    // path depends on did not see it.
                    if (!bSubmixRendered)
                    {
                        Emit(TEXT("failure"), TEXT("submix_did_not_render"));
                    }
                    else if (!bNullArgWorks && bExplicitArgWorks)
                    {
                        Emit(TEXT("failure"), TEXT("null_submix_arg_records_nothing_explicit_works"));
                    }
                    else if (!bNullArgWorks)
                    {
                        Emit(TEXT("failure"), TEXT("submix_rendered_but_recorder_returned_nothing"));
                    }

                    FinishProbe(bQuitWhenDone);
                    return false;
                }),
            DurationSeconds);
    }

    FAutoConsoleCommandWithWorldArgsAndOutputDevice GSubmixProbeCommand(
        TEXT("convai.tests.SubmixProbe"),
        TEXT("Issue 01: play a tone and report whether the master submix rendered it, measured "
             "both through StartRecordingOutput and through an independent submix listener. "
             "Args: [duration_seconds] [quit]"),
        FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&RunSubmixProbe));
}
