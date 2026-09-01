// Copyright 2022 Convai Inc. All Rights Reserved.

// What the server says about the session, and whether the game is told.
//
// The plugin used to label every `error-response` packet a "compatibility notice"
// in the log and drop it, and `UConvaiSubsystem::OnError` -- the fatal path -- was
// a static member, so it could not reach a session either. No server-reported
// error of either severity had a route to a game (FINDINGS F9).
//
// Both severities are driven, and neither is left to the server's mood. The
// subsystem's packet sink is private, so rather than widen it -- ADR-0005 is
// explicit that private surface becomes public on its own merits and not for a
// test -- the two errors go in one level further on, through
// `IConvaiConnectionInterface::OnServerError`. That is public on a public
// interface which the chatbot publicly declares it implements, and it is the exact
// call the subsystem makes when a packet arrives, so it is the seam a third-party
// session implementation would be driven through too.
//
// Three assertions:
//
//  1. a non-fatal server error reaches OnServerErrorEvent;
//  2. a fatal one reaches it too, and raises OnFailureEvent once;
//  3. the non-fatal one raises no OnFailureEvent.
//
// (3) is what keeps the fix honest. The cheap version -- route error-response to
// the existing failure delegate -- passes (1) and fails (3), and it would have
// every game reporting a failure on each of the advisories a healthy session
// receives.
//
// The half this cannot see is the subsystem's own decode and routing, and the live
// server covers it: it sends the client-version advisory on every session measured
// so far, and each one that arrives here travelled the real path. That count is
// reported, not asserted on -- the day the notice is fixed server-side it goes to
// zero and this scenario still passes.

#include "ConvaiChatbotComponent.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestEventSink.h"
#include "ConvaiTestFixture.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTests.h"

#include "Engine/World.h"

namespace
{
    constexpr float kConnectTimeoutSeconds = 30.0f;

    // Delegates are broadcast onto the game thread, so the assertion cannot run
    // on the frame that injects. Also the window in which the live server's own
    // advisories are collected.
    constexpr float kSettleSeconds = 5.0f;

    // Marks the injected errors so the live server's advisories, which arrive on
    // the same delegate, cannot be mistaken for them in either direction.
    const TCHAR* kMarker = TEXT("convai-tests-injected");
}

class FConvaiServerErrorScenario : public FConvaiTestScenario
{
public:
    static const TCHAR* StaticName() { return TEXT("server_error_reaches_game"); }
    virtual const TCHAR* Name() const override { return StaticName(); }
    virtual double DeadlineSeconds() const override { return 60.0; }
    virtual bool RequiresLiveConnection() const override { return true; }

    virtual void Start(UWorld* InWorld, FConvaiTestEventRecorder& InRecorder) override
    {
        World = InWorld;
        Recorder = &InRecorder;

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
        Recorder->Record(TEXT("session_started"), Fixture.CharacterID.Left(8));
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }
        Elapsed += DeltaSeconds;

        if (!bInjected)
        {
            if (!Fixture.IsChatbotConnected())
            {
                if (Elapsed > kConnectTimeoutSeconds)
                {
                    FailReason = FString::Printf(TEXT("no Connected state within %.0f s, so "
                                                      "nothing was injected"),
                                                 kConnectTimeoutSeconds);
                    return true;
                }
                return false;
            }

            IConvaiConnectionInterface* Connection =
                Cast<IConvaiConnectionInterface>(Fixture.Chatbot.Get());
            if (!Connection)
            {
                FailReason = TEXT("the chatbot does not implement IConvaiConnectionInterface");
                return true;
            }

            // After Connected, so the session this reports against is a real one.
            Connection->OnServerError(FString::Printf(TEXT("%s advisory"), kMarker),
                                      /*bFatal=*/false);
            Connection->OnServerError(FString::Printf(TEXT("%s fatal"), kMarker),
                                      /*bFatal=*/true);

            Recorder->Record(TEXT("injected"), TEXT("one non-fatal and one fatal"));
            bInjected = true;
            InjectedAtSeconds = Elapsed;
            return false;
        }

