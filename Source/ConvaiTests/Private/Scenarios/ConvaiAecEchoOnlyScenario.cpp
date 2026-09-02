// Copyright 2022 Convai Inc. All Rights Reserved.

// Issue 05's paired AEC run — the echo-only window.
//
// The player says nothing. The character talks, its voice reaches the master
// submix, and Injected Echo puts a delayed, attenuated copy into the
// microphone. That is the acoustic situation of a player on speakers, and it is
// the reported symptom "character responds to its own voice" reduced to one
// measurement: does the character's own voice come back as the *player's*
// transcript?
//
// Paired on AECType, one process each. The Internal run must produce no player
// transcript in that window. The None run must produce one -- and issue 05 is
// explicit that if it does not, the finding is "fixture broken" rather than "AEC
// works", because a scenario whose failure mode cannot fire proves nothing. So
// the None run asserts the leak is present, and says so in those words when it
// is not.
//
// The character is made to talk with SendText rather than with speech, so the
// player's microphone carries only echo and there is no near-end signal to
// confuse the question.
//
// A third arm, `disabled`, is the same question asked of a second control.
// AECType=None leaves the client with no canceller at all; AEC=0 keeps
// AECType=Internal, so the APM is constructed and every mic chunk goes through
// it while cancellation is off. Both must leak. Two controls that fail the same
// way for different reasons is what separates "the canceller did nothing" from
// "the fixture reached nothing" -- the shape of mistake F18 was.

#include "ConvaiChatbotComponent.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiInjectedEcho.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiReferenceFeedMonitor.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestEventSink.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTestSteps.h"
#include "ConvaiTests.h"
#include "ConvaiSubsystem.h"
#include "ConvaiUtils.h"
#include "ConvaiVirtualMicComponent.h"

#include "AudioMixerDevice.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundSubmix.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    // Loud on purpose. The server's VAD gates on level (ConvaiVadParams defaults
    // min_volume to 0.6), so a quiet echo would be filtered out before the
    // canceller's behaviour could be observed and the run would credit AEC for
    // the VAD's work.
    constexpr float kEchoGain = 0.8f;
    constexpr float kEchoDelayMs = 120.0f;

    const TCHAR* kPrompt = TEXT("Please count slowly from one to fifteen, out loud.");

    // Both components broadcast on the same delegate shape with themselves as
    // Speaker, so the name is the only thing separating the player's half of a
    // transcript stream from the character's.
    const TCHAR* kPlayerComponentName = TEXT("ConvaiPlayer");
    const TCHAR* kBotComponentName = TEXT("ConvaiChatbot");

    // One arm of the pair, as data. AECEnabled is the `AEC` custom param and
    // AECType is `AECType`; they are separate knobs (ConvaiUtils.cpp:843) and
    // conflating them is what made the None run look like a control for the
    // reference feed when it never was (F27).
    struct FArm
    {
        const TCHAR* ScenarioName;
        const TCHAR* AECType;
        const TCHAR* AECEnabled;
        bool bExpectLeak;
        // A fixture name makes the player talk over the character instead of
        // staying silent -- issue 05's double-talk variant, and the in-engine
        // tier of AecDoubleTalk.NearEndSurvivesSimultaneousEcho in the DLL repo.
        // Null is the echo-only window.
        const TCHAR* NearEndFixture;
        // "1" is the shipping default for all three, and every arm here runs
        // the shipping config: the transcript oracle asks what the player
        // actually got back, which is a question about the whole APM rather
        // than about cancellation in isolation.
        const TCHAR* NoiseSuppression;
        const TCHAR* GainControl;
        const TCHAR* HighPassFilter;
    };

    constexpr FArm kArms[] = {
        // Shipping config. These measure the reported symptom.
        {TEXT("aec_echo_only_internal"), TEXT("Internal"), TEXT("1"), false, nullptr,
         TEXT("1"), TEXT("1"), TEXT("1")},
        {TEXT("aec_echo_only_none"), TEXT("None"), TEXT("1"), true, nullptr,
         TEXT("1"), TEXT("1"), TEXT("1")},
        {TEXT("aec_echo_only_disabled"), TEXT("Internal"), TEXT("0"), true, nullptr,
         TEXT("1"), TEXT("1"), TEXT("1")},
        {TEXT("aec_double_talk"), TEXT("Internal"), TEXT("1"), false, TEXT("S1"),
         TEXT("1"), TEXT("1"), TEXT("1")},

        // No erle_* arms here. Measuring what the canceller removed needs the
        // residual, and the residual only exists inside convai_client -- the
        // header this branch builds against exposes no counters at all. That
        // measurement is the library's half of ADR-0004 and lives in
        // convai-livekit-cpp-p's offline suite (issue 06), where it reads 40 to
        // 49 dB with no engine and no network. F33 also measured that the
        // in-engine figure could not grade the canceller anyway: a seeded defect
        // that took the offline suite from 0 to 3 failures left it at 2.45 dB,
        // inside the healthy range, because in-engine that ~3 dB is AEC3's
        // residual suppressor rather than subtraction.
    };

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
}

