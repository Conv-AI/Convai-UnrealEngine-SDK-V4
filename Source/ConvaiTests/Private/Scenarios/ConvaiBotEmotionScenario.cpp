// Copyright 2022 Convai Inc. All Rights Reserved.

// Provoke an emotion, and check the plugin turned the server's bot-emotion
// packets into a non-zero score on the chatbot's emotion state.
//
// text_roundtrip proves the reply path; this proves the side channel a face
// AnimBP binds: OnEmotionStateChanged firing with GetEmotionScore reading
// something other than zero for one of the basic emotions. The failure this
// exists for is silent by construction -- a packet whose emotion name the
// plugin does not recognise still fires the event, with every score at zero --
// so the assertion is on the score, not on the event count.
//
// The prompt is loaded with anger vocabulary because the default server-side
// analyser (nrclex) counts lexicon hits per sentence; a bland reply legitimately
// comes back "neutral". Which emotion wins is the model's business and is not
// asserted.

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
    constexpr float kReplyTimeoutSeconds = 45.0f;

    const TCHAR* kPrompt = TEXT(
        "In three short sentences, tell me how furious and angry you are about thieves. "
        "Use the words rage, fury, hate and angry.");
}

class FConvaiBotEmotionScenario : public FConvaiTestScenario
{
public:
    static const TCHAR* StaticName() { return TEXT("bot_emotion"); }
    virtual const TCHAR* Name() const override { return StaticName(); }
    virtual double DeadlineSeconds() const override { return 120.0; }
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
        if (!Fixture.Spawn(World, *Recorder, Options, Error))
        {
            Recorder->Record(TEXT("setup_failed"), Error);
            bSetupFailed = true;
            return;
        }

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

        const TArray<UConvaiTestEventSink::FEmotionSample> Samples = Sink->EmotionSamples();
        for (; RecordedSamples < Samples.Num(); ++RecordedSamples)
        {
            const UConvaiTestEventSink::FEmotionSample& Sample = Samples[RecordedSamples];
            if (RecordedSamples == 0)
            {
                Latency.Mark(TEXT("first_emotion_ms"));
            }
            if (Sample.Score > 0.0f && !bSawNonZero)
            {
                bSawNonZero = true;
                Latency.Mark(TEXT("first_nonzero_emotion_ms"));
            }
            Recorder->Record(TEXT("emotion"),
                             FString::Printf(TEXT("%s %.2f"), *EmotionName(Sample.Dominant),
                                             Sample.Score));
        }

        // Emotions arrive while the reply streams, so the turn closing is the
        // natural end of the observation window.
        if (Sink->BotTurns().Num() > 0)
        {
            Latency.Mark(TEXT("bot_turn_completed_ms"));
            return true;
        }

        return Elapsed - SentAtSeconds > kReplyTimeoutSeconds;
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;
        UConvaiTestEventSink* Sink = Fixture.Sink.Get();

        const TArray<UConvaiTestEventSink::FEmotionSample> Samples =
            Sink ? Sink->EmotionSamples() : TArray<UConvaiTestEventSink::FEmotionSample>();
        const int32 Turns = Sink ? Sink->BotTurns().Num() : 0;
        const int32 Failures = Sink ? Sink->FailureCount() : 0;

        int32 NonZero = 0;
        float PeakScore = 0.0f;
        EBasicEmotions Peak = EBasicEmotions::None;
        for (const UConvaiTestEventSink::FEmotionSample& Sample : Samples)
        {
            if (Sample.Score > 0.0f)
            {
                ++NonZero;
            }
            if (Sample.Score > PeakScore)
            {
                PeakScore = Sample.Score;
                Peak = Sample.Dominant;
            }
        }

        Result.Metrics.Add(TEXT("connected"), bSent ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("emotion_events"), Samples.Num());
        Result.Metrics.Add(TEXT("emotion_events_nonzero"), NonZero);
        Result.Metrics.Add(TEXT("emotion_peak_score"), PeakScore);
        Result.Metrics.Add(TEXT("bot_turns"), Turns);
        Result.Metrics.Add(TEXT("failures"), Failures);
        for (const TPair<FString, double>& Pair : Latency.Snapshot())
        {
            Result.Metrics.Add(Pair.Key, Pair.Value);
        }
        Result.MetricNotes.Add(
            TEXT("emotion_events_nonzero"),
            TEXT("Covers: the server sent bot-emotion and the plugin decoded the name into a "
                 "basic emotion a game can read through GetEmotionScore. Does NOT cover: "
                 "which emotion, or whether the face AnimBP maps that emotion to anything."));

        Recorder->Record(TEXT("emotion_peak"),
                         FString::Printf(TEXT("%s %.2f"), *EmotionName(Peak), PeakScore));
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
            Result.FailReason =
                FString::Printf(TEXT("%d OnFailureEvent(s) during the exchange"), Failures);
            return Result;
        }

        if (Samples.Num() == 0)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("no-bot-emotion-event");
            Finding.Summary = TEXT("an emotionally loaded prompt produced no "
                                   "OnEmotionStateChanged event at all");
            Finding.Evidence = FString::Printf(
                TEXT("SendText was called with \"%s\"; %d bot turn(s) completed within %.0f s "
                     "and the chatbot never broadcast OnEmotionStateChanged. Either the server "
                     "sent no bot-emotion (check the character's State of Mind setting and the "
                     "connect body's emotion_config) or the plugin dropped the packet."),
                kPrompt, Turns, kReplyTimeoutSeconds);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));
            Result.FailReason = TEXT("no emotion event for an emotionally loaded prompt");
            return Result;
        }

        if (NonZero == 0)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("bot-emotion-decoded-to-nothing");
            Finding.Summary = TEXT("bot-emotion events fired but every basic emotion score "
                                   "stayed at zero, so a face AnimBP would see nothing");
            Finding.Evidence = FString::Printf(
                TEXT("%d OnEmotionStateChanged event(s) fired for \"%s\" and GetEmotionScore was "
                     "zero for all eight basic emotions after each one. A neutral reply does "
                     "this legitimately; every event being neutral for this prompt means the "
                     "server's emotion names are not being decoded."),
                Samples.Num(), kPrompt);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));
            Result.FailReason = TEXT("emotion events decoded to all-zero scores");
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

private:
    static FString EmotionName(EBasicEmotions Emotion)
    {
        return StaticEnum<EBasicEmotions>()->GetNameStringByValue(static_cast<int64>(Emotion));
    }

    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    FConvaiTestFixture Fixture;
    FConvaiTestLatencyTracker Latency;

    float Elapsed = 0.0f;
    float SentAtSeconds = 0.0f;
    int32 RecordedSamples = 0;
    bool bSent = false;
    bool bSawNonZero = false;
    bool bSetupFailed = false;
    FString FailReason;
};

CONVAI_REGISTER_SCENARIO(FConvaiBotEmotionScenario)
