// Copyright 2022 Convai Inc. All Rights Reserved.

// F19 — does the plugin route a capture component it adopted?
//
// UConvaiPlayerComponent assigns /ConvAI/Submixes/AudioInput in
// OnComponentCreated, to the UConvaiAudioCaptureComponent it constructs there
// (ConvaiPlayerComponent.cpp:128). SetAudioCaptureComponent, which is the whole
// of the adoption path, sets the object and the interface and nothing else
// (:477-495). So a third party plugging into the documented extension point
// gets discovery without routing, and their microphone renders into the default
// chain: audible through the speakers, and inside Reference Audio, where F12
// says a canceller destroys the player it can hear.
//
// mic_in_reference_audio_control measures what that costs — an unrouted mic
// reaches the master mix at -10.1 dB against -105.9 dB routed. This scenario
// asks the structural question instead, because the structural answer names the
// line to change and does not depend on anything being audible.
//
// The Virtual Mic clears its own SoundSubmix here. It assigns one in its
// constructor precisely because this defect exists, and leaving that in place
// would test the workaround rather than the plugin.
//
// The control is that /ConvAI/Submixes/AudioInput loads in the same run. The
// plugin's own capture component would be the better one, but
// UConvaiAudioCaptureComponent does not implement IConvaiAudioCaptureInterface
// (ConvaiAudioCaptureComponent.h:101) and is held in a private member of a class
// with no CONVAI_API, so it is unreachable from here by every route.

#include "ConvaiPlayerComponent.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTests.h"
#include "ConvaiVirtualMicComponent.h"
#include "Interface/ConvaiAudioCaptureInterface.h"

#include "Components/SceneComponent.h"
#include "Components/SynthComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundSubmix.h"

class FConvaiAdoptedCaptureRoutingScenario : public FConvaiTestScenario
{
public:
    static const TCHAR* StaticName() { return TEXT("adopted_capture_component_routing"); }
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

        // Registered before the player component, which is what puts it at index
        // 0 of GetComponentsByInterface and therefore what FindFirstAudioCaptureComponent
        // adopts. Same ordering constraint a customer has.
        UConvaiVirtualMicComponent* Mic =
            NewObject<UConvaiVirtualMicComponent>(SpawnedOwner, TEXT("VirtualMic"));
        Mic->SoundSubmix = nullptr;
        Mic->RegisterComponent();
        VirtualMic = Mic;

        UConvaiPlayerComponent* Player =
            NewObject<UConvaiPlayerComponent>(SpawnedOwner, TEXT("ConvaiPlayer"));
        Player->RegisterComponent();
        PlayerComponent = Player;

        // Something has to be queued or the synth renders nothing and adoption
        // cannot be observed through IsRunning.
        VirtualMic->EnqueueTone(440.0f, 2.0f);
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

        // Expected to be 1. UConvaiAudioCaptureComponent derives straight from
        // USynthComponent and does not implement IConvaiAudioCaptureInterface
        // (ConvaiAudioCaptureComponent.h:101), so the plugin's own capture
        // component is never discoverable this way and cannot serve as the
        // control. Recorded because a change in this number means the discovery
        // set changed under the scenario.
        int32 CaptureComponents = 0;
        if (AActor* OwnerActor = Owner.Get())
        {
            CaptureComponents =
                OwnerActor->GetComponentsByInterface(UConvaiAudioCaptureInterface::StaticClass())
                    .Num();
        }

        // The control. The assertion below is "the plugin did not assign
        // /ConvAI/Submixes/AudioInput"; this proves the thing it would have
        // assigned exists and loads at the moment of the assertion. Without it,
        // a missing Content folder (F6) and an unrouted adoption path produce
        // the same failure and get the same wrong fix.
        const bool bSubmixAssetResolves =
            LoadObject<USoundSubmixBase>(nullptr, TEXT("/ConvAI/Submixes/AudioInput.AudioInput"))
            != nullptr;
        const bool bAdoptedRouted = VirtualMic.IsValid() && VirtualMic->GetSubmix() != nullptr;

        Result.Metrics.Add(TEXT("capture_components"), CaptureComponents);
        Result.Metrics.Add(TEXT("running_before_start"), bRunningBeforeStart ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("adopted"), bObservedRunning ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("submix_asset_resolves"), bSubmixAssetResolves ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("adopted_component_routed"), bAdoptedRouted ? 1.0 : 0.0);

        // Still no teardown, and the actor leaks into the rest of the suite.
        // Destroy() routes through UConvaiPlayerComponent::EndPlay, which calls
        // FinishRecording() while IsRecording (ConvaiPlayerComponent.cpp:453-454).
        // F20's first crash on that path is fixed and this line was restored to
        // prove it; a second one is behind it, an access violation further down
        // the same function. See F20. Until that is understood, no scenario that
        // records can release what it spawned.

        if (bRunningBeforeStart)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("the Virtual Mic was already running before the plugin was asked to record, so "
                     "adoption cannot be established and the routing question does not arise");
            return Result;
        }

        if (!bObservedRunning)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("the player component never adopted the Virtual Mic, so this scenario is "
                     "asking about the routing of a component the plugin never took — see "
                     "virtual_mic_adoption");
            return Result;
        }

        if (!bSubmixAssetResolves)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("/ConvAI/Submixes/AudioInput did not resolve in this run, so the plugin had "
                     "nothing to assign and this cannot distinguish an unrouted adoption path from "
                     "a missing Content folder - that is F6");
            return Result;
        }

        if (!bAdoptedRouted)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("adopted-capture-component-unrouted");
            Finding.Summary =
                TEXT("a capture component supplied through IConvaiAudioCaptureInterface is adopted "
                     "but never assigned a SoundSubmix, so a customer's microphone renders into the "
                     "master mix — audibly, and inside Reference Audio");
            Finding.Evidence = FString::Printf(
                TEXT("UConvaiPlayerComponent adopted a component implementing "
                     "IConvaiAudioCaptureInterface (%d such on the actor) and started it, and that "
                     "component's SoundSubmix is still null. /ConvAI/Submixes/AudioInput loads in "
                     "this same run, so the asset is present. OnComponentCreated assigns it at "
                     "ConvaiPlayerComponent.cpp:128, to the component the plugin constructs "
                     "itself; SetAudioCaptureComponent, which is the whole adoption path, assigns "
                     "nothing. Measured cost in mic_in_reference_audio_control: -10.1 dB to the "
                     "master submix against -105.9 dB for the routed path"),
                CaptureComponents);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.bPassed = false;
            Result.FailReason = TEXT("the adopted capture component was left unrouted");
            return Result;
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
    float Elapsed = 0.0f;
};

CONVAI_REGISTER_SCENARIO(FConvaiAdoptedCaptureRoutingScenario)