class FConvaiAecEchoOnlyScenario : public FConvaiTestScenario
{
public:
    explicit FConvaiAecEchoOnlyScenario(const FArm& InArm)
        : Arm(InArm)
    {
    }

    virtual const TCHAR* Name() const override { return Arm.ScenarioName; }
    virtual double DeadlineSeconds() const override { return 120.0; }
    virtual bool RequiresLiveConnection() const override { return true; }

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

        UConvaiUtils::SetCustomParam(TEXT("AECType"), Arm.AECType);
        UConvaiUtils::SetCustomParam(TEXT("AEC"), Arm.AECEnabled);
        UConvaiUtils::SetCustomParam(TEXT("NoiseSuppression"), Arm.NoiseSuppression);
        UConvaiUtils::SetCustomParam(TEXT("GainControl"), Arm.GainControl);
        UConvaiUtils::SetCustomParam(TEXT("HighPassFilter"), Arm.HighPassFilter);
        // What resolved, not what was asked for: a command-line -AECType wins
        // over SetCustomParam (ConvaiUtils.cpp:155) and a run configured by the
        // orchestrator would otherwise be recorded as the arm it was not.
        Recorder->Record(TEXT("aec_type"), UConvaiUtils::GetAECType());
        Recorder->Record(TEXT("aec_enabled"),
                         UConvaiUtils::IsAECEnabled() ? TEXT("1") : TEXT("0"));
        Recorder->Record(TEXT("apm_config"),
                         FString::Printf(TEXT("NS=%d AGC=%d HPF=%d"),
                                         UConvaiUtils::IsNoiseSuppressionEnabled() ? 1 : 0,
                                         UConvaiUtils::IsGainControlEnabled() ? 1 : 0,
                                         UConvaiUtils::IsHighPassFilterEnabled() ? 1 : 0));

        CharacterID = UConvaiUtils::GetTestCharacterID();
        if (CharacterID.IsEmpty() || UConvaiUtils::GetAPI_Key().IsEmpty())
        {
            Recorder->Record(TEXT("setup_failed"), TEXT("no test character or no API key"));
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

        UConvaiPlayerComponent* Player =
            NewObject<UConvaiPlayerComponent>(SpawnedOwner, TEXT("ConvaiPlayer"));
        Player->RegisterComponent();
        PlayerComponent = Player;

        UConvaiChatbotComponent* Chatbot =
            NewObject<UConvaiChatbotComponent>(SpawnedOwner, TEXT("ConvaiChatbot"));
        Chatbot->CharacterID = CharacterID;
        Chatbot->RegisterComponent();
        ChatbotComponent = Chatbot;

        Sink.Reset(NewObject<UConvaiTestEventSink>());
        Player->OnTranscriptionReceivedDelegate.AddDynamic(
            Sink.Get(), &UConvaiTestEventSink::HandleTranscription);
        Chatbot->OnFailureEvent.AddDynamic(Sink.Get(), &UConvaiTestEventSink::HandleFailure);

        FConvaiInjectedEcho::FParams EchoParams;
        EchoParams.DelayMs = kEchoDelayMs;
        EchoParams.Gain = kEchoGain;
        EchoParams.TargetSampleRate = UConvaiVirtualMicComponent::CaptureSampleRate();
        Echo = MakeShared<FConvaiInjectedEcho, ESPMode::ThreadSafe>(EchoParams);
        MixerDevice->RegisterSubmixBufferListener(Echo.ToSharedRef(),
                                                  MixerDevice->GetMainSubmixObject());
        Mic->SetEchoSource(Echo);

        Chatbot->StartSession();
        ConvaiTestSteps::SetTalkTargets(Player, {Chatbot});

        // F26 asks whether the run that leaks is the run whose Reference Audio
        // died. The two have never been measured in the same process, so the
        // monitor rides along and every run records the feed's health next to
        // its leak count. Its capture-ratio figures are meaningless here -- F17,
        // a live Connection owns the one global recorder -- but the connection
        // deficit and the wall-clock cadence are exactly F1 and F4, and those
        // are the candidates.
        Monitor.Start(World, Recorder, ConnectTimeoutSeconds + PromptTimeoutSeconds +
                                           EchoWindowSeconds);
        Recorder->Record(TEXT("session_started"), CharacterID.Left(8));
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }
        Elapsed += DeltaSeconds;
        Monitor.Tick(DeltaSeconds);