        return Elapsed - InjectedAtSeconds > kSettleSeconds;
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;
        UConvaiTestEventSink* Sink = Fixture.Sink.Get();

        const TArray<UConvaiTestEventSink::FServerError> Errors =
            Sink ? Sink->ServerErrors() : TArray<UConvaiTestEventSink::FServerError>();
        const int32 Failures = Sink ? Sink->FailureCount() : 0;

        int32 InjectedAdvisory = 0;
        int32 InjectedFatal = 0;
        int32 ServerSent = 0;
        for (const UConvaiTestEventSink::FServerError& Error : Errors)
        {
            Recorder->Record(TEXT("server_error"),
                             FString::Printf(TEXT("fatal=%d %s"), Error.bFatal ? 1 : 0,
                                             *Error.Message));
            if (!Error.Message.Contains(kMarker))
            {
                ++ServerSent;
            }
            else if (Error.bFatal)
            {
                ++InjectedFatal;
            }
            else
            {
                ++InjectedAdvisory;
            }
        }

        Result.Metrics.Add(TEXT("connected"), bInjected ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("injected_advisory_delivered"), InjectedAdvisory);
        Result.Metrics.Add(TEXT("injected_fatal_delivered"), InjectedFatal);
        Result.Metrics.Add(TEXT("server_sent_errors"), ServerSent);
        Result.Metrics.Add(TEXT("failure_events"), Failures);

        Result.MetricNotes.Add(
            TEXT("server_sent_errors"),
            TEXT("Covers: how many errors the live server chose to send during the session, "
                 "which is currently the client-version advisory. Does NOT cover the pass "
                 "condition -- that rests on the injected packets, so a server that stops "
                 "sending advisories does not turn this red."));
        Result.MetricNotes.Add(
            TEXT("failure_events"),
            TEXT("Covers: OnFailureEvent, which must fire exactly once -- for the injected "
                 "fatal error and nothing else. Anything higher means a non-fatal server "
                 "error is being reported to the game as a failed session."));

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

        if (InjectedAdvisory != 1 || InjectedFatal != 1)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("server-error-does-not-reach-game");
            Finding.Summary = TEXT("an error the server reported on the data channel never "
                                   "reached the game");
            Finding.Evidence = FString::Printf(
                TEXT("One non-fatal and one fatal error were delivered through "
                     "IConvaiConnectionInterface::OnServerError on a Connected session. %d "
                     "advisory and %d fatal arrived at OnServerErrorEvent; 1 of each was "
                     "expected. The live server also sent %d."),
                InjectedAdvisory, InjectedFatal, ServerSent);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = FString::Printf(
                TEXT("%d/1 advisory and %d/1 fatal reached the game"), InjectedAdvisory,
                InjectedFatal);
            return Result;
        }

        if (Failures != 1)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("non-fatal-server-error-raises-failure");
            Finding.Summary = TEXT("OnFailureEvent did not fire exactly once for the one fatal "
                                   "error, so the game's failure signal does not mean failure");
            Finding.Evidence = FString::Printf(
                TEXT("The session stayed Connected. One fatal and one non-fatal error were "
                     "injected and the live server sent %d more advisories; OnFailureEvent "
                     "fired %d time(s), against 1 expected."),
                ServerSent, Failures);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason =
                FString::Printf(TEXT("%d OnFailureEvent(s) for 1 fatal error"), Failures);
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

private:
    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    FConvaiTestFixture Fixture;

    float Elapsed = 0.0f;
    float InjectedAtSeconds = 0.0f;
    bool bInjected = false;
    bool bSetupFailed = false;
    FString FailReason;
};

CONVAI_REGISTER_SCENARIO(FConvaiServerErrorScenario)
