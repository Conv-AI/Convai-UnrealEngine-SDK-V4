// Copyright 2022 Convai Inc. All Rights Reserved.

// Send text, get the character's answer back through the events a Blueprint
// binds.
//
// The DLL harness's text_roundtrip asserts a data packet comes back. This
// asserts the plugin turned that packet into the things a game consumes: the
// chatbot's OnTranscriptionReceived carrying the character's words, and
// OnBotTurnCompleted firing once, not aborted. Everything between the wire and
// those delegates is plugin code -- packet decode, the response-id bookkeeping,
// the hop to the game thread -- and none of it is exercised by the DLL's tier.
//
// Text rather than speech on purpose. It removes the microphone, the resampler,
// the echo canceller and the server's VAD from the question, so a failure here
// is the response path and nothing else. audio_roundtrip adds the capture half
// back on top of this one.

#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestEventSink.h"
#include "ConvaiTestFixture.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTestSteps.h"
#include "ConvaiTests.h"

#include "Engine/World.h"

namespace
{
    constexpr float kConnectTimeoutSeconds = 30.0f;

    // The character has to think, speak, and have the server decide the turn
    // ended. Generous because a slow model must not read as a broken plugin.
    constexpr float kReplyTimeoutSeconds = 45.0f;

    // Short and closed. A prompt inviting a monologue makes the turn-completed
    // deadline a test of the model's verbosity.
    const TCHAR* kPrompt = TEXT("Say the word hello and nothing else.");
}

class FConvaiTextRoundtripScenario : public FConvaiTestScenario
{
public:
    // The configured arm runs the same oracle against the character Blueprint
    // named by -ConvaiTestActorClass= instead of the bare actor.
    explicit FConvaiTextRoundtripScenario(bool bInConfigured = false)
        : bConfigured(bInConfigured)
    {
    }

    static const TCHAR* StaticName() { return TEXT("text_roundtrip"); }
    static const TCHAR* ConfiguredName() { return TEXT("configured_text_roundtrip"); }
    virtual const TCHAR* Name() const override
    {
        return bConfigured ? ConfiguredName() : StaticName();
    }
    virtual double DeadlineSeconds() const override { return bConfigured ? 180.0 : 120.0; }
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
        // Opens the player's own session too. SendText goes through the
        // player's Session Proxy, so without this the message has nowhere to
        // go and the scenario would time out looking like a slow server.
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

        if (!bSent)
        {
            if (!Fixture.IsChatbotConnected())
            {
                if (Elapsed > kConnectTimeoutSeconds)
                {
                    FailReason = FString::Printf(
                        TEXT("no Connected state within %.0f s, so nothing was sent"),
                        kConnectTimeoutSeconds);
                    return true;
                }
                return false;
            }

            Latency.Mark(TEXT("connect_ms"));
            ConvaiTestSteps::SayText(Fixture.Player.Get(), Fixture.Chatbot.Get(), kPrompt);
            Recorder->Record(TEXT("sent_text"), kPrompt);
            bSent = true;
            SentAtSeconds = Elapsed;
            return false;
        }

        UConvaiTestEventSink* Sink = Fixture.Sink.Get();
        if (!Sink)
        {
            FailReason = TEXT("event sink went away");
            return true;
        }

        if (!bSawBotText && !ConvaiTestSteps::AssembledTextFrom(*Sink, BotName).IsEmpty())
        {
            bSawBotText = true;
            Latency.Mark(TEXT("bot_transcript_ms"));
        }

        if (Sink->BotTurns().Num() > 0)
        {
            Latency.Mark(TEXT("bot_turn_completed_ms"));
            return true;
        }

        if (Elapsed - SentAtSeconds > kReplyTimeoutSeconds)
        {
            return true;
        }
        return false;
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;
        UConvaiTestEventSink* Sink = Fixture.Sink.Get();

        // The character broadcasts increments and finalises empty, so its answer
        // is the increments joined -- see AssembledTextFrom.
        const FString BotText = Sink ? ConvaiTestSteps::AssembledTextFrom(*Sink, BotName)
                                     : FString();
        const TArray<UConvaiTestEventSink::FBotTurn> Turns =
            Sink ? Sink->BotTurns() : TArray<UConvaiTestEventSink::FBotTurn>();
        const int32 Failures = Sink ? Sink->FailureCount() : 0;
        const int32 DetailsFailures = Sink ? Sink->CharacterDataLoadFailures() : 0;

