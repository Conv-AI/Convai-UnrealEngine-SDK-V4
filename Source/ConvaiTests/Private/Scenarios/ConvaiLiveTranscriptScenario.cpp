// Copyright 2022 Convai Inc. All Rights Reserved.

// Issue 04's live half, and issue 05's paired AEC run.
//
// speak_wav_through_virtual_mic proves scripted speech reaches the capture
// submix. This carries it the rest of the way: through the plugin's resample and
// send path, to a real character, and back as a transcript. Word error rate is
// reported and never asserted -- issue 04 keeps ASR quality out of the pass
// condition because a threshold there fails when the model changes rather than
// when the plugin breaks.
//
// Registered three times, differing only in AECType, which
// UConvaiUtils::SetCustomParam sets before the session starts because the
// subsystem reads it at connect time (ConvaiSubsystem.cpp:417). One scenario per
// process makes that clean: each run sets the value once, in its own engine, and
// nothing has to be put back.
//
// Costs a live Connection and backend time. Everything it can answer offline is
// answered offline by the scenarios above it.

#include "ConvaiChatbotComponent.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestEventSink.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTestSteps.h"
#include "ConvaiTests.h"
#include "ConvaiUtils.h"
#include "ConvaiVirtualMicComponent.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    const TCHAR* kFixture = TEXT("S1");

    // The speaker this scenario is about. Both halves of a conversation
    // broadcast on the same delegate, so a check on "the last final
    // transcript" is satisfied by whichever side spoke last -- and since the
    // character's answer became a non-empty final (F23), that can be the
    // character. This scenario asserts that the *player's* speech came back;
    // the name it filters on is what makes the assertion say so.
    const TCHAR* kPlayerComponentName = TEXT("ConvaiPlayer");
}

class FConvaiLiveTranscriptScenario : public FConvaiTestScenario
{
public:
    explicit FConvaiLiveTranscriptScenario(const TCHAR* InAECType = nullptr)
        : AECType(InAECType)
    {
    }

    static const TCHAR* StaticName() { return TEXT("live_player_transcript"); }
    static const TCHAR* NameForAEC(const TCHAR* InAECType)
    {
        static const FString Internal = TEXT("live_player_transcript_aec_internal");
        static const FString None = TEXT("live_player_transcript_aec_none");
        return FCString::Stricmp(InAECType, TEXT("None")) == 0 ? *None : *Internal;
    }

    virtual const TCHAR* Name() const override
    {
        return AECType ? NameForAEC(AECType) : StaticName();
    }

    // Generous: a handshake, an utterance, and however long the server takes to
    // decide the utterance ended. ConvaiVadParams defaults stop_secs to 2.2.
    virtual double DeadlineSeconds() const override { return 90.0; }
    virtual bool RequiresLiveConnection() const override { return true; }

