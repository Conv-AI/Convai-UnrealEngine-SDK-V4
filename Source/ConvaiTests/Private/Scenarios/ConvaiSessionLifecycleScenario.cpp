// Copyright 2022 Convai Inc. All Rights Reserved.

// StartSession -> Connected -> StopSession -> Disconnected, through the
// component a game actually calls.
//
// The DLL harness's connect_disconnect_basic and connect_disconnect_warm cover
// the same shape one layer down, on convai::ConvaiClient directly. This is the
// layer above: UConvaiChatbotComponent owns a Session Proxy, the proxy owns the
// connection, and the component's own state machine is what a Blueprint reads.
// A plugin that leaves GetChatbotConnectionState() saying Connected after
// StopSession is broken for every customer while the DLL's test stays green.
//
// The warm arm is the one worth the backend time. It reconnects on the SAME
// component rather than spawning a new one, which is the path a game takes when
// a player walks away from an NPC and returns -- and the path where a proxy
// that was not fully reset shows up as a second session that never connects.

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
    constexpr float kConnectTimeoutSeconds = 30.0f;
    constexpr float kDisconnectTimeoutSeconds = 15.0f;

    // The DLL harness sleeps 2 s after Connected before tearing down, because
    // the auto-started audio publish races the teardown otherwise and every
    // iteration logs a spurious error. The same race exists here.
    constexpr float kSettleSeconds = 2.0f;

    const TCHAR* StateName(EC_ConnectionState State)
    {
        switch (State)
        {
        case EC_ConnectionState::Connected:
            return TEXT("Connected");
        case EC_ConnectionState::Connecting:
            return TEXT("Connecting");
        default:
            return TEXT("Disconnected");
        }
    }
}

// Cycles is a constructor argument rather than a subclass because the two arms
// differ only in how many times round they go and whether the component is
// rebuilt in between.
class FConvaiSessionLifecycleScenario : public FConvaiTestScenario
{
public:
    FConvaiSessionLifecycleScenario(const TCHAR* InName, int32 InCycles, bool bInWarm)
        : ScenarioName(InName)
        , Cycles(InCycles)
        , bWarm(bInWarm)
    {
    }

    virtual const TCHAR* Name() const override { return ScenarioName; }
    virtual double DeadlineSeconds() const override
    {
        return Cycles * (kConnectTimeoutSeconds + kDisconnectTimeoutSeconds + kSettleSeconds) +
               15.0;
    }
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