        Result.Metrics.Add(TEXT("connected"), bSent ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("bot_transcript_chars"), BotText.Len());
        Result.Metrics.Add(TEXT("bot_turns"), Turns.Num());
        Result.Metrics.Add(TEXT("failures"), Failures);
        for (const TPair<FString, double>& Pair : Latency.Snapshot())
        {
            Result.Metrics.Add(Pair.Key, Pair.Value);
        }

        Result.MetricNotes.Add(
            TEXT("bot_transcript_chars"),
            TEXT("Covers: the character answered and the plugin delivered the answer to the "
                 "chatbot's transcript delegate. Does NOT cover: whether the answer is a "
                 "sensible reply to the prompt. Asserting on the words would fail when the "
                 "model changes rather than when the plugin breaks."));

        // Graded before Destroy, while the sink still holds the stream. The
        // shape is a separate question from the words: the final was correct
        // through the whole "Hello Hello. Hello." regression, so nothing that
        // grades LatestFinalTextFrom alone can see this.
        TOptional<FConvaiScenarioFinding> ShapeFinding;
        if (Sink)
        {
            ShapeFinding = ConvaiTestSteps::TranscriptShapeFinding(
                *Sink, BotName, ConvaiTestSteps::ETranscriptShape::Increment, Result.Metrics);
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
                Failures, DetailsFailures, TEXT("a plain text exchange"), Result.Metrics));
            Result.FailReason =
                FString::Printf(TEXT("%d OnFailureEvent(s) during a plain text exchange"),
                                Failures);
            return Result;
        }

        if (BotText.IsEmpty())
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("no-bot-transcript-for-text-prompt");
            Finding.Summary = TEXT("a text prompt produced no character transcript on the "
                                   "chatbot's transcript delegate");
            Finding.Evidence = FString::Printf(
                TEXT("The session reached Connected and SendText was called with \"%s\". Within "
                     "%.0f s the chatbot component broadcast no non-empty final transcript. "
                     "%d bot turn(s) completed and %d failure event(s) fired."),
                kPrompt, kReplyTimeoutSeconds, Turns.Num(), Failures);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = TEXT("no character transcript came back for a text prompt");
            return Result;
        }

        if (Turns.Num() == 0)
        {
            // Distinct from no transcript on purpose: the words arriving while
            // the turn never closes is a bookkeeping bug in the plugin, and a
            // game that waits for OnBotTurnCompleted hangs on it forever.
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("bot-turn-never-completed");
            Finding.Summary = TEXT("the character's answer arrived but OnBotTurnCompleted never "
                                   "fired, so a game waiting on the turn would wait forever");
            Finding.Evidence = FString::Printf(
                TEXT("The chatbot broadcast a final transcript of %d character(s) but no "
                     "OnBotTurnCompleted event within %.0f s of the prompt being sent."),
                BotText.Len(), kReplyTimeoutSeconds);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = TEXT("the bot's turn never completed");
            return Result;
        }

        if (Turns[0].bAborted)
        {
            Result.FailReason = FString::Printf(TEXT("the bot's turn was aborted: %s"),
                                                Turns[0].ErrorReason.IsEmpty()
                                                    ? TEXT("<no reason given>")
                                                    : *Turns[0].ErrorReason);
            return Result;
        }

        if (ShapeFinding.IsSet())
        {
            Result.Findings.Add(MoveTemp(ShapeFinding.GetValue()));
            Result.FailReason =
                TEXT("the character's transcript broadcasts break the whole-utterance contract");
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
    float SentAtSeconds = 0.0f;
    bool bSent = false;
    bool bSawBotText = false;
    bool bSetupFailed = false;
    FString FailReason;
};

CONVAI_REGISTER_SCENARIO(FConvaiTextRoundtripScenario)

static struct FConvaiConfiguredTextRoundtripRegistrar
{
    FConvaiConfiguredTextRoundtripRegistrar()
    {
        ConvaiTestRegistry::Register(FConvaiTextRoundtripScenario::ConfiguredName(),
                                     []() -> TSharedRef<FConvaiTestScenario> {
                                         return MakeShared<FConvaiTextRoundtripScenario>(true);
                                     });
    }
} GConvaiConfiguredTextRoundtripRegistrar;