        if (!bPrompted)
        {
            if (!IsConnected())
            {
                return Elapsed >= ConnectTimeoutSeconds;
            }
            bPrompted = true;
            ConnectedAtSeconds = Elapsed;

            if (UConvaiPlayerComponent* Player = PlayerComponent.Get())
            {
                // The microphone streams for the whole window and carries only
                // echo. The player never speaks; the queue is silence so the
                // synth keeps rendering and the echo has something to mix into.
                Player->UnmuteStreamingAudio();
            }
            ConvaiTestSteps::Silence(VirtualMic.Get(), EchoWindowSeconds + 5.0f);
            VirtualMic->Start();
            ConvaiTestSteps::SayText(PlayerComponent.Get(), ChatbotComponent.Get(), kPrompt);
            Recorder->Record(TEXT("prompted"), kPrompt);
            return false;
        }

        // The window opens when the character actually starts producing audio,
        // not when it was asked to: everything before that has no echo in it and
        // counting transcripts there would measure the prompt round trip.
        if (!bCharacterSpoke)
        {
            if (UConvaiChatbotComponent* Chatbot = ChatbotComponent.Get())
            {
                if (Chatbot->GetIsTalking())
                {
                    bCharacterSpoke = true;
                    SpeechStartedAtSeconds = Elapsed;
                    TranscriptsAtSpeechStart = Sink.IsValid() ? Sink->Transcripts().Num() : 0;
                    // Opened here rather than at Start for the same reason the
                    // transcript count is: the seconds before the character has
                    // any voice in the room contain no echo, and cancellation
                    // measured across them is diluted by silence the canceller
                    // was never asked to do anything with.
                    if (UConvaiVirtualMicComponent* Mic = VirtualMic.Get())
                    {
                        MicEnergyAtWindowOpen = Mic->EmittedEnergy();
                        MicSamplesAtWindowOpen = Mic->EmittedSamples();

                        // Queued now, not at prompt time: double-talk means the
                        // player's voice and the echo overlapping, and the echo
                        // does not exist until this instant.
                        if (Arm.NearEndFixture)
                        {
                            FString Error;
                            bNearEndQueued =
                                ConvaiTestSteps::SpeakWav(Mic, Arm.NearEndFixture, Error);
                            Recorder->Record(TEXT("near_end_fixture"),
                                             bNearEndQueued ? Arm.NearEndFixture : *Error);
                        }
                    }
                    Recorder->Record(TEXT("character_talking"),
                                     FString::Printf(TEXT("%.1fs"), Elapsed));
                }
            }
            return Elapsed >= ConnectedAtSeconds + PromptTimeoutSeconds;
        }

