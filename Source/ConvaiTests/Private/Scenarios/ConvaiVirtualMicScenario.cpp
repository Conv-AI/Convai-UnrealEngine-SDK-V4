// Copyright 2022 Convai Inc. All Rights Reserved.

// Issue 04 — does the plugin adopt the Virtual Mic?
//
// Everything later in the plan assumes scripted audio enters at the same seam a
// real microphone does. If the player component quietly kept using real capture,
// every audio scenario would run against a silent device and pass, which issue
// 04 calls out as worse than having no test at all.
//
// The assertion is mechanical rather than a log scrape: the plugin calls
// IConvaiAudioCaptureInterface::Start() on whichever component it adopted, so a
// Virtual Mic that reports running without the scenario ever having started it
// was started by the plugin. Nothing else in the process can have done it.
//
// No backend. This tests the discovery seam, not a conversation.

#include "ConvaiPlayerComponent.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTests.h"
#include "ConvaiVirtualMicComponent.h"
#include "Interface/ConvaiAudioCaptureInterface.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

class FConvaiVirtualMicAdoptionScenario : public FConvaiTestScenario
{
public:
    static const TCHAR* StaticName() { return TEXT("virtual_mic_adoption"); }
    virtual const TCHAR* Name() const override { return StaticName(); }
    virtual double DeadlineSeconds() const override { return 15.0; }
    virtual bool RequiresLiveConnection() const override { return false; }

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

        // Spawned at runtime, so the suite needs no level content.
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

        // Order matters: the Virtual Mic has to exist on the actor before the
        // player component looks for a capture component, which is the same
        // constraint a customer supplying their own capture component has.
        UConvaiVirtualMicComponent* Mic =
            NewObject<UConvaiVirtualMicComponent>(SpawnedOwner, TEXT("VirtualMic"));
        Mic->RegisterComponent();
        VirtualMic = Mic;

        UConvaiPlayerComponent* Player =
            NewObject<UConvaiPlayerComponent>(SpawnedOwner, TEXT("ConvaiPlayer"));
        Player->RegisterComponent();
        PlayerComponent = Player;

        Recorder->Record(TEXT("components_registered"));

        // Queue audio before capture starts so the very first OnGenerateAudio
        // has something to emit; an empty queue would render silence and make
        // "adopted but starved" look like "not adopted".
        VirtualMic->EnqueueTone(440.0f, 2.0f);
        StartedWithPending = VirtualMic->PendingSamples();

        // Negative control: assert the mic is NOT running before the plugin is
        // asked for anything. Without this, a component that self-started would
        // satisfy the adoption assertion below for the wrong reason.
        bRunningBeforeStart = VirtualMic->IsRunning();
        Recorder->Record(TEXT("running_before_start"),
                         bRunningBeforeStart ? TEXT("true") : TEXT("false"));

        PlayerComponent->StartRecording();
        Recorder->Record(TEXT("start_recording_called"));
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }
        Elapsed += DeltaSeconds;

        if (VirtualMic.IsValid() && VirtualMic->IsRunning())
        {
            bObservedRunning = true;
        }

        // Give the audio thread time to spin the synth up and drain some of the
        // queue; adoption is not instantaneous.
        return Elapsed >= 3.0f;
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

        const int32 Pending = VirtualMic.IsValid() ? VirtualMic->PendingSamples() : -1;
        const int64 Emitted = VirtualMic.IsValid() ? VirtualMic->EmittedSamples() : -1;

        // The input to the plugin's adoption decision, not just its outcome.
        // A fix agent given only the outcome knows a component was not adopted
        // and cannot tell "none was found" from "one was found and rejected",
        // which are different bugs in different functions. Measured here rather
        // than asked of the plugin because the question is what was on the
        // actor, and that is the scenario's own setup.
        int32 Candidates = -1;
        if (const AActor* OwnerActor = Owner.Get())
        {
            Candidates = const_cast<AActor*>(OwnerActor)
                             ->GetComponentsByInterface(UConvaiAudioCaptureInterface::StaticClass())
                             .Num();
        }

        Result.Metrics.Add(TEXT("adoption_candidates"), Candidates);
        Result.Metrics.Add(TEXT("queued_samples"), StartedWithPending);
        Result.Metrics.Add(TEXT("pending_samples"), Pending);
        Result.Metrics.Add(TEXT("emitted_samples"), static_cast<double>(Emitted));
        Result.Metrics.Add(TEXT("running_before_start"), bRunningBeforeStart ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("observed_running"), bObservedRunning ? 1.0 : 0.0);

        if (PlayerComponent.IsValid())
        {
            Result.Metrics.Add(TEXT("player_is_streaming"),
                               PlayerComponent->GetIsStreaming() ? 1.0 : 0.0);
        }

        if (bRunningBeforeStart)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("the Virtual Mic was already running before the plugin was asked to record — "
                     "the adoption check below would pass regardless of adoption");
            return Result;
        }

        if (!bObservedRunning)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("virtual-mic-not-adopted");
            Finding.Summary =
                TEXT("the player component did not adopt the Virtual Mic; scripted audio never "
                     "reaches the plugin's capture path");
            // Every clause is a fact about state, not about sequence. An
            // earlier version said the mic was registered "before
            // UConvaiPlayerComponent", which is true of the setup and reads as
            // a claim that ordering is the mechanism -- a coherent and wrong
            // hypothesis that cost a fix agent most of its search.
            Finding.Evidence = FString::Printf(
                TEXT("the owning actor carried %d component(s) implementing "
                     "IConvaiAudioCaptureInterface, one of them a UConvaiVirtualMicComponent. "
                     "UConvaiPlayerComponent::StartRecording() was called and Start() was never "
                     "invoked on the interface over %.1f s, so the component was on the actor and "
                     "was not adopted. Queued %d samples, %d still pending."),
                Candidates, Elapsed, StartedWithPending, Pending);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.bPassed = false;
            Result.FailReason = TEXT("Virtual Mic was not adopted by the player component");
            return Result;
        }

        // Adopted. Whether the queue actually drained is a separate question —
        // the synth only renders when something pulls on it — so it is reported
        // rather than asserted until issue 05 needs it.
        if (Emitted <= 0)
        {
            Result.Hypotheses.Add(
                TEXT("the Virtual Mic was adopted and started but OnGenerateAudio never ran, so "
                     "nothing pulled on the synth. Check the submix routing before relying on "
                     "SpeakWav in a later scenario."));
        }

        Result.bPassed = true;
        return Result;
    }

private:
    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;

    TWeakObjectPtr<AActor> Owner;
    TWeakObjectPtr<UConvaiVirtualMicComponent> VirtualMic;
    TWeakObjectPtr<UConvaiPlayerComponent> PlayerComponent;

    bool bSetupFailed = false;
    bool bRunningBeforeStart = false;
    bool bObservedRunning = false;
    int32 StartedWithPending = 0;
    float Elapsed = 0.0f;
};

CONVAI_REGISTER_SCENARIO(FConvaiVirtualMicAdoptionScenario)
