// Copyright 2022 Convai Inc. All Rights Reserved.

// The whole loop: a WAV through the Virtual Mic, the server's transcript of it,
// the character's answer, and the turn closing.
//
// live_player_transcript stops at the player's own transcript -- it proves
// scripted speech reached the server and came back recognised. This carries on
// to the half that scenario never asserts: that the character then answered,
// and that the plugin delivered the answer. Those are separate failures with
// separate causes. Capture breaking is a microphone, resampler or send-path
// problem; the answer not arriving is the response path, which text_roundtrip
// isolates without any audio at all. Running all three localises a break to one
// of the three without a human reading logs.
//
// Word error rate is reported and never asserted, for issue 04's reason: a
// threshold on ASR quality fails when the model changes rather than when the
// plugin breaks.

#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestEventSink.h"
#include "ConvaiTestFixture.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTestSteps.h"
#include "ConvaiTests.h"
#include "ConvaiVirtualMicComponent.h"

#include "Engine/World.h"

namespace
{
    const TCHAR* kFixture = TEXT("S1");
    const TCHAR* kPlayerComponentName = TEXT("ConvaiPlayer");

    constexpr float kConnectTimeoutSeconds = 30.0f;

    // Long enough for the utterance, the server's end-of-speech decision
    // (ConvaiVadParams defaults stop_secs to 2.2), the model, and the answer.
    constexpr float kReplyTimeoutSeconds = 60.0f;
}

class FConvaiAudioRoundtripScenario : public FConvaiTestScenario
{
public:
    // The configured arm runs the same oracle against the character Blueprint
    // named by -ConvaiTestActorClass= instead of the bare actor.
    explicit FConvaiAudioRoundtripScenario(bool bInConfigured = false)
        : bConfigured(bInConfigured)
    {
    }

    static const TCHAR* StaticName() { return TEXT("audio_roundtrip"); }
    static const TCHAR* ConfiguredName() { return TEXT("configured_audio_roundtrip"); }
    virtual const TCHAR* Name() const override
    {
        return bConfigured ? ConfiguredName() : StaticName();
    }
    virtual double DeadlineSeconds() const override { return bConfigured ? 180.0 : 150.0; }
    virtual bool RequiresLiveConnection() const override { return true; }

    virtual void Start(UWorld* InWorld, FConvaiTestEventRecorder& InRecorder) override
    {
        World = InWorld;
        Recorder = &InRecorder;
        Latency.Begin();

        FString Error;
        if (!FConvaiTestFixture::HasCredentials(Error))
        {
            Recorder->Record(TEXT("setup_failed"), Error);
            bSetupFailed = true;
            return;
        }

        FConvaiTestFixture::FOptions Options;
        if (bConfigured && !FConvaiTestFixture::HasConfiguredActorClass(Options.ActorClassPath, Error))
        {
            Recorder->Record(TEXT("setup_failed"), Error);
            bSetupFailed = true;
            return;
        }
        if (!Fixture.Spawn(World, *Recorder, Options, Error))
        {
            Recorder->Record(TEXT("setup_failed"), Error);
            bSetupFailed = true;
            return;
        }
        BotName = Fixture.Chatbot.IsValid() ? Fixture.Chatbot->GetName() : FString();

        if (UConvaiChatbotComponent* Chatbot = Fixture.Chatbot.Get())
        {
            Chatbot->StartSession();
        }
        ConvaiTestSteps::SetTalkTargets(Fixture.Player.Get(), {Fixture.Chatbot.Get()});
        Recorder->Record(TEXT("session_started"), Fixture.CharacterID.Left(8));
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }
        Elapsed += DeltaSeconds;

        if (!bSpoke)
        {
            if (!Fixture.IsChatbotConnected())
            {
                if (Elapsed > kConnectTimeoutSeconds)
                {
                    FailReason = FString::Printf(
                        TEXT("no Connected state within %.0f s, so nothing was spoken"),
                        kConnectTimeoutSeconds);
                    return true;
                }
                return false;
            }
            Latency.Mark(TEXT("connect_ms"));

            UConvaiPlayerComponent* Player = Fixture.Player.Get();
            UConvaiVirtualMicComponent* Mic = Fixture.Mic.Get();
            if (!Player || !Mic)
            {
                FailReason = TEXT("player or virtual mic went away before speaking");
                return true;
            }

            // Unmute before queueing: the plugin only pulls from the adopted
            // capture component while it is streaming, so samples queued first
            // would be emitted into a stream nobody is reading.
            Player->UnmuteStreamingAudio();

            FString Error;
            if (!ConvaiTestSteps::SpeakWav(Mic, kFixture, Error))
            {
                FailReason = FString::Printf(TEXT("SpeakWav(%s) failed: %s"), kFixture, *Error);
                return true;
            }
            // Trailing quiet so the server's VAD sees the utterance end rather
            // than the stream stopping, which is a different code path.
            ConvaiTestSteps::Silence(Mic, 3.0f);
            Mic->Start();

            bSpoke = true;
            SpokeAtSeconds = Elapsed;
            Recorder->Record(TEXT("spoke_wav"), kFixture);
            return false;
        }

