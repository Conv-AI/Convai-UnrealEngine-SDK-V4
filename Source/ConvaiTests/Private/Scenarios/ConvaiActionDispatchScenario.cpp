// Copyright 2022 Convai Inc. All Rights Reserved.

// Declare an action, ask for it, get it back through OnActionReceivedEvent_V2.
//
// The character advertises one action -- TestAction with a String parameter
// "option" -- and the player asks it to execute option A. The assertion is that
// the plugin delivered a batch naming TestAction with option=A, which covers
// what sits between the wire and a Blueprint's action handler: action_config
// rendered at connect, the response parsed against the connect-time snapshot,
// the value mapped back to its declared parameter name, and the hop to the
// game thread. Whether the model chooses the action is the model's business,
// so a wrong or missing choice is reported as its own finding rather than as a
// plugin failure to deliver one.
//
// Ported from the in-plugin UConvaiTest_ActionResponse; its Follow, Show
// Routine, MostrarRutina and Recomendar producto arms are dropped because they
// were customer-character-specific prompt variants.

#include "ConvaiChatbotComponent.h"
#include "ConvaiDefinitions.h"
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

    // Connected is the handshake; the realtime data channel opens a beat
    // later, and a prompt sent into that gap is dropped, which would read as
    // the action never arriving.
    constexpr float kTransportGraceSeconds = 2.0f;

    // The model has to answer and pick the action. Generous because a slow
    // model must not read as a broken plugin.
    constexpr float kActionTimeoutSeconds = 45.0f;

    const TCHAR* kPrompt = TEXT("execute option A.");
    const TCHAR* kActionName = TEXT("TestAction");
    const TCHAR* kParamName = TEXT("option");
    const TCHAR* kExpectedValue = TEXT("A");

    const FConvaiResultAction* FindAction(const TArray<FConvaiResultAction>& Actions)
    {
        return Actions.FindByPredicate(
            [](const FConvaiResultAction& Action)
            { return Action.Action.Equals(kActionName, ESearchCase::IgnoreCase); });
    }

    // Keyed by the declared name: the plugin's parser assigns the key, not the
    // server, so an exact lookup is the assertion.
    bool ReadStringParameter(const FConvaiResultAction& Action, FString& OutValue)
    {
        const FConvaiResultParam* Param = Action.Parameters.Find(kParamName);
        if (!Param)
        {
            return false;
        }
        OutValue = Param->StringValue.TrimStartAndEnd();
        return true;
    }

    FString ActionNames(const TArray<FConvaiResultAction>& Actions)
    {
        TArray<FString> Names;
        for (const FConvaiResultAction& Action : Actions)
        {
            Names.Add(Action.Action);
        }
        return FString::Join(Names, TEXT(","));
    }
}

class FConvaiActionDispatchScenario : public FConvaiTestScenario
{
public:
    static const TCHAR* StaticName() { return TEXT("action_dispatch"); }
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

        UConvaiChatbotComponent* Chatbot = Fixture.Chatbot.Get();
        if (!Chatbot)
        {
            Recorder->Record(TEXT("setup_failed"), TEXT("no chatbot after spawn"));
            bSetupFailed = true;
            return;
        }

        // Before StartSession: the action list is frozen into the connect
        // payload, so anything added afterwards is never advertised.
        Chatbot->EnvironmentData.bEnableActions = true;
        Chatbot->EnvironmentData.Actions.Reset();
        Chatbot->EnvironmentData.Actions.Add(FConvaiAction(
            kActionName,
            TEXT("Call this action whenever the user asks to execute option A or option B. "
                 "Calling it is required even if you also answer verbally. Always set option "
                 "to the exact requested value."),
            {FConvaiActionParam(
                kParamName,
                TEXT("Required parameter. Return exactly A or B. Never omit this value."),
                EConvaiActionParamType::String)}));
        Chatbot->OnActionReceivedEvent_V2.AddDynamic(
            Fixture.Sink.Get(), &UConvaiTestEventSink::HandleActionsReceived);
        Recorder->Record(TEXT("action_declared"), kActionName);