        return Elapsed >= SpeechStartedAtSeconds + EchoWindowSeconds;
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
        }
        if (VirtualMic.IsValid())
        {
            VirtualMic->SetEchoSource(nullptr);
        }
        if (UConvaiPlayerComponent* Player = PlayerComponent.Get())
        {
            Player->MuteStreamingAudio();
        }

        const TArray<UConvaiTestEventSink::FTranscript> All =
            Sink.IsValid() ? Sink->Transcripts() : TArray<UConvaiTestEventSink::FTranscript>();

        // Only what arrived after the character's voice was in the room, and
        // only non-empty text -- F22 says an empty final follows every real one
        // and counting those would inflate every run equally.
        int32 LeakedTranscripts = 0;
        FString LeakedText;
        for (int32 i = TranscriptsAtSpeechStart; i < All.Num(); ++i)
        {
            if (!All[i].Text.IsEmpty())
            {
                ++LeakedTranscripts;
                if (LeakedText.IsEmpty())
                {
                    LeakedText = All[i].Text;
                }
            }
        }

        const int64 EchoSource = Echo.IsValid() ? Echo->NonSilentSourceSamples() : 0;
        const float EchoPeak = Echo.IsValid() ? Echo->PeakInjected() : 0.0f;

        Result.Metrics.Add(TEXT("connected"), bPrompted ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("character_spoke"), bCharacterSpoke ? 1.0 : 0.0);

        // -1 means the hint was never called, which is distinct from a hint of 0.
        const FString StreamDelayStr = UConvaiUtils::GetAECStreamDelayMs();
        Result.Metrics.Add(TEXT("aec_stream_delay_ms"),
                           StreamDelayStr.IsEmpty() ? -1.0 : FCString::Atod(*StreamDelayStr));

        // The counters the canceller keeps about itself. aec_stream_parity is
        // the whole point: mic and reference chunks over the same window must
        // agree, because streams that drift apart cannot be cancelled however
        // good the filter is.
        if (const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr)
        {
            if (const UConvaiSubsystem* Subsystem = GameInstance->GetSubsystem<UConvaiSubsystem>())
            {
                const UConvaiSubsystem::FConvaiAECStats Aec = Subsystem->GetAECStats();
                Result.Metrics.Add(TEXT("aec_stats_available"), Aec.bValid ? 1.0 : 0.0);
                if (Aec.bValid)
                {
                    Result.Metrics.Add(TEXT("aec_mic_chunks"), static_cast<double>(Aec.MicChunks));
                    Result.Metrics.Add(TEXT("aec_ref_chunks"), static_cast<double>(Aec.ReferenceChunks));
                    Result.Metrics.Add(TEXT("aec_ref_calls"), static_cast<double>(Aec.ReferenceCalls));
                    Result.Metrics.Add(TEXT("aec_published_frames"), static_cast<double>(Aec.PublishedFrames));
                    Result.Metrics.Add(TEXT("aec_dropped_frames"), static_cast<double>(Aec.DroppedFrames));
                    Result.Metrics.Add(TEXT("aec_queue_depth"), static_cast<double>(Aec.QueueDepth));
                    Result.Metrics.Add(TEXT("aec_stream_parity"),
                                       Aec.ReferenceChunks > 0
                                           ? static_cast<double>(Aec.MicChunks) / static_cast<double>(Aec.ReferenceChunks)
                                           : 0.0);
                    const double AttenDb = (Aec.LastRmsIn > 1.0 && Aec.LastRmsOut > 1.0)
                                               ? 20.0 * FMath::LogX(10.0, Aec.LastRmsIn / Aec.LastRmsOut)
                                               : 0.0;
                    Result.Metrics.Add(TEXT("aec_last_atten_db"), AttenDb);
                }
            }
        }

        if (UConvaiPlayerComponent* Player = PlayerComponent.Get())
        {
            int32 TapChannels = 0, TapRate = 0;
            int64 TapChunks = 0;
            Player->GetMicTapFormat(TapChannels, TapRate, TapChunks);
            Result.Metrics.Add(TEXT("mic_tap_channels"), static_cast<double>(TapChannels));
            Result.Metrics.Add(TEXT("mic_tap_sample_rate"), static_cast<double>(TapRate));
            Result.Metrics.Add(TEXT("mic_tap_chunks_routed"), static_cast<double>(TapChunks));
        }

        Result.Metrics.Add(TEXT("mic_capture_tap_is_listener"),
                           UConvaiUtils::GetMicCaptureTap().Equals(TEXT("Recorder"), ESearchCase::IgnoreCase) ? 0.0 : 1.0);
        Result.Metrics.Add(TEXT("echo_source_nonsilent_samples"), static_cast<double>(EchoSource));
        Result.Metrics.Add(TEXT("echo_peak_amplitude"), EchoPeak);

        // What the Virtual Mic actually put on the wire, against what the
        // put on the wire over the echo window, scaled to int16 counts. The
        // canceller's own view of the same signal is not available here, so
        // contamination is caught at its source instead -- see
        // default_capture_active below.
        double MicWindowRms = 0.0;
        if (UConvaiVirtualMicComponent* Mic = VirtualMic.Get())
        {
            const double WindowEnergy = Mic->EmittedEnergy() - MicEnergyAtWindowOpen;
            const int64 WindowSamples = Mic->EmittedSamples() - MicSamplesAtWindowOpen;
            MicWindowRms = WindowSamples > 0
                               ? FMath::Sqrt(FMath::Max(0.0, WindowEnergy) /
                                             static_cast<double>(WindowSamples)) *
                                     32768.0
                               : 0.0;
            Result.Metrics.Add(TEXT("mic_window_rms"), MicWindowRms);
            Result.Metrics.Add(TEXT("mic_emitted_rms"), Mic->EmittedRms() * 32768.0);
            Result.Metrics.Add(TEXT("mic_emitted_peak"), Mic->EmittedPeak());
            Result.Metrics.Add(TEXT("mic_emitted_samples"),
                               static_cast<double>(Mic->EmittedSamples()));
        }

        // Whether the plugin's own capture component is still live. It is
        // created and registered by UConvaiPlayerComponent::OnComponentCreated
        // and auto-activates there, which opens the host's capture device; if
        // it survives adoption it renders the host's microphone into the same
        // submix this scenario's Virtual Mic uses. Found by class name because
        // UConvaiAudioCaptureComponent is UCLASS() without CONVAI_API, so
        // another module cannot reference its StaticClass().
        int32 DefaultCaptureActive = 0;
        if (AActor* OwnerActor = Owner.Get())
        {
            for (const UActorComponent* Component : OwnerActor->GetComponents())
            {
                if (Component &&
                    Component->GetClass()->GetName() == TEXT("ConvaiAudioCaptureComponent") &&
                    Component->IsActive())
                {
                    ++DefaultCaptureActive;
                }
            }
        }
        Result.Metrics.Add(TEXT("default_capture_active"), DefaultCaptureActive);
        Result.Metrics.Add(TEXT("transcripts_total"), All.Num());
        Result.Metrics.Add(TEXT("transcripts_during_echo"), LeakedTranscripts);
        Result.Metrics.Add(TEXT("failures"),
                           Sink.IsValid() ? Sink->FailureCount() : 0);

        // Issue 16. Labelling what this scenario cannot see, because an agent
        // looping on the report will otherwise read a passing run as evidence
        // the canceller is healthy.
        Result.MetricNotes.Add(
            TEXT("transcripts_during_echo"),
            TEXT("Covers: the reported symptom -- the character's own voice coming back as the "
                 "player's transcript, against two controls that must leak. Does NOT cover: how "
                 "much the canceller removed. That needs the residual, which exists only inside "
                 "convai_client and which the header this branch builds against does not expose. "
                 "A change to the canceller (convai-livekit-cpp-p) is graded by "
                 "tests/aec_erle_test.cpp offline (issue 06), never by this scenario."));

        // Reported on every run, leaking or not, because the correlation is the
        // point and it needs both columns. The monitor's own capture ratio is
        // deliberately absent: with a live Connection it measures F17's
        // contention rather than the feed. feed_capture_ratio below is the same
        // quantity taken from inside the capture loop, where the contention
        // does not exist.
        if (Monitor.IsStarted())
        {
            const FConvaiReferenceFeedMonitor::FReport Ref = Monitor.Finish();
            Result.Metrics.Add(TEXT("ref_aec_enabled"), Ref.bAecEnabled ? 1.0 : 0.0);
            Result.Metrics.Add(TEXT("ref_max_connection_deficit"), Ref.MaxConnectionDeficit);
            Result.Metrics.Add(TEXT("ref_first_deficit_at_s"), Ref.FirstDeficitAtSeconds);
            Result.Metrics.Add(TEXT("ref_wall_gap_max_ms"), Ref.WallGapMaxMs);
            Result.Metrics.Add(TEXT("ref_wall_gap_stddev_ms"), Ref.WallGapStdDevMs);
            Result.Metrics.Add(TEXT("ref_audio_gap_max_ms"), Ref.AudioClockGapMaxMs);
            Result.Metrics.Add(TEXT("ref_rendered_peak"), Ref.RenderedPeak);
            Result.Metrics.Add(TEXT("ref_rendered_buffers"), Ref.RenderedBuffers);
            Result.Metrics.Add(TEXT("ref_recorder_contended"), Ref.bRecorderContended ? 1.0 : 0.0);

            // Issue 13. Measured inside the capture loop, so unlike everything
            // above these describe the plugin's behaviour rather than the audio
            // device's.
            //
            // Their control is not this scenario's None run: AECType and AEC
            // are separate custom params (ConvaiUtils.cpp:843), so None selects
            // a null canceller while the feed keeps running, and both sides
            // report a live feed. The zero control is reference_feed_capture on
            // a map with no Connection, where no capture thread exists and
            // every one of these must read zero.
            FConvaiReferenceFeedMonitor::AddFeedMetrics(Result.Metrics, Ref.Feed, TEXT("feed_"));
        }

        if (!bPrompted)
        {
            Result.bPassed = false;
            Result.FailReason = FString::Printf(
                TEXT("no Connection reached Connected within %.0f s"), ConnectTimeoutSeconds);
            return Result;
        }

        if (!bCharacterSpoke)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("the character never started talking, so no echo existed and neither run's "
                     "assertion has a subject");
            return Result;
        }

        // The fixture self-check the PRD names. Without it a dead audio path
        // satisfies the Internal run's assertion perfectly.
        //
        // The floor is a level, not zero. Written as `EchoPeak <= 0` this passed
        // a run whose echo peaked at 8.9e-06 -- about -101 dB, which is the
        // Virtual Mic's own output returning through MuteMic (F18) and no
        // character audio whatsoever. That run reported 126 leaked transcripts
        // and they were F24 hallucinations on a silent stream, not echo. A
        // genuine echo of this character measures 0.176 at gain 0.8, so 0.01
        // sits three orders of magnitude clear of the residue and one clear of
        // the signal.
        constexpr float MinAudibleEchoPeak = 0.01f;
        if (EchoSource <= 0 || EchoPeak < MinAudibleEchoPeak)
        {
            Result.bPassed = false;
            Result.FailReason = FString::Printf(
                TEXT("Injected Echo carried nothing audible (%lld non-silent source samples, peak "
                     "%.4g against a %.2f floor), so the microphone was clean. For the Internal "
                     "run 'no leak' would be true for the wrong reason; for the None run any "
                     "transcripts counted are hallucinations on silence (F24), not echo."),
                EchoSource, EchoPeak, MinAudibleEchoPeak);
            return Result;
        }

        // The fixture check the echo floor above cannot make. That one asks
        // whether the microphone carried enough; this asks whether it carried
        // only what the fixture put there. Both halves are needed: a run whose
        // microphone is picking up the room has a perfectly healthy echo peak
        // and every cancellation number on it is about the wrong audio.
        //
        // An RMS cannot exceed the peak of the signal it was taken over. So the
        // loudest second the canceller saw cannot be louder than the loudest
        // single sample the Virtual Mic ever emitted, and if it is, the capture
        // path is carrying something the fixture did not put there.
        //
        // Stated as an invariant rather than as a ratio on purpose. The obvious
        // check -- the canceller's window RMS against the microphone's window
        // RMS -- reads between 1.35 and 3.79 across runs that are all fine,
        // because the two windows close at slightly different instants on a
        // signal that is bursts of echo separated by silence. That spread is
        // wider than some real contamination. This comparison has no window in
        // it at all: both sides are extrema over the whole run.
        //
        // The run this was built for measured a block RMS of 10862 against a
        // microphone whose loudest sample for the run was 0.168, i.e. 5505.
        // Clean runs sit around a quarter of the bound.
        if (DefaultCaptureActive > 0)
        {
            Result.bPassed = false;
            Result.FailReason = FString::Printf(
                TEXT("%d default capture component(s) were still active alongside the Virtual "
                     "Mic. Both render into /ConvAI/Submixes/AudioInput, so the host's real "
                     "microphone was summed into the fixture's signal and sent to the server: "
                     "no transcript on this run is about the audio the fixture emitted."),
                DefaultCaptureActive);
            return Result;
        }

        if (Arm.NearEndFixture)
        {
            return FinishDoubleTalk(Result, All, EchoSource, EchoPeak);
        }

        if (Arm.bExpectLeak)
        {
            // The negative control for the pair. AEC is off, so the character's
            // own voice should come back as the player's transcript.
            Result.bPassed = LeakedTranscripts > 0;
            if (!Result.bPassed)
            {
                Result.FailReason = FString::Printf(
                    TEXT("with AECType=%s AEC=%s and %lld non-silent samples of echo at peak %.3g, "
                         "no player transcript came back. The fixture is broken, not the canceller: "
                         "this run is what proves the Internal run can fail, and it did not. "
                         "Suspect the server's VAD gating the echo below min_volume, or the echo "
                         "not reaching the send path."),
                    Arm.AECType, Arm.AECEnabled, EchoSource, EchoPeak);
            }
            else
            {
                Recorder->Record(TEXT("leaked_transcript"), LeakedText);
            }
            return Result;
        }

        if (LeakedTranscripts > 0)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("character-hears-its-own-voice");
            Finding.Summary =
                TEXT("with echo cancellation enabled, the character's own speech came back as the "
                     "player's transcript, so the character is talking to itself");
            Finding.Evidence = FString::Printf(
                TEXT("AECType=Internal. The character spoke, Injected Echo carried %lld non-silent "
                     "samples at peak %.3g back into the microphone with %.0f ms delay and %.2f "
                     "gain, and %d non-empty player transcript(s) arrived in the %.0f s after the "
                     "character started talking. First was: \"%s\""),
                EchoSource, EchoPeak, kEchoDelayMs, kEchoGain, LeakedTranscripts,
                EchoWindowSeconds, *LeakedText);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.bPassed = false;
            Result.FailReason = TEXT("echo leaked into the player transcript with AEC enabled");
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

