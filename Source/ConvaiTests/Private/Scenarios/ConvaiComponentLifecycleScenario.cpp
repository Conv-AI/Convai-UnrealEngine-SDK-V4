// Copyright 2022 Convai Inc. All Rights Reserved.

// Spawn the component set, tear it down, repeat. No network.
//
// The in-engine counterpart of the DLL harness's lifecycle_basic, which does
// Init -> Cleanup twice as a regression guard for the FFI singleton bug class.
// The plugin has its own version of that class and the DLL's test cannot see
// it: UConvaiPlayerComponent creates and registers a capture component in
// OnComponentCreated, adopts an alternative during registration, and hands
// state to a process-wide subsystem that outlives every actor. A leak or a
// double-free there survives the DLL being perfectly well behaved.
//
// Worth having offline for a second reason. Every other live scenario needs a
// backend, an API key and a character that answers, so a bad afternoon at the
// service turns the whole suite red and a real teardown regression lands in the
// middle of that noise unnoticed. This one is red only when the plugin is.

#include "ConvaiPlayerComponent.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestEventSink.h"
#include "ConvaiTestFixture.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTests.h"
#include "ConvaiVirtualMicComponent.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Interface/ConvaiAudioCaptureInterface.h"

namespace
{
    constexpr int32 kIterations = 3;
}

class FConvaiComponentLifecycleScenario : public FConvaiTestScenario
{
public:
    static const TCHAR* StaticName() { return TEXT("component_lifecycle"); }
    virtual const TCHAR* Name() const override { return StaticName(); }

    virtual double DeadlineSeconds() const override { return 30.0; }

    // The whole point. Nothing here connects.
    virtual bool RequiresLiveConnection() const override { return false; }

    virtual void Start(UWorld* InWorld, FConvaiTestEventRecorder& InRecorder) override
    {
        World = InWorld;
        Recorder = &InRecorder;
        if (!World)
        {
            Recorder->Record(TEXT("setup_failed"), TEXT("no world"));
            bSetupFailed = true;
        }
    }

    // One iteration per tick rather than a loop. Destroy() marks an actor
    // pending kill and the components are not unregistered until the world
    // ticks, so a loop would build the next set alongside the previous one and
    // measure a condition no game produces.
    virtual bool Poll(float /*DeltaSeconds*/) override
    {
        if (bSetupFailed || Iteration >= kIterations)
        {
            return true;
        }

        FConvaiTestFixture Fixture;
        FConvaiTestFixture::FOptions Options;
        // No character: nothing connects, and requiring one would make an
        // offline scenario fail on a missing credential.
        Options.CharacterID = TEXT("");
        FString Error;
        if (!Fixture.Spawn(World, *Recorder, Options, Error))
        {
            FailReason = FString::Printf(TEXT("iteration %d: spawn failed: %s"), Iteration,
                                         *Error);
            return true;
        }

        if (!Fixture.Player.IsValid() || !Fixture.Chatbot.IsValid() || !Fixture.Mic.IsValid())
        {
            FailReason = FString::Printf(
                TEXT("iteration %d: a component did not survive its own registration"), Iteration);
            Fixture.Destroy();
            return true;
        }

        // The adoption seam, checked every iteration. The player discovers a
        // capture component during registration; if a previous iteration left
        // a registered component behind on a destroyed actor, or if discovery
        // stops finding one after the first pass, this is where it shows.
        int32 Candidates = 0;
        if (AActor* OwnerActor = Fixture.Owner.Get())
        {
            Candidates = OwnerActor
                             ->GetComponentsByInterface(UConvaiAudioCaptureInterface::StaticClass())
                             .Num();
        }
        AdoptionCandidates.Add(Candidates);

        const int32 Failures = Fixture.Sink.IsValid() ? Fixture.Sink->FailureCount() : 0;
        if (Failures > 0)
        {
            FailReason = FString::Printf(
                TEXT("iteration %d: %d failure event(s) with no session ever started"), Iteration,
                Failures);
            Fixture.Destroy();
            return true;
        }

        Recorder->Record(TEXT("lifecycle_iteration"),
                         FString::Printf(TEXT("i=%d candidates=%d"), Iteration, Candidates));
        Fixture.Destroy();
        ++Iteration;
        return false;
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;
        Result.Metrics.Add(TEXT("iterations_completed"), static_cast<double>(Iteration));

        int32 MinCandidates = MAX_int32;
        int32 MaxCandidates = 0;
        for (int32 Count : AdoptionCandidates)
        {
            MinCandidates = FMath::Min(MinCandidates, Count);
            MaxCandidates = FMath::Max(MaxCandidates, Count);
        }
        Result.Metrics.Add(TEXT("adoption_candidates_min"),
                           AdoptionCandidates.Num() ? MinCandidates : 0);
        Result.Metrics.Add(TEXT("adoption_candidates_max"), MaxCandidates);

        Result.MetricNotes.Add(
            TEXT("adoption_candidates_max"),
            TEXT("Covers: components accumulating across teardowns -- a count that climbs with "
                 "the iteration means a destroyed actor's capture component is still registered. "
                 "Does NOT cover: leaks with no component identity, such as a subsystem entry or "
                 "a native thread the actor owned."));

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
        if (Iteration < kIterations)
        {
            Result.FailReason = FString::Printf(
                TEXT("only %d of %d iterations completed before the deadline"), Iteration,
                kIterations);
            return Result;
        }

        // Constant, not merely non-zero. One Virtual Mic per actor is the
        // condition; a climbing count is the leak this scenario exists for.
        if (MaxCandidates != MinCandidates)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("capture-components-accumulate-across-teardown");
            Finding.Summary = TEXT("the number of capture components discoverable on a freshly "
                                   "spawned actor changed between identical iterations");
            Finding.Evidence = FString::Printf(
                TEXT("%d iterations each spawned one actor with one Virtual Mic and destroyed it. "
                     "GetComponentsByInterface(UConvaiAudioCaptureInterface) returned between %d "
                     "and %d across them, so a previous iteration's component is still reachable "
                     "after its actor was destroyed."),
                Iteration, MinCandidates, MaxCandidates);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = FString::Printf(
                TEXT("capture component count varied from %d to %d across identical iterations"),
                MinCandidates, MaxCandidates);
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

private:
    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    bool bSetupFailed = false;
    FString FailReason;
    int32 Iteration = 0;
    TArray<int32> AdoptionCandidates;
};

CONVAI_REGISTER_SCENARIO(FConvaiComponentLifecycleScenario)
