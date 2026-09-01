// Copyright 2022 Convai Inc. All Rights Reserved.

// Issue 03 — make Reference Audio observable, then assert it is correct.
//
// No backend, no microphone, no echo cancellation. Every confirmed suspect in
// FINDINGS.md that does not need a Connection lives here.
//
// The measurement is a comparison, not an absolute: an ISubmixBufferListener on
// the main submix is the ground truth, and StartRecordingOutput / StopRecording
// is what FConvaiReferenceAudioThread uses. Sharing one tap would make a dead
// capture path and a silent game look identical, which is F2's failure mode.
//
// Two variants. The idle one establishes the baseline; the loaded one exists
// because F4 predicts the feed degrades only under frame-time pressure, and a
// bound that is only ever checked at idle is not a bound.

#include "ConvaiReferenceFeedMonitor.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTests.h"

#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Sound/SoundBase.h"

namespace
{
    // Loose on purpose. The recorder and the listener start and stop at
    // slightly different instants, so a few percent is structural. F15's
    // failure mode is not a few percent — it is near-total.
    constexpr double AcceptableCaptureLoss = 0.10;

    // The master submix auto-disables when nothing is playing, and its disabled
    // path returns before the recorder append (AudioMixerSubmix.cpp:1410 vs
    // :1686). A quiet scenario would measure the auto-disable, not the capture.
    const TCHAR* ProbeSoundPath = TEXT("/Engine/EngineSounds/WhiteNoise");
}

// Shared by both variants. The load variant differs only in what it does to the
// frame while the same measurement runs underneath.
class FConvaiReferenceFeedScenarioBase : public FConvaiTestScenario
{
public:
    virtual double DeadlineSeconds() const override { return 30.0; }
    virtual bool RequiresLiveConnection() const override { return false; }

    virtual void Start(UWorld* InWorld, FConvaiTestEventRecorder& InRecorder) override
    {
        World = InWorld;
        Recorder = &InRecorder;

        ProbeSound = LoadObject<USoundBase>(nullptr, ProbeSoundPath);
        if (!ProbeSound || !World)
        {
            Recorder->Record(TEXT("setup_failed"),
                             ProbeSound ? TEXT("no world") : TEXT("probe sound missing"));
            bSetupFailed = true;
            return;
        }

        RetriggerInterval = FMath::Max(0.05f, ProbeSound->GetDuration() * 0.8f);
        UGameplayStatics::SpawnSound2D(World, ProbeSound);

        Monitor.Start(World, Recorder, CaptureSeconds);
        if (!Monitor.IsStarted())
        {
            bSetupFailed = true;
        }
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }

        Elapsed += DeltaSeconds;
        SinceRetrigger += DeltaSeconds;
        if (SinceRetrigger >= RetriggerInterval && IsValid(World) && IsValid(ProbeSound))
        {
            SinceRetrigger = 0.0f;
            UGameplayStatics::SpawnSound2D(World, ProbeSound);
        }

        ApplyLoad();
        Monitor.Tick(DeltaSeconds);
        return Elapsed >= CaptureSeconds;
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

        const FConvaiReferenceFeedMonitor::FReport Report = Monitor.Finish();

        Result.Metrics.Add(TEXT("rendered_samples"), static_cast<double>(Report.RenderedSamples));
        Result.Metrics.Add(TEXT("rendered_buffers"), Report.RenderedBuffers);
        Result.Metrics.Add(TEXT("rendered_peak"), Report.RenderedPeak);
        Result.Metrics.Add(TEXT("rendered_sample_rate"), Report.RenderedSampleRate);
        Result.Metrics.Add(TEXT("rendered_channels"), Report.RenderedChannels);
        Result.Metrics.Add(TEXT("audio_clock_gap_max_ms"), Report.AudioClockGapMaxMs);
        Result.Metrics.Add(TEXT("audio_clock_gap_mean_ms"), Report.AudioClockGapMeanMs);
        Result.Metrics.Add(TEXT("audio_clock_gap_stddev_ms"), Report.AudioClockGapStdDevMs);
        Result.Metrics.Add(TEXT("wall_gap_max_ms"), Report.WallGapMaxMs);
        Result.Metrics.Add(TEXT("wall_gap_mean_ms"), Report.WallGapMeanMs);
        Result.Metrics.Add(TEXT("wall_gap_stddev_ms"), Report.WallGapStdDevMs);
        Result.Metrics.Add(TEXT("captured_samples"), Report.CapturedSamples);
        Result.Metrics.Add(TEXT("captured_peak"), Report.CapturedPeak);
        Result.Metrics.Add(TEXT("capture_ratio"), Report.CaptureRatio);
        Result.Metrics.Add(TEXT("capture_loss_percent"), (1.0 - Report.CaptureRatio) * 100.0);
        Result.Metrics.Add(TEXT("connection_deficit_max"), Report.MaxConnectionDeficit);
        Result.Metrics.Add(TEXT("samples_taken"), Report.SamplesTaken);
        Result.Metrics.Add(TEXT("aec_enabled"), Report.bAecEnabled ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("recorder_contended"), Report.bRecorderContended ? 1.0 : 0.0);
        Result.Metrics.Add(TEXT("deficit_first_at_s"), Report.FirstDeficitAtSeconds);
        Result.Metrics.Add(TEXT("deficit_last_at_s"), Report.LastDeficitAtSeconds);

