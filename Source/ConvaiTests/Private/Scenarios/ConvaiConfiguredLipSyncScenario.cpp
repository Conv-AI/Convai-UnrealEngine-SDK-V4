// Copyright 2022 Convai Inc. All Rights Reserved.

// Lip-sync on a character Blueprint: frames reach the face-sync component, the
// face mesh's curves move while the character speaks, and settle afterwards.
//
// Three claims because they fail separately. Frames can be applied to the
// component while the mesh stays still (no face-sync node in the AnimBP, a
// mode/rig mismatch); the mesh can move and never settle (the anim node's fade
// is what returns it to neutral -- the component keeps its last frame by
// design); and audio can play with no frame to show for it, which is F10.
// Everything is read from surface a game binds: OnFacialDataReady,
// HasPlayableFaceFrames, and the face mesh's own anim curves.
//
// Packet receipt is not observable. Blendshape packets stay out of the packet
// log and arrive on the transport thread with no public event, so the first
// applied frame is the earliest evidence a game could have either.

#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestEventSink.h"
#include "ConvaiTestFixture.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTestSteps.h"
#include "ConvaiTests.h"
#include "ConvaiUtils.h"

#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
    constexpr float kConnectTimeoutSeconds = 30.0f;

    // The utterance, the server closing the turn, and the curve fade after.
    constexpr float kReplyTimeoutSeconds = 90.0f;

    // Speech arrives in TTS chunks and GetIsTalking() drops between them, so a
    // gap is the end of the turn only once it has outlasted any chunk gap
    // seen on a live run. Turn-completed is not used: I5 loses it, and a turn
    // the host microphone provoked can complete before this one starts.
    constexpr float kEndOfSpeechQuietSeconds = 2.0f;

    // The anim node fades curves out over 0.8 s once frames run dry. Well
    // past that the face is stuck, not settling.
    constexpr float kNeutralTimeoutSeconds = 5.0f;

    // Long enough to produce frames worth measuring, closed enough to end.
    const TCHAR* kPrompt = TEXT("Count slowly from one to ten, then stop.");

    // The MetaHuman jaw control the plugin's face AnimBP drives: the one curve
    // every lip-synced utterance has to move.
    const FName kJawCurve(TEXT("CTRL_expressions_jawOpen"));

    // Rig-logic controls run 0..1. Anything a viewer would call a moving mouth
    // clears the first; a still face sits under the second.
    constexpr double kMinJawOpen = 0.05;
    constexpr double kNeutralJawOpen = 0.01;
}

class FConvaiConfiguredLipSyncScenario : public FConvaiTestScenario
{
public:
    static const TCHAR* StaticName() { return TEXT("configured_lipsync"); }
    virtual const TCHAR* Name() const override { return StaticName(); }
    virtual double DeadlineSeconds() const override { return 180.0; }
    virtual bool RequiresLiveConnection() const override { return true; }

