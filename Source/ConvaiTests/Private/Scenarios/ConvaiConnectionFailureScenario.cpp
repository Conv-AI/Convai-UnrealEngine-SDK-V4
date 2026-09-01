// Copyright 2022 Convai Inc. All Rights Reserved.

// A character ID that cannot exist, and what the plugin does about it.
//
// The DLL harness's error_handling covers the client's own behaviour on a bad
// connect. This covers the plugin's: that a failed session is reported through
// OnFailureEvent or leaves the component out of Connected, and above all that
// it terminates. A plugin that sits in Connecting forever on a bad ID is a
// game that hangs on a typo'd character, and nothing in the DLL's tier can see
// the component's state machine.
//
// The pass condition is deliberately loose about *which* of the two happens.
// Pinning it to OnFailureEvent alone would make this scenario fail when the
// plugin legitimately reports the failure by returning to Disconnected instead,
// and the invariant worth defending is that the game is told something and is
// not left waiting.

#include "ConvaiChatbotComponent.h"
#include "ConvaiDefinitions.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestEventSink.h"
#include "ConvaiTestFixture.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTests.h"

#include "Engine/World.h"

namespace
{
    // Well-formed enough to be sent, and cannot match a real character.
    const TCHAR* kInvalidCharacterID = TEXT("00000000-0000-0000-0000-00000000dead");

    // Well past a healthy connect. session_connect_disconnect allows 30 s for a
    // real one, so anything still Connecting here is not slow, it is stuck.
    constexpr float kResolveTimeoutSeconds = 45.0f;
}

class FConvaiConnectionFailureScenario : public FConvaiTestScenario
{
public:
    static const TCHAR* StaticName() { return TEXT("connection_invalid_character"); }
    virtual const TCHAR* Name() const override { return StaticName(); }
    virtual double DeadlineSeconds() const override { return 90.0; }

    // A real API key is needed for the request to get far enough to be rejected
    // for the right reason; the character is the part being made invalid.
    virtual bool RequiresLiveConnection() const override { return true; }
    // The character it connects on is deliberately impossible, so the
    // configured one is not needed -- HasCredentials is called with
    // bRequireCharacter=false for the same reason.
    virtual bool RequiresTestCharacter() const override { return false; }

    virtual void Start(UWorld* InWorld, FConvaiTestEventRecorder& InRecorder) override
    {
        World = InWorld;
        Recorder = &InRecorder;
        Latency.Begin();

        FString Error;
        // No character requirement: this scenario supplies its own.
        if (!FConvaiTestFixture::HasCredentials(Error, /*bRequireCharacter=*/false))
        {
            Recorder->Record(TEXT("setup_failed"), Error);
            bSetupFailed = true;
            return;
        }

        FConvaiTestFixture::FOptions Options;
        Options.CharacterID = kInvalidCharacterID;
        if (!Fixture.Spawn(World, *Recorder, Options, Error))
        {
            Recorder->Record(TEXT("setup_failed"), Error);
            bSetupFailed = true;
            return;
        }

        if (UConvaiChatbotComponent* Chatbot = Fixture.Chatbot.Get())
        {
            Chatbot->StartSession();
            Recorder->Record(TEXT("start_session"), kInvalidCharacterID);
        }
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }
        Elapsed += DeltaSeconds;

        UConvaiChatbotComponent* Chatbot = Fixture.Chatbot.Get();
        UConvaiTestEventSink* Sink = Fixture.Sink.Get();
        if (!Chatbot || !Sink)
        {
            FailReason = TEXT("the component set went away before the connection resolved");
            return true;
        }

        const EC_ConnectionState State = Chatbot->GetChatbotConnectionState();

        if (Sink->FailureCount() > 0)
        {
            bReportedFailure = true;
            Latency.Mark(TEXT("resolved_ms"));
            return true;
        }

        // Connected on a character that cannot exist is its own bug, and a
        // worse one than hanging: the game would proceed as if it had a
        // character. Recorded and asserted separately below.
        if (State == EC_ConnectionState::Connected)
        {
            bConnectedAnyway = true;
            Latency.Mark(TEXT("resolved_ms"));
            return true;
        }

        if (State == EC_ConnectionState::Connecting)
        {
            bEverConnecting = true;
        }
        // Back to Disconnected after having been Connecting: the plugin gave up
        // without raising OnFailureEvent, which is a legitimate way to report
        // this. The bEverConnecting guard matters because Disconnected is also
        // the state before StartSession has taken effect, and treating that as
        // a resolution would pass this scenario in the first tick.
        else if (bEverConnecting && State == EC_ConnectionState::Disconnected)
        {
            bLeftDisconnected = true;
            Latency.Mark(TEXT("resolved_ms"));
            return true;
        }