        // Issue 13's tap, and on a map with no Connection this is its zero
        // control: no Connection means no capture thread, so every one of these
        // must read zero. If they do not, the tap is reporting something other
        // than the plugin's Reference Audio feed and its numbers on the live
        // scenarios mean nothing.
        FConvaiReferenceFeedMonitor::AddFeedMetrics(Result.Metrics, Report.Feed, TEXT("feed_"));

        // Fixture self-check first, always. Zero rendered and zero captured is
        // a ratio of nothing, and "the capture lost nothing" would then be both
        // true and meaningless — the shape that lets a silent machine report a
        // clean sweep forever.
        if (Report.RenderedBuffers == 0 || Report.RenderedPeak <= 1e-5f)
        {
            Result.bPassed = false;
            Result.FailReason = FString::Printf(
                TEXT("the mixer rendered nothing audible (buffers=%d peak=%.6f, volume "
                     "multiplier=%.2f) — the fixture is broken, not the capture"),
                Report.RenderedBuffers, Report.RenderedPeak, FApp::GetVolumeMultiplier());
            return Result;
        }

        // F1. Sampled once a second rather than checked once, because a
        // Connection that loses its feed part-way through is the case that
        // produced the symptom reports. The monitor only computes a deficit
        // when AEC is on — with it off, zero clients is correct.
        if (Report.bAecEnabled && Report.MaxConnectionDeficit > 0)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("reference-clients-below-connections");
            Finding.Summary = TEXT("a live Connection was not receiving Reference Audio");
            Finding.Evidence = FString::Printf(
                TEXT("peak deficit %d across %d per-second samples with AEC enabled; first seen "
                     "at t=%.1fs, last at t=%.1fs; see reference_sample events in the trace"),
                Report.MaxConnectionDeficit, Report.SamplesTaken, Report.FirstDeficitAtSeconds,
                Report.LastDeficitAtSeconds);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            // 1 Hz cannot tell a one-frame attach race at startup from a
            // Connection that is never fed. Both are worth fixing and they have
            // different fixes, so the ambiguity is stated rather than resolved
            // by assertion.
            if (Report.FirstDeficitAtSeconds == Report.LastDeficitAtSeconds)
            {
                Result.Hypotheses.Add(
                    TEXT("the deficit appeared in a single sample, which is consistent with a "
                         "startup race between a Connection registering as live and "
                         "AttachReferenceAudioClient running. Sub-second sampling would "
                         "distinguish that from a Connection that never gets fed."));
            }
        }

        // F17. The plugin's reference capture and this monitor both drive the
        // one process-global master-submix recorder, so while a Connection is
        // live neither can measure anything — they stop and restart each
        // other's recording. That is reported as its own condition rather than
        // as capture loss, because calling it loss would blame the plugin's
        // feed for an instrument that cannot read while the feed is running.
        //
        // It is also a real defect in its own right: any customer calling
        // UAudioMixerBlueprintLibrary::StartRecordingOutput during a
        // conversation hits the same collision, and so does the plugin's own
        // microphone path (F6).
        if (Report.bRecorderContended)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("master-recorder-contention");
            Finding.Summary =
                TEXT("the plugin's Reference Audio capture and a second master-submix recorder "
                     "cannot coexist; each stops the other's recording");
            Finding.Evidence = FString::Printf(
                TEXT("reference capture was active during the window; an independent "
                     "StartRecordingOutput/StopRecording over the same %.0f s returned %d samples "
                     "against %lld rendered"),
                CaptureSeconds, Report.CapturedSamples, Report.RenderedSamples);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.Hypotheses.Add(
                TEXT("capture_ratio is not meaningful in this run: the recorder was contended. "
                     "Run this scenario with no live Connection to measure the feed itself."));

            Result.bPassed = false;
            Result.FailReason = TEXT("master-submix recorder contended by the plugin's own capture");
            return Result;
        }

        // F15 / F3.
        if (Report.CaptureRatio < 1.0 - AcceptableCaptureLoss)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("reference-capture-loss");
            Finding.Summary =
                TEXT("StopRecording on the master submix returned far less audio than the mixer "
                     "rendered; the Reference Audio feed is losing most of the far-end signal");
            Finding.Evidence = FString::Printf(
                TEXT("submix listener saw %lld samples in %d buffers at peak %.4f; "
                     "StopRecording(nullptr) returned %d samples at peak %.4f; ratio %.4f"),
                Report.RenderedSamples, Report.RenderedBuffers, Report.RenderedPeak,
                Report.CapturedSamples, Report.CapturedPeak, Report.CaptureRatio);
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.bPassed = false;
            Result.FailReason = FString::Printf(TEXT("capture lost %.1f%% of rendered audio"),
                                                (1.0 - Report.CaptureRatio) * 100.0);
            return Result;
        }

        if (Report.CapturedPeak <= 1e-5f)
        {
            Result.bPassed = false;
            Result.FailReason =
                TEXT("capture returned the right number of samples and all of them are silent");
            return Result;
        }

        // F4's bound is reported rather than asserted until the idle and loaded
        // baselines exist side by side across enough runs to say what "within
        // bounds" means. Guessing a threshold now would either never fire or
        // fire on every run, and both teach nothing.
        Result.Hypotheses.Add(FString::Printf(
            TEXT("F4: wall-clock buffer spacing mean %.2f ms, stddev %.2f ms, max %.2f ms "
                 "(audio-clock spacing %.2f ms, stddev %.2f — flat by construction, so it only "
                 "confirms no rendered audio was skipped). No bound asserted yet: compare the "
                 "idle and loaded variants across runs before setting one."),
            Report.WallGapMeanMs, Report.WallGapStdDevMs, Report.WallGapMaxMs,
            Report.AudioClockGapMeanMs, Report.AudioClockGapStdDevMs));

        Result.bPassed = Result.Findings.Num() == 0;
        if (!Result.bPassed)
        {
            Result.FailReason = TEXT("invariant violated; see findings");
        }
        return Result;
    }