    virtual void Start(UWorld* InWorld, FConvaiTestEventRecorder& InRecorder) override
    {
        World = InWorld;
        Recorder = &InRecorder;
        Latency.Begin();

        FString Error;
        FConvaiTestFixture::FOptions Options;
        if (!FConvaiTestFixture::HasCredentials(Error) ||
            !FConvaiTestFixture::HasConfiguredActorClass(Options.ActorClassPath, Error) ||
            !Fixture.Spawn(World, *Recorder, Options, Error))
        {
            Recorder->Record(TEXT("setup_failed"), Error);
            bSetupFailed = true;
            return;
        }

        USkeletalMeshComponent* Body = nullptr;
        USkeletalMeshComponent* Face = nullptr;
        UConvaiUtils::GetBodyAndFaceSkeletalMeshComponents(Fixture.Character.Get(), Body, Face);
        if (!Face || !Face->GetAnimInstance())
        {
            Recorder->Record(TEXT("setup_failed"),
                             TEXT("the character Blueprint has no face skeletal mesh running an "
                                  "Anim Blueprint, so applied lip-sync cannot be observed"));
            bSetupFailed = true;
            return;
        }
        // Nobody looks at a headless launch. Off-screen a mesh may skip its
        // animation, and the oracle would read a still face for a working plugin.
        for (USkeletalMeshComponent* Mesh : {Body, Face})
        {
            if (Mesh)
            {
                Mesh->VisibilityBasedAnimTickOption =
                    EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
            }
        }
        FaceMesh = Face;

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

        UConvaiChatbotComponent* Chatbot = Fixture.Chatbot.Get();
        UConvaiTestEventSink* Sink = Fixture.Sink.Get();
        USkeletalMeshComponent* Face = FaceMesh.Get();
        UAnimInstance* Anim = Face ? Face->GetAnimInstance() : nullptr;
        if (!Chatbot || !Sink || !Anim)
        {
            FailReason = TEXT("chatbot, sink or face mesh went away mid-run");
            return true;
        }

        const double Jaw = Anim->GetCurveValue(kJawCurve);

        if (Chatbot->GetIsTalking())
        {
            if (!bSpoke)
            {
                bSpoke = true;
                Latency.Mark(TEXT("bot_started_speaking_ms"));
                Recorder->Record(TEXT("bot_started_speaking"), FString());
            }
            // Audio resumed after a gap: that was a chunk boundary, not the end,
            // and neutral is measured again from the last chunk.
            if (QuietSinceSeconds >= 0.0f)
            {
                QuietSinceSeconds = -1.0f;
                NeutralAfterMs = -1.0;
                ++SpeechGaps;
            }
            TalkingSeconds += DeltaSeconds;

            if (!bSawFrame && Sink->FacialFrames() > 0)
            {
                bSawFrame = true;
                Latency.Mark(TEXT("face_first_frame_ms"));
            }
            for (const TPair<FName, float>& Weight : Chatbot->ConvaiGetFaceBlendshapes())
            {
                FaceMaxWeight = FMath::Max(FaceMaxWeight, static_cast<double>(Weight.Value));
            }
            if (!bSawCurve && Jaw >= kMinJawOpen)
            {
                bSawCurve = true;
                Latency.Mark(TEXT("mesh_first_curve_ms"));
                Recorder->Record(TEXT("face_moved"), FString::Printf(TEXT("jaw=%.3f"), Jaw));
            }
            MeshJawMax = FMath::Max(MeshJawMax, Jaw);

            if (Chatbot->HasPlayableFaceFrames())
            {
                StarvedSeconds = 0.0f;
            }
            else
            {
                StarvedSeconds += DeltaSeconds;
                StarvedMaxSeconds = FMath::Max(StarvedMaxSeconds, StarvedSeconds);
            }
        }
        else if (bSpoke)
        {
            if (QuietSinceSeconds < 0.0f)
            {
                QuietSinceSeconds = Elapsed;
                Recorder->Record(TEXT("audio_quiet"), FString::Printf(TEXT("jaw=%.3f"), Jaw));
            }
            const float Quiet = Elapsed - QuietSinceSeconds;
            if (NeutralAfterMs < 0.0 && Jaw < kNeutralJawOpen)
            {
                NeutralAfterMs = Quiet * 1000.0;
                Recorder->Record(TEXT("face_neutral"),
                                 FString::Printf(TEXT("%.0f ms after audio"), NeutralAfterMs));
            }
            if (Quiet > kEndOfSpeechQuietSeconds && NeutralAfterMs >= 0.0)
            {
                Latency.Mark(TEXT("bot_finished_speaking_ms"));
                return true;
            }
            if (Quiet > kNeutralTimeoutSeconds)
            {
                return true;
            }
        }

        return Elapsed - SentAtSeconds > kReplyTimeoutSeconds;
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;
        UConvaiTestEventSink* Sink = Fixture.Sink.Get();
        const int32 Frames = Sink ? Sink->FacialFrames() : 0;
        const int32 Failures = Sink ? Sink->FailureCount() : 0;
        const int32 DetailsFailures = Sink ? Sink->CharacterDataLoadFailures() : 0;

        Result.Metrics.Add(TEXT("connected"), bSent ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("bot_spoke"), bSpoke ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("bot_speech_ms"), TalkingSeconds * 1000.0);
        Result.Metrics.Add(TEXT("speech_gaps"), SpeechGaps);
        Result.Metrics.Add(TEXT("face_frames_applied"), Frames);
        Result.Metrics.Add(TEXT("face_max_weight"), FaceMaxWeight);
        Result.Metrics.Add(TEXT("mesh_jaw_max"), MeshJawMax);
        Result.Metrics.Add(TEXT("face_starvation_max_ms"), StarvedMaxSeconds * 1000.0);
        Result.Metrics.Add(TEXT("mesh_neutral_after_ms"), NeutralAfterMs);
        Result.Metrics.Add(TEXT("failures"), Failures);
        for (const TPair<FString, double>& Pair : Latency.Snapshot())
        {
            Result.Metrics.Add(Pair.Key, Pair.Value);
        }

        Result.MetricNotes.Add(
            TEXT("face_frames_applied"),
            TEXT("Covers: lip-sync frames reached the face-sync component and were applied to its "
                 "blendshape map, one OnFacialDataReady per animated tick. Does NOT cover: receipt "
                 "from the server, which has no public event, nor whether the mesh moved -- "
                 "mesh_jaw_max is that."));
        Result.MetricNotes.Add(
            TEXT("mesh_jaw_max"),
            TEXT("Covers: the face mesh's CTRL_expressions_jawOpen anim curve moved while the "
                 "character spoke, so the face-sync node in the Anim Blueprint resolved the chatbot "
                 "and applied its frames. Does NOT cover: how the lip-sync looks, or any other "
                 "curve."));
        Result.MetricNotes.Add(
            TEXT("face_starvation_max_ms"),
            TEXT("Longest stretch of audible speech with no playable face frame. Reported, not "
                 "asserted: F10 is open and its rate is unknown. Stalls shorter than the fallback "
                 "window never reach the log, so this is the only place they show."));
        Result.MetricNotes.Add(
            TEXT("mesh_neutral_after_ms"),
            TEXT("Time from the last audio chunk to the jaw curve dropping under 0.01 through the "
                 "anim node's starvation fade; -1 when it never did. Does NOT cover the "
                 "component's blendshape map, which keeps its last frame by design."));
        Result.MetricNotes.Add(
            TEXT("speech_gaps"),
            TEXT("Chunk boundaries where GetIsTalking() dropped and came back. Covers: how the "
                 "answer was delivered. Does NOT mean the face froze -- face_starvation_max_ms "
                 "is that."));

        Recorder->Record(TEXT("lipsync_summary"),
                         FString::Printf(TEXT("frames=%d max_weight=%.3f jaw_max=%.3f "
                                              "starved_max_ms=%.0f neutral_after_ms=%.0f gaps=%d"),
                                         Frames, FaceMaxWeight, MeshJawMax,
                                         StarvedMaxSeconds * 1000.0, NeutralAfterMs, SpeechGaps));
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
                Failures, DetailsFailures, TEXT("the lip-sync exchange"), Result.Metrics));
            Result.FailReason =
                FString::Printf(TEXT("%d OnFailureEvent(s) during the exchange"), Failures);
            return Result;
        }

        // Ordered so the report names the first thing that broke.
        if (!bSpoke)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("no-bot-speech-for-lipsync-prompt");
            Finding.Summary = TEXT("the character never became audible, so lip-sync had nothing "
                                   "to synchronise to");
            Finding.Evidence = FString::Printf(
                TEXT("The session reached Connected and SendText was called with \"%s\". Within "
                     "%.0f s GetIsTalking() never became true; %d face frame(s) were applied "
                     "meanwhile."),
                kPrompt, kReplyTimeoutSeconds, Frames);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));
            Result.FailReason = TEXT("the character never spoke");
            return Result;
        }
        if (Frames == 0)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("no-face-frames-applied-during-speech");
            Finding.Summary = TEXT("the character spoke but the face-sync component applied no "
                                   "lip-sync frame");
            Finding.Evidence = FString::Printf(
                TEXT("Audio played for %.0f ms and OnFacialDataReady never fired; "
                     "HasPlayableFaceFrames was false for %.0f ms at its longest. Either no "
                     "frames arrived or none was ever selected against the audio clock."),
                TalkingSeconds * 1000.0, StarvedMaxSeconds * 1000.0);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));
            Result.FailReason = TEXT("no lip-sync frames were applied while the character spoke");
            return Result;
        }
        if (MeshJawMax < kMinJawOpen)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("face-mesh-did-not-move-with-lipsync");
            Finding.Summary = TEXT("lip-sync frames were applied to the component but the face "
                                   "mesh's jaw curve never moved");
            Finding.Evidence = FString::Printf(
                TEXT("%d frame(s) applied with a peak blendshape weight of %.3f, yet "
                     "CTRL_expressions_jawOpen on the face mesh peaked at %.3f (< %.2f) across "
                     "%.0f ms of speech. The Anim Blueprint's face-sync node is not driving this "
                     "mesh from this chatbot."),
                Frames, FaceMaxWeight, MeshJawMax, kMinJawOpen, TalkingSeconds * 1000.0);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));
            Result.FailReason = TEXT("the face mesh did not move with the lip-sync frames");
            return Result;
        }
        if (NeutralAfterMs < 0.0)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("face-did-not-return-to-neutral");
            Finding.Summary = TEXT("the face moved with speech but never settled after it");
            Finding.Evidence = FString::Printf(
                TEXT("CTRL_expressions_jawOpen peaked at %.3f during speech and stayed at or "
                     "above %.2f for %.0f s after the audio finished; the anim node's fade "
                     "should have zeroed it within about 0.8 s."),
                MeshJawMax, kNeutralJawOpen, kNeutralTimeoutSeconds);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));
            Result.FailReason = TEXT("the face did not return to neutral after speech");
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
    TWeakObjectPtr<USkeletalMeshComponent> FaceMesh;

    float Elapsed = 0.0f;
    float SentAtSeconds = 0.0f;
    float QuietSinceSeconds = -1.0f;
    float TalkingSeconds = 0.0f;
    float StarvedSeconds = 0.0f;
    float StarvedMaxSeconds = 0.0f;
    double FaceMaxWeight = 0.0;
    double MeshJawMax = 0.0;
    double NeutralAfterMs = -1.0;
    int32 SpeechGaps = 0;
    bool bSent = false;
    bool bSpoke = false;
    bool bSawFrame = false;
    bool bSawCurve = false;
    bool bSetupFailed = false;
    FString FailReason;
};

CONVAI_REGISTER_SCENARIO(FConvaiConfiguredLipSyncScenario)