        if (Elapsed > kResolveTimeoutSeconds)
        {
            return true;
        }
        return false;
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;
        UConvaiChatbotComponent* Chatbot = Fixture.Chatbot.Get();
        UConvaiTestEventSink* Sink = Fixture.Sink.Get();

        const int32 Failures = Sink ? Sink->FailureCount() : 0;
        const EC_ConnectionState FinalState =
            Chatbot ? Chatbot->GetChatbotConnectionState() : EC_ConnectionState::Disconnected;

        Result.Metrics.Add(TEXT("failure_events"), Failures);
        Result.Metrics.Add(TEXT("connected_anyway"), bConnectedAnyway ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("ever_connecting"), bEverConnecting ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("still_connecting_at_end"),
                           FinalState == EC_ConnectionState::Connecting ? 1.0 : 0.0);
        for (const TPair<FString, double>& Pair : Latency.Snapshot())
        {
            Result.Metrics.Add(Pair.Key, Pair.Value);
        }

        Result.MetricNotes.Add(
            TEXT("failure_events"),
            TEXT("Covers: a bad character ID being reported rather than hung on. Does NOT cover: "
                 "the reason being correct -- the scenario cannot tell a rejected character from "
                 "a network error, so a backend outage passes it for the wrong reason. Read it "
                 "with the other live scenarios, which go red on an outage."));

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

        if (bConnectedAnyway)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("invalid-character-reports-connected");
            Finding.Summary = TEXT("a character ID that cannot exist produced a Connected "
                                   "session, so a game would proceed as if it had a character");
            Finding.Evidence = FString::Printf(
                TEXT("StartSession with CharacterID=%s reached Connected within %.1f s and no "
                     "OnFailureEvent fired."),
                kInvalidCharacterID, Elapsed);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = TEXT("an invalid character ID connected successfully");
            return Result;
        }

        if (!bReportedFailure && !bLeftDisconnected)
        {
            const bool bStuckConnecting = FinalState == EC_ConnectionState::Connecting;

            FConvaiScenarioFinding Finding;
            Finding.DedupKey = bStuckConnecting ? TEXT("invalid-character-hangs-connecting")
                                                : TEXT("invalid-character-fails-silently");
            Finding.Summary =
                bStuckConnecting
                    ? TEXT("a session on an impossible character sat in Connecting until the "
                           "deadline, so a game waiting on it waits forever")
                    : TEXT("a session on an impossible character failed without raising "
                           "OnFailureEvent, so the game is never told the character is bad");
            Finding.Evidence = FString::Printf(
                TEXT("StartSession with CharacterID=%s. Over %.0f s the component never reached "
                     "Connected, never entered Connecting (ever_connecting=%d), and raised %d "
                     "OnFailureEvent(s); it reports %s at the end. Every route a game has to "
                     "learn the character is bad -- the failure delegate and the connection "
                     "state -- is silent."),
                kInvalidCharacterID, kResolveTimeoutSeconds, bEverConnecting ? 1 : 0, Failures,
                bStuckConnecting ? TEXT("Connecting") : TEXT("Disconnected"));
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            // Unverified, and kept out of the evidence for that reason. The
            // scenario cannot see the plugin's internals; this is where to look
            // first because it is the only place that knows the ID was refused.
            Result.Hypotheses.Add(
                TEXT("UConvaiChatbotComponent::OnConvaiGetDetailsCompleted logs "
                     "\"Could not get character details\" on the REST 404 and returns without "
                     "broadcasting OnFailureEvent. Check whether the character-details failure "
                     "path has any route to the delegate at all."));

            Result.FailReason =
                bStuckConnecting
                    ? FString::Printf(TEXT("the bad-character session hung in Connecting for %.0f s"),
                                      kResolveTimeoutSeconds)
                    : TEXT("a bad character ID failed without raising OnFailureEvent");
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

    float Elapsed = 0.0f;
    bool bSetupFailed = false;
    bool bReportedFailure = false;
    bool bConnectedAnyway = false;
    bool bEverConnecting = false;
    bool bLeftDisconnected = false;
    FString FailReason;
};

CONVAI_REGISTER_SCENARIO(FConvaiConnectionFailureScenario)