    virtual void Start(UWorld* InWorld, FConvaiTestEventRecorder& InRecorder) override
    {
        World = InWorld;
        Recorder = &InRecorder;

        if (!World)
        {
            Recorder->Record(TEXT("setup_failed"), TEXT("no world"));
            bSetupFailed = true;
            return;
        }

        // Before the session starts, because the subsystem reads it at connect.
        if (AECType)
        {
            UConvaiUtils::SetCustomParam(TEXT("AECType"), AECType);
            Recorder->Record(TEXT("aec_type"), AECType);
        }

        CharacterID = UConvaiUtils::GetTestCharacterID();
        if (CharacterID.IsEmpty())
        {
            Recorder->Record(TEXT("setup_failed"),
                             TEXT("no test character; set TestCharacterID in "
                                  "[/Script/Convai.ConvaiSettings] or pass "
                                  "-ConvaiTestCharacterID="));
            bSetupFailed = true;
            return;
        }
        if (UConvaiUtils::GetAPI_Key().IsEmpty())
        {
            Recorder->Record(TEXT("setup_failed"), TEXT("no Convai API key configured"));
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
        Player->OnTranscriptionReceivedDelegate.AddDynamic(Sink.Get(),
                                                           &UConvaiTestEventSink::HandleTranscription);
        Chatbot->OnFailureEvent.AddDynamic(Sink.Get(), &UConvaiTestEventSink::HandleFailure);

        Chatbot->StartSession();
        ConvaiTestSteps::SetTalkTargets(Player, {Chatbot});
        Recorder->Record(TEXT("session_started"), CharacterID.Left(8));
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }
        Elapsed += DeltaSeconds;

        if (!bSpeaking)
        {
            if (!IsConnected())
            {
                // Give up waiting rather than burn the whole deadline on a
                // handshake that is not coming; the report then says "never
                // connected" instead of "watchdog".
                return Elapsed >= ConnectTimeoutSeconds;
            }

            bSpeaking = true;
            ConnectedAtSeconds = Elapsed;
            Recorder->Record(TEXT("connected"), FString::Printf(TEXT("%.1fs"), Elapsed));

            if (UConvaiPlayerComponent* Player = PlayerComponent.Get())
            {
                Player->UnmuteStreamingAudio();
            }
            FString Error;
            if (!ConvaiTestSteps::SpeakWav(VirtualMic.Get(), kFixture, Error))
            {
                Recorder->Record(TEXT("speak_failed"), Error);
                bSpeakFailed = true;
                return true;
            }
            // Keeps the stream alive past the utterance so the server's VAD sees
            // an end rather than a cut connection.
            ConvaiTestSteps::Silence(VirtualMic.Get(), TrailingSilenceSeconds);
            VirtualMic->Start();
            Recorder->Record(TEXT("speaking"), kFixture);
            return false;
        }

        // Done as soon as a final transcript exists; otherwise wait out the
        // window, because "no transcript" is the finding rather than a timeout.
        if (Sink.IsValid() && !Sink->LatestFinalTextFrom(kPlayerComponentName).IsEmpty())
        {
            return true;
        }
        return Elapsed >= ConnectedAtSeconds + TranscriptWindowSeconds;
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

        if (UConvaiPlayerComponent* Player = PlayerComponent.Get())
        {
            Player->MuteStreamingAudio();
        }

        const FString Expected = ConvaiTestSteps::ExpectedTranscript(kFixture);
        const FString Actual =
            Sink.IsValid() ? Sink->LatestFinalTextFrom(kPlayerComponentName) : FString();
        const TArray<UConvaiTestEventSink::FTranscript> All =
            Sink.IsValid() ? Sink->Transcripts() : TArray<UConvaiTestEventSink::FTranscript>();
        const int32 Failures = Sink.IsValid() ? Sink->FailureCount() : 0;

        Result.Metrics.Add(TEXT("connected"), bSpeaking ? 1.0 : 0.0);
        // Seconds here, milliseconds in the key: every other scenario times this
        // through FConvaiTestLatencyTracker, which records milliseconds, and one
        // name carrying two units across scenarios is worse than either unit.
        Result.Metrics.Add(TEXT("connect_ms"), bSpeaking ? ConnectedAtSeconds * 1000.0 : -1.0);
        Result.Metrics.Add(TEXT("transcripts"), All.Num());
        Result.Metrics.Add(TEXT("distinct_speakers"),
                           Sink.IsValid() ? Sink->Speakers().Num() : 0);
        Result.Metrics.Add(TEXT("failures"), Failures);
        Result.Metrics.Add(TEXT("mic_emitted_samples"),
                           static_cast<double>(VirtualMic.IsValid() ? VirtualMic->EmittedSamples()
                                                                    : 0));

        if (bSpeakFailed)
        {
            Result.bPassed = false;
            Result.FailReason = TEXT("SpeakWav failed; see trace");
            return Result;
        }

        if (!bSpeaking)
        {
            Result.bPassed = false;
            Result.FailReason = FString::Printf(
                TEXT("no Connection reached state Connected within %.0f s, so nothing was spoken "
                     "and the transcript question does not arise"),
                ConnectTimeoutSeconds);
            return Result;
        }

        if (Failures > 0)
        {
            Result.bPassed = false;
            Result.FailReason = FString::Printf(TEXT("OnFailureEvent fired %d time(s)"), Failures);
            return Result;
        }

        if (Actual.IsEmpty())
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("no-player-transcript-for-spoken-audio");
            Finding.Summary =
                TEXT("scripted speech was streamed to a live character and no final player "
                     "transcript came back, so the player's voice is not reaching the server or "
                     "the response is not reaching the plugin");
            Finding.Evidence = FString::Printf(
                TEXT("fixture %s spoken through the Virtual Mic over a Connection that reached "
                     "Connected in %.1f s; %d transcript events arrived in the %.0f s after, none "
                     "of them final and non-empty. AECType=%s. The mic emitted %lld samples."),
                kFixture, ConnectedAtSeconds, All.Num(), TranscriptWindowSeconds,
                AECType ? AECType : TEXT("<default>"),
                VirtualMic.IsValid() ? VirtualMic->EmittedSamples() : 0);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.bPassed = false;
            Result.FailReason = TEXT("no final player transcript");
            return Result;
        }