private:
    // Issue 05's double-talk variant. The echo-only arms ask whether the
    // character's voice got through; this one asks the opposite question, and
    // it is the dangerous one: an agent looping on the echo leak can "fix" it by
    // suppressing harder and destroy the player's speech (F12) with nothing
    // noticing. The DLL has the offline case as
    // AecDoubleTalk.NearEndSurvivesSimultaneousEcho; this is the in-engine tier.
    //
    // transcripts_during_echo means something different here and is not an
    // assertion: the player IS talking, so transcripts are expected, and
    // separating the player's words from the character's needs the text rather
    // than the count.
    FConvaiScenarioResult FinishDoubleTalk(FConvaiScenarioResult& Result,
                                           const TArray<UConvaiTestEventSink::FTranscript>& All,
                                           int64 EchoSource, float EchoPeak)
    {
        if (!bNearEndQueued)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("the near-end fixture never loaded, so there was no player speech to "
                     "preserve and this run says nothing about over-suppression");
            return Result;
        }

        const FString Expected = ConvaiTestSteps::ExpectedTranscript(Arm.NearEndFixture);

        // Filtered by speaker. This loop used to take the last non-empty final
        // from *either* component, so the character's own transcript could be
        // scored as the player's -- and during double-talk it usually is the
        // later of the two. That made this arm's WER a coin toss between the two
        // speakers and is the likeliest source of its flakiness: a failing run
        // observed 2026-08-20 had near_end_transcript_empty=0 with WER 1.0,
        // which is what scoring the character's words against the player's
        // fixture looks like.
        FString Best;
        FString BotText;
        for (int32 i = TranscriptsAtSpeechStart; i < All.Num(); ++i)
        {
            if (!All[i].bFinal || All[i].Text.IsEmpty())
            {
                continue;
            }
            if (All[i].Speaker == kPlayerComponentName)
            {
                Best = All[i].Text;
            }
            else if (All[i].Speaker == kBotComponentName)
            {
                BotText = All[i].Text;
            }
        }

        const double Wer = ConvaiTestSteps::WordErrorRate(Expected, Best);
        // Which speaker the player's transcript actually resembles. F26's leak
        // and F12's over-suppression both land here as "the player's words did
        // not come back", and the fix for one is the opposite of the fix for the
        // other, so the finding has to say which it saw.
        const double WerAgainstBot = BotText.IsEmpty()
                                         ? 1.0
                                         : ConvaiTestSteps::WordErrorRate(BotText, Best);
        Result.Metrics.Add(TEXT("near_end_wer"), Wer);
        Result.Metrics.Add(TEXT("near_end_wer_against_character"), WerAgainstBot);
        Result.Metrics.Add(TEXT("near_end_transcript_empty"), Best.IsEmpty() ? 1.0 : 0.0);
        Recorder->Record(TEXT("near_end_transcript"), Best);
        Recorder->Record(TEXT("near_end_expected"), Expected);
        Recorder->Record(TEXT("character_transcript"), BotText);

        Result.MetricNotes.Add(
            TEXT("near_end_wer"),
            TEXT("Covers: whether the player's own words came back on the player's transcript "
                 "path, scored only against transcripts whose Speaker is the player component. "
                 "Does NOT cover: which defect destroyed them. Read it beside "
                 "near_end_wer_against_character -- a player transcript that matches what the "
                 "character said is F26's leak, not F12's over-suppression, and they have "
                 "opposite fixes."));

        // Word error rate is normally reported and never asserted (issue 04),
        // because a threshold on it fails when the recogniser's model changes
        // rather than when the plugin breaks. This threshold is not in that
        // regime: F12 measured over-suppression annihilating whole quarters of
        // the utterance to -74 dB, and what survives that is not a degraded
        // transcript but no transcript. A WER at or above 1.0 means nothing the
        // player said came back.
        // The leak, not the destruction. A non-empty player transcript that is
        // closer to what the character said than to what the player said is the
        // character's voice arriving on the player's path -- F26 measured during
        // double-talk. Reported under its own key so an agent looping on this
        // scenario cannot "fix" it by suppressing harder, which is the exact
        // trade this arm exists to catch.
        if (!Best.IsEmpty() && Wer >= 1.0 && WerAgainstBot < Wer)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("character-voice-returned-as-player-during-double-talk");
            Finding.Summary =
                TEXT("during double-talk the player's transcript came back carrying the "
                     "character's words rather than the player's, so the echo leaked onto the "
                     "player's path while the player was speaking");
            Finding.Evidence = FString::Printf(
                TEXT("AECType=%s AEC=%s. Fixture %s played into the microphone while the "
                     "character spoke. The player's transcript came back as \"%s\", which scores "
                     "%.2f against the player's expected \"%s\" and %.2f against the character's "
                     "own \"%s\"."),
                Arm.AECType, Arm.AECEnabled, Arm.NearEndFixture, *Best, Wer, *Expected,
                WerAgainstBot, *BotText);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.bPassed = false;
            Result.FailReason =
                TEXT("the player's transcript carried the character's words, not the player's");
            return Result;
        }

        if (Best.IsEmpty() || Wer >= 1.0)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("player-speech-destroyed-during-double-talk");
            Finding.Summary =
                TEXT("with the character talking at the same time, the player's speech did not "
                     "come back from the server, so echo cancellation is removing the near end "
                     "along with the echo");
            Finding.Evidence = FString::Printf(
                TEXT("AECType=%s AEC=%s. Injected Echo carried %lld non-silent samples at peak "
                     "%.3g while fixture %s played into the microphone, which emitted RMS %.0f "
                     "over the window. The player's transcript came back as \"%s\" against an "
                     "expected \"%s\", a word error rate of %.2f."),
                Arm.AECType, Arm.AECEnabled, EchoSource, EchoPeak, Arm.NearEndFixture,
                Result.Metrics.FindRef(TEXT("mic_window_rms")), *Best, *Expected, Wer);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.bPassed = false;
            Result.FailReason =
                TEXT("the player's speech did not survive simultaneous echo cancellation");
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

    bool IsConnected() const
    {
        const UConvaiChatbotComponent* Chatbot = ChatbotComponent.Get();
        const UConvaiConnectionSessionProxy* Proxy = Chatbot ? Chatbot->GetSessionProxy() : nullptr;
        return Proxy && Proxy->GetConnectionState() == EC_ConnectionState::Connected;
    }

    static constexpr float ConnectTimeoutSeconds = 30.0f;
    static constexpr float PromptTimeoutSeconds = 30.0f;
    static constexpr float EchoWindowSeconds = 20.0f;

    const FArm Arm;

    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    FString CharacterID;

    TWeakObjectPtr<AActor> Owner;
    TWeakObjectPtr<UConvaiVirtualMicComponent> VirtualMic;
    TWeakObjectPtr<UConvaiPlayerComponent> PlayerComponent;
    TWeakObjectPtr<UConvaiChatbotComponent> ChatbotComponent;
    TStrongObjectPtr<UConvaiTestEventSink> Sink;
    TSharedPtr<FConvaiInjectedEcho, ESPMode::ThreadSafe> Echo;
    FConvaiReferenceFeedMonitor Monitor;

    bool bSetupFailed = false;
    bool bPrompted = false;
    bool bCharacterSpoke = false;
    float Elapsed = 0.0f;
    float ConnectedAtSeconds = 0.0f;
    float SpeechStartedAtSeconds = 0.0f;
    int32 TranscriptsAtSpeechStart = 0;
    double MicEnergyAtWindowOpen = 0.0;
    int64 MicSamplesAtWindowOpen = 0;
    bool bNearEndQueued = false;
};

static struct FConvaiAecEchoOnlyRegistrar
{
    FConvaiAecEchoOnlyRegistrar()
    {
        for (const FArm& Arm : kArms)
        {
            ConvaiTestRegistry::Register(Arm.ScenarioName,
                                         [&Arm]() -> TSharedRef<FConvaiTestScenario>
                                         { return MakeShared<FConvaiAecEchoOnlyScenario>(Arm); });
        }
    }
} GConvaiAecEchoOnlyRegistrar;
