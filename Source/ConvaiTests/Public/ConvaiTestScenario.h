// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FConvaiTestEventRecorder;
class UWorld;

// A scenario's outcome. Findings are separate from the pass flag on purpose:
// an invariant monitor can produce a finding on a scenario that passed, and the
// report's consumer is a fix agent that needs both.
struct CONVAITESTS_API FConvaiScenarioFinding
{
    // Stable across runs and machines — the report deduplicates by this, so one
    // cause failing forty scenarios is one finding with forty occurrences.
    FString DedupKey;

    FString Summary;

    // Mechanically derived only. "RemoveClient never called for client 0x…,
    // attach 3, detach 2" qualifies; "probably a lifetime bug" does not — that
    // belongs in Hypotheses.
    FString Evidence;

    // Numbers the fix agent can diff between runs.
    TMap<FString, double> Metrics;
};

struct CONVAITESTS_API FConvaiScenarioResult
{
    bool bPassed = false;

    // The scenario never started -- an input it needs was not supplied, so it
    // graded nothing. Reported in its own column and kept out of pass_rate,
    // because a missing character reads as twelve product failures otherwise.
    // The sweep still exits non-zero: not-run is not a pass either.
    bool bSetupFailed = false;

    FString FailReason;
    double ElapsedSeconds = 0.0;

    TArray<FConvaiScenarioFinding> Findings;

    // Unverified leads. Kept in their own array so the agent is never handed a
    // guess wearing a finding's clothes.
    TArray<FString> Hypotheses;

    TMap<FString, double> Metrics;

    // What a metric can and cannot separate, keyed by metric name. The report's
    // consumer is an agent that will not read FINDINGS.md, and F33 is what that
    // costs: the most AEC-looking number in the report cannot detect a broken
    // canceller, and nothing in the report said so.
    TMap<FString, FString> MetricNotes;
};

class CONVAITESTS_API FConvaiTestScenario
{
public:
    virtual ~FConvaiTestScenario() = default;

    virtual const TCHAR* Name() const = 0;

    // The watchdog aborts the run at twice this.
    virtual double DeadlineSeconds() const { return 30.0; }

    virtual bool RequiresLiveConnection() const { return true; }

    // Whether the scenario needs the character from TestCharacterID. Almost
    // always the same question as RequiresLiveConnection, and separate for the
    // one scenario that connects on a character it invents. Reported by
    // convai.tests.List so the runner can refuse a sweep that cannot run
    // rather than launch an editor per scenario to discover it twelve times.
    virtual bool RequiresTestCharacter() const { return RequiresLiveConnection(); }

    // Scenarios are driven from a ticker, not blocked on: the game thread has
    // to keep running for the engine to render audio at all, so a scenario that
    // slept would measure nothing. Start() arms the work, Poll() returns true
    // when finished.
    virtual void Start(UWorld* World, FConvaiTestEventRecorder& Recorder) = 0;
    virtual bool Poll(float DeltaSeconds) = 0;
    virtual FConvaiScenarioResult Finish() = 0;
};

namespace ConvaiTestRegistry
{
    using FScenarioFactory = TFunction<TSharedRef<FConvaiTestScenario>()>;

    CONVAITESTS_API void Register(const TCHAR* Name, FScenarioFactory Factory);
    CONVAITESTS_API const TArray<TPair<FString, FScenarioFactory>>& All();
}

// Linking the .cpp is enough to make a scenario visible to the runner, matching
// convai_harness's HARNESS_REGISTER_SCENARIO. No list to keep in sync.
#define CONVAI_REGISTER_SCENARIO(ScenarioClass)                                                    \
    static struct FConvaiScenarioRegistrar_##ScenarioClass                                         \
    {                                                                                              \
        FConvaiScenarioRegistrar_##ScenarioClass()                                                 \
        {                                                                                          \
            ConvaiTestRegistry::Register(ScenarioClass::StaticName(),                              \
                                         []() -> TSharedRef<FConvaiTestScenario>                   \
                                         { return MakeShared<ScenarioClass>(); });                 \
        }                                                                                          \
    } GConvaiScenarioRegistrar_##ScenarioClass;