        UConvaiTestEventSink* Sink = Fixture.Sink.Get();
        if (!Sink)
        {
            FailReason = TEXT("event sink went away");
            return true;
        }

        if (!bSawPlayerText && !Sink->LatestFinalTextFrom(kPlayerComponentName).IsEmpty())
        {
            bSawPlayerText = true;
            Latency.Mark(TEXT("player_transcript_ms"));
        }
        if (!bSawBotText && !ConvaiTestSteps::AssembledTextFrom(*Sink, BotName).IsEmpty())
        {
            bSawBotText = true;
            Latency.Mark(TEXT("bot_transcript_ms"));
        }

        // Whether the character spoke at all, from the same public state the
        // AEC scenarios poll. Without it the only evidence of an answer is
        // OnBotTurnCompleted, and a run that lost that one packet is
        // indistinguishable from a character that never answered.
        if (!bBotSpoke)
        {
            if (UConvaiChatbotComponent* Chatbot = Fixture.Chatbot.Get())
            {
                if (Chatbot->GetIsTalking())
                {
                    bBotSpoke = true;
                    Latency.Mark(TEXT("bot_started_speaking_ms"));
                    Recorder->Record(TEXT("bot_started_speaking"), FString());
                }
            }
        }

        if (Sink->BotTurns().Num() > 0)
        {
            Latency.Mark(TEXT("bot_turn_completed_ms"));
            return true;
        }

        if (Elapsed - SpokeAtSeconds > kReplyTimeoutSeconds)
        {
            return true;
        }
        return false;
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;
        UConvaiTestEventSink* Sink = Fixture.Sink.Get();
        UConvaiVirtualMicComponent* Mic = Fixture.Mic.Get();

        const FString PlayerText =
            Sink ? Sink->LatestFinalTextFrom(kPlayerComponentName) : FString();
        // The character broadcasts increments and finalises empty, so its answer
        // is the increments joined -- see AssembledTextFrom.
        const FString BotText = Sink ? ConvaiTestSteps::AssembledTextFrom(*Sink, BotName)
                                     : FString();
        const TArray<UConvaiTestEventSink::FBotTurn> Turns =
            Sink ? Sink->BotTurns() : TArray<UConvaiTestEventSink::FBotTurn>();
        const int32 Failures = Sink ? Sink->FailureCount() : 0;
        const int32 DetailsFailures = Sink ? Sink->CharacterDataLoadFailures() : 0;

        const FString Expected = ConvaiTestSteps::ExpectedTranscript(kFixture);
        const double Wer = ConvaiTestSteps::WordErrorRate(Expected, PlayerText);

        Result.Metrics.Add(TEXT("connected"), bSpoke ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("mic_emitted_samples"),
                           Mic ? static_cast<double>(Mic->EmittedSamples()) : 0.0);
        Result.Metrics.Add(TEXT("player_transcript_chars"), PlayerText.Len());
        Result.Metrics.Add(TEXT("bot_transcript_chars"), BotText.Len());
        Result.Metrics.Add(TEXT("player_wer"), Wer);
        Result.Metrics.Add(TEXT("bot_turns"), Turns.Num());
        Result.Metrics.Add(TEXT("bot_spoke"), bBotSpoke ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("failures"), Failures);
        for (const TPair<FString, double>& Pair : Latency.Snapshot())
        {
            Result.Metrics.Add(Pair.Key, Pair.Value);
        }

        Result.MetricNotes.Add(
            TEXT("player_wer"),
            TEXT("Reported, never asserted (issue 04). Covers: nothing on its own -- it moves "
                 "when the recogniser's model changes. Useful only as a trend across runs of one "
                 "fixture. The assertion is that a transcript arrived at all."));
        Result.MetricNotes.Add(
            TEXT("bot_turns"),
            TEXT("Covers: the response half of the loop closing. Does NOT separate a capture "
                 "failure from a response failure -- player_transcript_chars is what tells those "
                 "apart, and text_roundtrip isolates the response half with no audio at all. Nor "
                 "does it separate a character that never answered from an answer whose "
                 "completion packet went missing -- bot_spoke is what tells those apart."));
        Result.MetricNotes.Add(
            TEXT("bot_spoke"),
            TEXT("Covers: the character produced audible speech, from UConvaiChatbotComponent::"
                 "GetIsTalking() polled each tick. Does NOT cover whether the answer was "
                 "complete, sensible, or acknowledged -- bot_turns is the completion signal. "
                 "bot_spoke=1 with bot_turns=0 is I5, not a broken response path."));