        Chatbot->StartSession();
        ConvaiTestSteps::SetTalkTargets(Fixture.Player.Get(), {Chatbot});
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
            if (!bConnected)
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
                bConnected = true;
                ConnectedAtSeconds = Elapsed;
                Latency.Mark(TEXT("connect_ms"));
                return false;
            }
            if (Elapsed - ConnectedAtSeconds < kTransportGraceSeconds)
            {
                return false;
            }

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

        const TArray<UConvaiTestEventSink::FActionBatch> Batches = Sink->ActionBatches();
        for (; BatchesSeen < Batches.Num(); ++BatchesSeen)
        {
            const TArray<FConvaiResultAction>& Actions = Batches[BatchesSeen].Actions;
            Recorder->Record(TEXT("actions_received"), ActionNames(Actions));
            if (FindAction(Actions))
            {
                Latency.Mark(TEXT("action_ms"));
                ++BatchesSeen;
                return true;
            }
        }

        // A batch without TestAction is not a verdict yet: the model may act
        // in a later turn, and only the deadline decides it never chose.
        if (Elapsed - SentAtSeconds > kActionTimeoutSeconds)
        {
            return true;
        }
        return false;
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;
        UConvaiTestEventSink* Sink = Fixture.Sink.Get();

        const TArray<UConvaiTestEventSink::FActionBatch> Batches =
            Sink ? Sink->ActionBatches() : TArray<UConvaiTestEventSink::FActionBatch>();
        const int32 Failures = Sink ? Sink->FailureCount() : 0;

        const FConvaiResultAction* Matched = nullptr;
        TArray<FString> NamesSeen;
        int32 TestActionCount = 0;
        for (const UConvaiTestEventSink::FActionBatch& Batch : Batches)
        {
            for (const FConvaiResultAction& Action : Batch.Actions)
            {
                NamesSeen.AddUnique(Action.Action);
            }
            if (const FConvaiResultAction* Found = FindAction(Batch.Actions))
            {
                ++TestActionCount;
                Matched = Matched ? Matched : Found;
            }
        }
        const FString NamesSeenList =
            NamesSeen.IsEmpty() ? FString(TEXT("<none>")) : FString::Join(NamesSeen, TEXT(","));

        Result.Metrics.Add(TEXT("connected"), bConnected ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("actions_received"), Batches.Num());
        Result.Metrics.Add(TEXT("test_action_batches"), TestActionCount);
        Result.Metrics.Add(TEXT("failures"), Failures);
        for (const TPair<FString, double>& Pair : Latency.Snapshot())
        {
            Result.Metrics.Add(Pair.Key, Pair.Value);
        }

        Result.MetricNotes.Add(
            TEXT("action_ms"),
            TEXT("Covers: the plugin decoding the action the model chose and dispatching it to "
                 "OnActionReceivedEvent_V2. Does NOT cover: whether the model always chooses "
                 "correctly. A model regression shows as wrong-action-returned, not as a "
                 "plugin bug."));

        if (Sink)
        {
            Recorder->Record(TEXT("actions_seen"), NamesSeenList);
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
            Result.FailReason = FString::Printf(
                TEXT("%d OnFailureEvent(s) during an action exchange"), Failures);
            return Result;
        }

        if (Batches.Num() == 0)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("no-action-for-declared-action");
            Finding.Summary = TEXT("a prompt naming a declared action produced no "
                                   "OnActionReceivedEvent_V2 at all");
            Finding.Evidence = FString::Printf(
                TEXT("The session reached Connected with actions enabled and %s(%s) declared, "
                     "and SendText was called with \"%s\". Within %.0f s "
                     "OnActionReceivedEvent_V2 never fired. %d failure event(s) fired."),
                kActionName, kParamName, kPrompt, kActionTimeoutSeconds, Failures);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = TEXT("no action came back for a declared action");
            return Result;
        }

        if (!Matched)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("wrong-action-returned");
            Finding.Summary = TEXT("actions arrived but none was the declared one the prompt "
                                   "asked for");
            Finding.Evidence = FString::Printf(
                TEXT("OnActionReceivedEvent_V2 fired %d time(s) within %.0f s of \"%s\" and no "
                     "action was named %s. Names seen: %s."),
                Batches.Num(), kActionTimeoutSeconds, kPrompt, kActionName, *NamesSeenList);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = FString::Printf(TEXT("%s never arrived; got %s"), kActionName,
                                                *NamesSeenList);
            return Result;
        }

        FString Value;
        if (!ReadStringParameter(*Matched, Value))
        {
            TArray<FString> Keys;
            Matched->Parameters.GetKeys(Keys);

            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("action-parameter-missing");
            Finding.Summary = TEXT("the declared action arrived without its declared parameter");
            Finding.Evidence = FString::Printf(
                TEXT("%s arrived (raw \"%s\") with %d parameter(s) [%s] and none named %s."),
                kActionName, *Matched->ActionString, Matched->Parameters.Num(),
                *FString::Join(Keys, TEXT(",")), kParamName);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = FString::Printf(TEXT("%s arrived without '%s'"), kActionName,
                                                kParamName);
            return Result;
        }

        if (!Value.Equals(kExpectedValue, ESearchCase::IgnoreCase))
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("action-parameter-wrong");
            Finding.Summary = TEXT("the declared action arrived with a parameter value other "
                                   "than the one the prompt asked for");
            Finding.Evidence = FString::Printf(
                TEXT("%s arrived (raw \"%s\") with %s=\"%s\"; the prompt \"%s\" asked for "
                     "\"%s\"."),
                kActionName, *Matched->ActionString, kParamName, *Value, kPrompt,
                kExpectedValue);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = FString::Printf(TEXT("%s %s was '%s', expected '%s'"),
                                                kActionName, kParamName, *Value,
                                                kExpectedValue);
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
    float ConnectedAtSeconds = 0.0f;
    float SentAtSeconds = 0.0f;
    int32 BatchesSeen = 0;
    bool bConnected = false;
    bool bSent = false;
    bool bSetupFailed = false;
    FString FailReason;
};

CONVAI_REGISTER_SCENARIO(FConvaiActionDispatchScenario)