protected:
    virtual void ApplyLoad() {}

    static constexpr float CaptureSeconds = 5.0f;

    UWorld* World = nullptr;
    FConvaiTestEventRecorder* Recorder = nullptr;
    USoundBase* ProbeSound = nullptr;
    FConvaiReferenceFeedMonitor Monitor;

    bool bSetupFailed = false;
    float Elapsed = 0.0f;
    float SinceRetrigger = 0.0f;
    float RetriggerInterval = 0.3f;
};

class FConvaiReferenceFeedScenario : public FConvaiReferenceFeedScenarioBase
{
public:
    static const TCHAR* StaticName() { return TEXT("reference_feed_capture"); }
    virtual const TCHAR* Name() const override { return StaticName(); }
};

CONVAI_REGISTER_SCENARIO(FConvaiReferenceFeedScenario)

// F4 predicts the feed degrades under frame-time pressure, because Run() polls
// a wall clock and dispatches asynchronously to the audio thread, so commands
// queue and coalesce when the game thread is busy. Burning game-thread time is
// a crude stand-in for a real workload, but it is the variable F4 names and it
// is reproducible, which a real workload would not be.
class FConvaiReferenceFeedUnderLoadScenario : public FConvaiReferenceFeedScenarioBase
{
public:
    static const TCHAR* StaticName() { return TEXT("reference_feed_capture_under_load"); }
    virtual const TCHAR* Name() const override { return StaticName(); }

protected:
    virtual void ApplyLoad() override
    {
        // ~25 ms of game thread per frame, well past a 60 Hz budget. The result
        // is deliberately not optimised away: the sum is fed back into the
        // seed so the compiler cannot drop the loop.
        const double Deadline = FPlatformTime::Seconds() + 0.025;
        while (FPlatformTime::Seconds() < Deadline)
        {
            Spin = Spin * 1664525u + 1013904223u;
        }
    }

private:
    volatile uint32 Spin = 1;
};

CONVAI_REGISTER_SCENARIO(FConvaiReferenceFeedUnderLoadScenario)
