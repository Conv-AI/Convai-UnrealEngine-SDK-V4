// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISubmixBufferListener.h"

#include "ConvaiReferenceAudioThread.h"

class FConvaiTestEventRecorder;
class UWorld;
namespace Audio { class FMixerDevice; }

/**
 * Issue 03. Makes Reference Audio observable, then asserts it is correct.
 *
 * Attaches to any scenario. Two independent taps by design: an
 * ISubmixBufferListener on the main submix as ground truth, against the
 * StartRecordingOutput path the plugin's own capture uses. Sharing a tap would
 * make a dead capture path and a silent game look identical, which is F2's
 * failure mode exactly.
 *
 * Per-second samples exist because the interesting failures are intermittent:
 * F15 shows the capture returning almost nothing in some runs and not others,
 * and a whole-run average hides that.
 */
class CONVAITESTS_API FConvaiReferenceFeedMonitor
{
public:
    struct FSample
    {
        double TimeSeconds = 0.0;
        // Connections in state Connected. A Connection still handshaking has no
        // reference client yet and that is correct, so counting registered
        // sessions would report a deficit for every one of them.
        int32 LiveConnections = 0;
        int32 ReferenceClients = 0;
        bool bCapturing = false;
        int32 RenderedBuffersInWindow = 0;
        float RenderedPeakInWindow = 0.0f;
        // Chunks the plugin's own capture fanned out since the last sample. A
        // feed that dies partway through a run reads as a healthy total.
        int64 ReferenceChunksInWindow = 0;
    };

    struct FReport
    {
        // Ground truth: what the mixer rendered.
        int64 RenderedSamples = 0;
        int32 RenderedBuffers = 0;
        float RenderedPeak = 0.0f;
        int32 RenderedSampleRate = 0;
        int32 RenderedChannels = 0;

        // Audio-clock spacing between rendered buffers. This is a continuity
        // check, not a jitter measure: the audio clock advances with rendered
        // samples, so an even spacing only confirms no rendered audio was
        // skipped. It is flat by construction and a non-flat value means
        // something dropped buffers outright.
        double AudioClockGapMaxMs = 0.0;
        double AudioClockGapMeanMs = 0.0;
        double AudioClockGapStdDevMs = 0.0;

        // Wall-clock spacing between the same buffers -- the mixer's own render
        // cadence, and nothing to do with the plugin.
        //
        // This was documented as "where F4's jitter lives". It is not, and the
        // claim was wrong: these gaps are between submix buffer callbacks, so
        // they measure the audio device's fixed interval. Ten runs put the
        // standard deviation between 4.922 and 4.929 ms, identical to three
        // decimals whether the run misbehaved or not. F4 is the 2 ms poll and
        // asynchronous dispatch inside FConvaiReferenceAudioThread::Run, which
        // this listener never observes. That is what Feed below measures.
        double WallGapMaxMs = 0.0;
        double WallGapMeanMs = 0.0;
        double WallGapStdDevMs = 0.0;

        // The plugin's own capture, reported by the thread that runs it. This
        // is the only tap that works while a Connection is live: the feed holds
        // the one global recorder, so a second recorder-based measurement is
        // drained by it and reports the contention rather than the feed.
        FConvaiReferenceAudioThread::FStats Feed;

        // What a master-submix recorder returned over the same window.
        int32 CapturedSamples = 0;
        float CapturedPeak = 0.0f;
        double CaptureRatio = 0.0;

        // True when the plugin's own reference capture was running during the
        // window. Both it and this monitor drive the one process-global
        // master-submix recorder, so they steal each other's buffer and the
        // capture figures above mean nothing. Reported rather than hidden: a
        // second consumer of that recorder being unusable while a Connection is
        // live is itself the finding.
        bool bRecorderContended = false;

        // F1: sampled once a second, the worst disagreement seen between live
        // Connections and Connections actually being fed. Only meaningful when
        // AEC is on — with it off, zero clients is correct behaviour.
        bool bAecEnabled = false;
        int32 MaxConnectionDeficit = 0;
        int32 SamplesTaken = 0;

        // When the deficit was seen. At 1 Hz these cannot separate a
        // one-frame attach race at startup from a Connection that never gets
        // fed, so they are reported and the distinction is left explicit
        // rather than asserted away. Negative means never.
        double FirstDeficitAtSeconds = -1.0;
        double LastDeficitAtSeconds = -1.0;

        TArray<FSample> Samples;
    };

    // Registers the ground-truth listener and starts the plugin-path capture.
    void Start(UWorld* World, FConvaiTestEventRecorder* Recorder, double ExpectedDurationSeconds);

    // Call every frame. Takes a sample once a second.
    void Tick(float DeltaSeconds);

    // Stops the capture, unregisters, and returns everything measured.
    FReport Finish();

    bool IsStarted() const { return bStarted; }

    // The thread's own numbers, flattened for the report. Shared so the zero
    // control and the live runs emit the same keys — a control that reports
    // different fields is not a control.
    static void AddFeedMetrics(TMap<FString, double>& OutMetrics,
                               const FConvaiReferenceAudioThread::FStats& Feed,
                               const TCHAR* Prefix);

private:
    class FGroundTruthListener;

    TSharedPtr<FGroundTruthListener, ESPMode::ThreadSafe> GroundTruth;
    TWeakObjectPtr<UWorld> WorldPtr;
    FConvaiTestEventRecorder* Recorder = nullptr;

    bool bStarted = false;
    bool bRecorderContended = false;
    double StartSeconds = 0.0;
    float SinceSample = 0.0f;
    int32 BuffersAtLastSample = 0;
    int64 ChunksAtLastSample = 0;
    // The capture stops with the last Connection, so the final poll can find no
    // thread at all. Kept from the last sample that saw one.
    FConvaiReferenceAudioThread::FStats LastFeed;
    TArray<FSample> Samples;
};