        if (Recorder)
        {
            Recorder->Record(TEXT("player_transcript"), PlayerText);
            Recorder->Record(TEXT("bot_transcript"), BotText);
        }
        Fixture.Destroy();

        if (bSetupFailed)
        {
            Result.bSetupFailed = true;
            Result.FailReason = TEXT("setup failed; see trace");
            return Result;
        }
        if (!FailReason.IsEmpty())
        {
            Result.FailReason = FailReason;
            return Result;
        }
        if (Failures > 0)
        {
            Result.Findings.Add(ConvaiTestSteps::FailureEventFinding(
                Failures, DetailsFailures, TEXT("the audio roundtrip"), Result.Metrics));
            Result.FailReason =
                FString::Printf(TEXT("%d OnFailureEvent(s) during the roundtrip"), Failures);
            return Result;
        }

        // Ordered so the report names the first thing that broke rather than
        // the last thing that did not happen.
        if (PlayerText.IsEmpty())
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("no-player-transcript-for-spoken-fixture");
            Finding.Summary = TEXT("a WAV played through the Virtual Mic produced no player "
                                   "transcript, so the capture half of the loop is broken");
            Finding.Evidence = FString::Printf(
                TEXT("Fixture %s was queued and the Virtual Mic emitted %lld samples after "
                     "UnmuteStreamingAudio. Within %.0f s no non-empty final transcript arrived "
                     "on the player component's delegate."),
                kFixture, Mic ? Mic->EmittedSamples() : 0, kReplyTimeoutSeconds);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = TEXT("no player transcript came back for the spoken fixture");
            return Result;
        }

        // Two different failures wore one message until 2026-08-20 (I6): a
        // character that never answered, and an answer whose completion packet
        // never arrived. Every observed failure was the second, and the report
        // sent readers after the first.
        if (Turns.Num() == 0 && (bBotSpoke || !BotText.IsEmpty()))
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("no-bot-turn-completed-after-character-spoke");
            Finding.Summary = TEXT("the character answered the player's speech but no "
                                   "OnBotTurnCompleted arrived for the turn");
            Finding.Evidence = FString::Printf(
                TEXT("The server returned the player transcript \"%s\" and the character %s; "
                     "%s. Within %.0f s of speaking no OnBotTurnCompleted arrived, so anything "
                     "keyed on turn completion never fired. The answer itself was delivered."),
                *PlayerText,
                bBotSpoke ? TEXT("spoke") : TEXT("did not reach an audible state"),
                BotText.IsEmpty()
                    ? TEXT("no character transcript arrived either, which is F23 and independent")
                    : TEXT("its transcript arrived"),
                kReplyTimeoutSeconds);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason =
                TEXT("the character answered but the turn never completed");
            return Result;
        }

        if (BotText.IsEmpty() && Turns.Num() == 0)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("no-character-response-to-speech");
            Finding.Summary = TEXT("the player's speech was transcribed but the character never "
                                   "answered it");
            Finding.Evidence = FString::Printf(
                TEXT("The server returned the player transcript \"%s\", so capture and send both "
                     "worked. Within %.0f s of speaking the character never reached a talking "
                     "state, no character transcript arrived and no OnBotTurnCompleted arrived. "
                     "text_roundtrip answers whether the response path works at all without "
                     "audio."),
                *PlayerText, kReplyTimeoutSeconds);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = TEXT("the character did not answer the player's speech");
            return Result;
        }

        if (Turns.Num() > 0 && Turns[0].bAborted)
        {
            Result.FailReason = FString::Printf(TEXT("the bot's turn was aborted: %s"),
                                                Turns[0].ErrorReason.IsEmpty()
                                                    ? TEXT("<no reason given>")
                                                    : *Turns[0].ErrorReason);
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

private:
    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    FConvaiTestFixture Fixture;
    FConvaiTestLatencyTracker Latency;

    const bool bConfigured;
    FString BotName;

    float Elapsed = 0.0f;
    float SpokeAtSeconds = 0.0f;
    bool bSpoke = false;
    bool bSawPlayerText = false;
    bool bSawBotText = false;
    bool bBotSpoke = false;
    bool bSetupFailed = false;
    FString FailReason;
};

CONVAI_REGISTER_SCENARIO(FConvaiAudioRoundtripScenario)

static struct FConvaiConfiguredAudioRoundtripRegistrar
{
    FConvaiConfiguredAudioRoundtripRegistrar()
    {
        ConvaiTestRegistry::Register(FConvaiAudioRoundtripScenario::ConfiguredName(),
                                     []() -> TSharedRef<FConvaiTestScenario> {
                                         return MakeShared<FConvaiAudioRoundtripScenario>(true);
                                     });
    }
} GConvaiConfiguredAudioRoundtripRegistrar;