        // The warm arm builds its components once and keeps them. The cold arm
        // rebuilds per cycle, so it spawns inside the cycle instead.
        if (bWarm && !SpawnFixture())
        {
            return;
        }
        Phase = EPhase::Connect;
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed || Phase == EPhase::Done)
        {
            return true;
        }
        InPhaseSeconds += DeltaSeconds;

        switch (Phase)
        {
        case EPhase::Connect:
            return PollConnect();
        case EPhase::Settle:
            return PollSettle();
        case EPhase::Disconnect:
            return PollDisconnect();
        default:
            return true;
        }
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;
        Result.Metrics.Add(TEXT("cycles_requested"), Cycles);
        Result.Metrics.Add(TEXT("cycles_completed"), Cycle);
        Result.Metrics.Add(TEXT("warm"), bWarm ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("failures"),
                           Fixture.Sink.IsValid() ? Fixture.Sink->FailureCount() : 0);
        for (const TPair<FString, double>& Pair : Latency.Snapshot())
        {
            Result.Metrics.Add(Pair.Key, Pair.Value);
        }

        Result.MetricNotes.Add(
            TEXT("cycles_completed"),
            TEXT("Covers: the component's connect and disconnect path, and on the warm arm the "
                 "reuse of one component across sessions. Does NOT cover: what travels over the "
                 "connection -- a session that connects and carries nothing passes this."));

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
        if (Cycle < Cycles)
        {
            Result.FailReason = FString::Printf(
                TEXT("only %d of %d connect/disconnect cycles finished before the deadline"),
                Cycle, Cycles);
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

private:
    enum class EPhase
    {
        Connect,
        Settle,
        Disconnect,
        Done
    };

    bool SpawnFixture()
    {
        FConvaiTestFixture::FOptions Options;
        FString Error;
        if (!Fixture.Spawn(World, *Recorder, Options, Error))
        {
            Recorder->Record(TEXT("setup_failed"), Error);
            bSetupFailed = true;
            return false;
        }
        return true;
    }

    void EnterPhase(EPhase Next)
    {
        Phase = Next;
        InPhaseSeconds = 0.0f;
    }

    bool PollConnect()
    {
        // The cold arm rebuilds the whole component set every cycle.
        if (!bWarm && !Fixture.Chatbot.IsValid())
        {
            if (!SpawnFixture())
            {
                return true;
            }
        }

        UConvaiChatbotComponent* Chatbot = Fixture.Chatbot.Get();
        if (!Chatbot)
        {
            FailReason = FString::Printf(TEXT("cycle %d: chatbot component went away"), Cycle);
            return true;
        }

        if (!bStartRequested)
        {
            Chatbot->StartSession();
            bStartRequested = true;
            Recorder->Record(TEXT("start_session"), FString::Printf(TEXT("cycle=%d"), Cycle));
            return false;
        }

        if (Fixture.IsChatbotConnected())
        {
            Latency.Mark(bWarm && Cycle > 0 ? TEXT("warm_connect_ms")
                                            : TEXT("cold_connect_ms"));
            Recorder->Record(TEXT("connected"), FString::Printf(TEXT("cycle=%d"), Cycle));
            EnterPhase(EPhase::Settle);
            return false;
        }

        if (InPhaseSeconds > kConnectTimeoutSeconds)
        {
            const UConvaiChatbotComponent* Bot = Fixture.Chatbot.Get();
            FailReason = FString::Printf(
                TEXT("cycle %d: no Connected state within %.0f s; the component is still %s"),
                Cycle, kConnectTimeoutSeconds,
                Bot ? StateName(Bot->GetChatbotConnectionState()) : TEXT("<destroyed>"));
            return true;
        }
        return false;
    }

    bool PollSettle()
    {
        if (InPhaseSeconds < kSettleSeconds)
        {
            return false;
        }
        EnterPhase(EPhase::Disconnect);
        return false;
    }

    bool PollDisconnect()
    {
        UConvaiChatbotComponent* Chatbot = Fixture.Chatbot.Get();
        if (!Chatbot)
        {
            FailReason = FString::Printf(TEXT("cycle %d: chatbot went away before StopSession"),
                                         Cycle);
            return true;
        }

        if (!bStopRequested)
        {
            Chatbot->StopSession();
            bStopRequested = true;
            Recorder->Record(TEXT("stop_session"), FString::Printf(TEXT("cycle=%d"), Cycle));
            return false;
        }

        if (Chatbot->GetChatbotConnectionState() != EC_ConnectionState::Connected)
        {
            Latency.Mark(TEXT("disconnect_ms"));
            Recorder->Record(TEXT("disconnected"), FString::Printf(TEXT("cycle=%d"), Cycle));
            ++Cycle;
            bStartRequested = false;
            bStopRequested = false;

            // The cold arm proves a fresh component connects; the warm arm
            // proves the same one does. Only the cold arm rebuilds.
            if (!bWarm)
            {
                Fixture.Destroy();
            }

            if (Cycle >= Cycles)
            {
                EnterPhase(EPhase::Done);
                return true;
            }
            EnterPhase(EPhase::Connect);
            return false;
        }

        if (InPhaseSeconds > kDisconnectTimeoutSeconds)
        {
            FailReason = FString::Printf(
                TEXT("cycle %d: StopSession left the component reporting Connected after %.0f s"),
                Cycle, kDisconnectTimeoutSeconds);
            return true;
        }
        return false;
    }

    const TCHAR* ScenarioName = nullptr;
    int32 Cycles = 1;
    bool bWarm = false;

    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    FConvaiTestFixture Fixture;
    FConvaiTestLatencyTracker Latency;

    EPhase Phase = EPhase::Done;
    float InPhaseSeconds = 0.0f;
    int32 Cycle = 0;
    bool bStartRequested = false;
    bool bStopRequested = false;
    bool bSetupFailed = false;
    FString FailReason;
};

static struct FConvaiSessionLifecycleRegistrar
{
    FConvaiSessionLifecycleRegistrar()
    {
        ConvaiTestRegistry::Register(TEXT("session_connect_disconnect"),
                                     []() -> TSharedRef<FConvaiTestScenario> {
                                         return MakeShared<FConvaiSessionLifecycleScenario>(
                                             TEXT("session_connect_disconnect"), 1, false);
                                     });
        ConvaiTestRegistry::Register(TEXT("session_reconnect_warm"),
                                     []() -> TSharedRef<FConvaiTestScenario> {
                                         return MakeShared<FConvaiSessionLifecycleScenario>(
                                             TEXT("session_reconnect_warm"), 2, true);
                                     });
    }
} GConvaiSessionLifecycleRegistrar;