        // Reported, not asserted. The number is the point of the scenario; the
        // pass condition is only that a transcript arrived at all.
        const double Wer = ConvaiTestSteps::WordErrorRate(Expected, Actual);
        Result.Metrics.Add(TEXT("word_error_rate"), Wer);
        Recorder->Record(TEXT("transcript"), Actual);
        Recorder->Record(TEXT("expected"), Expected);
        Recorder->Record(TEXT("word_error_rate"), FString::Printf(TEXT("%.3f"), Wer));

        // Model-dependent, so it travels as a lead rather than as a finding.
        if (Wer > 0.5)
        {
            Result.Hypotheses.Add(FString::Printf(
                TEXT("word error rate %.2f against STT.json for %s. Above roughly 0.5 the audio "
                     "reaching the server is more likely damaged than merely mis-transcribed; "
                     "compare against speak_wav_through_virtual_mic, which measures the same "
                     "fixture at the capture submix."),
                Wer, kFixture));
        }

        // No shape assertion for the player. The plugin forwards
        // user-transcription exactly as the server sent it, so the shape is the
        // server's to choose -- measured cumulative in PIE and as word-sized
        // deltas against this harness character -- and asserting either one
        // would fail on the backend's configuration rather than on the plugin.
        // The character's shape IS the plugin's, and text_roundtrip grades it.

        Result.bPassed = true;
        return Result;
    }

private:
    bool IsConnected() const
    {
        const UConvaiChatbotComponent* Chatbot = ChatbotComponent.Get();
        const UConvaiConnectionSessionProxy* Proxy = Chatbot ? Chatbot->GetSessionProxy() : nullptr;
        return Proxy && Proxy->GetConnectionState() == EC_ConnectionState::Connected;
    }

    static constexpr float ConnectTimeoutSeconds = 30.0f;
    static constexpr float TranscriptWindowSeconds = 25.0f;
    static constexpr float TrailingSilenceSeconds = 3.0f;

    const TCHAR* AECType;

    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    FString CharacterID;

    TWeakObjectPtr<AActor> Owner;
    TWeakObjectPtr<UConvaiVirtualMicComponent> VirtualMic;
    TWeakObjectPtr<UConvaiPlayerComponent> PlayerComponent;
    TWeakObjectPtr<UConvaiChatbotComponent> ChatbotComponent;
    TStrongObjectPtr<UConvaiTestEventSink> Sink;

    bool bSetupFailed = false;
    bool bSpeaking = false;
    bool bSpeakFailed = false;
    float Elapsed = 0.0f;
    float ConnectedAtSeconds = 0.0f;
};

CONVAI_REGISTER_SCENARIO(FConvaiLiveTranscriptScenario)

// The paired AEC run. Same scenario, same fixture, different AECType, one
// process each. Issue 05 wants the None run to misbehave where the Internal run
// does not; this pair establishes that both connect and transcribe first,
// because a None run that simply failed to connect would look like a result.
static struct FConvaiLiveTranscriptAECRegistrar
{
    FConvaiLiveTranscriptAECRegistrar()
    {
        for (const TCHAR* Type : {TEXT("Internal"), TEXT("None")})
        {
            ConvaiTestRegistry::Register(
                FConvaiLiveTranscriptScenario::NameForAEC(Type),
                [Type]() -> TSharedRef<FConvaiTestScenario>
                { return MakeShared<FConvaiLiveTranscriptScenario>(Type); });
        }
    }
} GConvaiLiveTranscriptAECRegistrar;
