// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiReferenceFeedMonitor.h"

#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiSubsystem.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTests.h"
#include "ConvaiUtils.h"

#include "AudioMixerBlueprintLibrary.h"
#include "AudioMixerDevice.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Sound/SoundSubmix.h"

namespace
{
    Audio::FMixerDevice* ResolveMixerDevice(UWorld* World)
    {
        FAudioDeviceHandle Handle = World ? World->GetAudioDevice() : FAudioDeviceHandle();
        if (!Handle.IsValid() && GEngine)
        {
            Handle = GEngine->GetMainAudioDevice();
        }
        FAudioDevice* Device = Handle.GetAudioDevice();
        return Device ? static_cast<Audio::FMixerDevice*>(Device) : nullptr;
    }

    UConvaiSubsystem* ResolveConvaiSubsystem(UWorld* World)
    {
        UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
        return GameInstance ? GameInstance->GetSubsystem<UConvaiSubsystem>() : nullptr;
    }
}

// Ground truth. Runs on the audio render thread, so everything it touches is
// under one lock and the readers only ever take snapshots.
class FConvaiReferenceFeedMonitor::FGroundTruthListener : public ISubmixBufferListener
{
public:
    virtual void OnNewSubmixBuffer(const USoundSubmix*, float* AudioData, int32 NumSamples,
                                   int32 InNumChannels, const int32 InSampleRate,
                                   double AudioClock) override
    {
        if (!AudioData || NumSamples <= 0)
        {
            return;
        }

        float LocalPeak = 0.0f;
        for (int32 i = 0; i < NumSamples; ++i)
        {
            LocalPeak = FMath::Max(LocalPeak, FMath::Abs(AudioData[i]));
        }

        const double Now = FPlatformTime::Seconds();

        FScopeLock ScopeLock(&StateLock);
        if (Buffers > 0)
        {
            // Audio clock: advances with rendered samples, so this is flat by
            // construction and only a continuity check.
            AudioClockGaps.Add((AudioClock - LastAudioClock) * 1000.0);
            // Wall clock: where F4's jitter actually shows up.
            WallGaps.Add((Now - LastWallSeconds) * 1000.0);
        }
        LastAudioClock = AudioClock;
        LastWallSeconds = Now;

        ++Buffers;
        Samples += NumSamples;
        Peak = FMath::Max(Peak, LocalPeak);
        NumChannels = InNumChannels;
        SampleRate = InSampleRate;
    }

    virtual const FString& GetListenerName() const override
    {
        static const FString Name = TEXT("ConvaiReferenceFeedMonitor");
        return Name;
    }

    void FillReport(FReport& Out) const
    {
        FScopeLock ScopeLock(&StateLock);
        Out.RenderedSamples = Samples;
        Out.RenderedBuffers = Buffers;
        Out.RenderedPeak = Peak;
        Out.RenderedSampleRate = SampleRate;
        Out.RenderedChannels = NumChannels;

        Summarise(AudioClockGaps, Out.AudioClockGapMeanMs, Out.AudioClockGapMaxMs,
                  Out.AudioClockGapStdDevMs);
        Summarise(WallGaps, Out.WallGapMeanMs, Out.WallGapMaxMs, Out.WallGapStdDevMs);
    }

    int32 BufferCount() const
    {
        FScopeLock ScopeLock(&StateLock);
        return Buffers;
    }

    float PeakSoFar() const
    {
        FScopeLock ScopeLock(&StateLock);
        return Peak;
    }

private:
    static void Summarise(const TArray<double>& Values, double& OutMean, double& OutMax,
                          double& OutStdDev)
    {
        if (Values.Num() == 0)
        {
            return;
        }
        double Sum = 0.0;
        double Max = 0.0;
        for (const double Value : Values)
        {
            Sum += Value;
            Max = FMath::Max(Max, Value);
        }
        const double Mean = Sum / Values.Num();

        double SumSquaredError = 0.0;
        for (const double Value : Values)
        {
            SumSquaredError += (Value - Mean) * (Value - Mean);
        }

        OutMean = Mean;
        OutMax = Max;
        OutStdDev = FMath::Sqrt(SumSquaredError / Values.Num());
    }

    mutable FCriticalSection StateLock;
    int32 Buffers = 0;
    int64 Samples = 0;
    float Peak = 0.0f;
    int32 NumChannels = 0;
    int32 SampleRate = 0;
    double LastAudioClock = 0.0;
    double LastWallSeconds = 0.0;
    TArray<double> AudioClockGaps;
    TArray<double> WallGaps;
};

void FConvaiReferenceFeedMonitor::AddFeedMetrics(
    TMap<FString, double>& OutMetrics, const FConvaiReferenceAudioThread::FStats& Feed,
    const TCHAR* Prefix)
{
    const auto Add = [&OutMetrics, Prefix](const TCHAR* Key, double Value)
    { OutMetrics.Add(FString(Prefix) + Key, Value); };

    Add(TEXT("chunks_sent"), static_cast<double>(Feed.ChunksSent));
    Add(TEXT("client_sends"), static_cast<double>(Feed.ClientSends));
    Add(TEXT("captured_samples"), static_cast<double>(Feed.CapturedSamples));
    Add(TEXT("capture_ratio"), Feed.CaptureRatio);
    Add(TEXT("capture_window_s"), Feed.CaptureWindowSeconds);
    Add(TEXT("dispatch_count"), static_cast<double>(Feed.DispatchCount));
    Add(TEXT("dispatch_gap_mean_ms"), Feed.DispatchGapMeanMs);
    Add(TEXT("dispatch_gap_max_ms"), Feed.DispatchGapMaxMs);
    Add(TEXT("dispatch_gap_stddev_ms"), Feed.DispatchGapStdDevMs);
    Add(TEXT("recorder_off_mean_ms"), Feed.RecorderOffMeanMs);
    Add(TEXT("recorder_off_max_ms"), Feed.RecorderOffMaxMs);
    Add(TEXT("recorder_off_stddev_ms"), Feed.RecorderOffStdDevMs);

    // Which tap produced all of the above. Reported as a metric rather than
    // trusted from the command line, so a run that silently fell back to the
    // shipping path cannot be read as a measurement of the other one.
    Add(TEXT("tap_is_listener"), Feed.bSubmixListenerTap ? 1.0 : 0.0);
}

void FConvaiReferenceFeedMonitor::Start(UWorld* World, FConvaiTestEventRecorder* InRecorder,
                                        double ExpectedDurationSeconds)
{
    WorldPtr = World;
    Recorder = InRecorder;

    Audio::FMixerDevice* MixerDevice = ResolveMixerDevice(World);
    if (!MixerDevice || !World)
    {
        if (Recorder)
        {
            Recorder->Record(TEXT("monitor_setup_failed"),
                             MixerDevice ? TEXT("no world") : TEXT("no mixer device"));
        }
        return;
    }

    GroundTruth = MakeShared<FGroundTruthListener, ESPMode::ThreadSafe>();
    MixerDevice->RegisterSubmixBufferListener(GroundTruth.ToSharedRef(),
                                              MixerDevice->GetMainSubmixObject());

    // Reserve past the window so a slow stop cannot truncate the capture. The
    // reserve itself is free after the first call — see F11, which was refuted.
    UAudioMixerBlueprintLibrary::StartRecordingOutput(
        World, static_cast<float>(ExpectedDurationSeconds + 1.0), nullptr);

    StartSeconds = FPlatformTime::Seconds();
    SinceSample = 0.0f;
    BuffersAtLastSample = 0;
    ChunksAtLastSample = 0;
    LastFeed = FConvaiReferenceAudioThread::FStats();
    bStarted = true;

    if (Recorder)
    {
        Recorder->Record(TEXT("monitor_started"),
                         FString::Printf(TEXT("rate=%.0f channels=%d"), MixerDevice->GetSampleRate(),
                                         MixerDevice->GetNumDeviceChannels()));
    }
}

void FConvaiReferenceFeedMonitor::Tick(float DeltaSeconds)
{
    if (!bStarted)
    {
        return;
    }

    SinceSample += DeltaSeconds;
    if (SinceSample < 1.0f)
    {
        return;
    }
    SinceSample = 0.0f;

    FSample Sample;
    Sample.TimeSeconds = FPlatformTime::Seconds() - StartSeconds;

    // F1's invariant, polled from public state. LiveConnections is the count of
    // Connections that exist; GetReferenceAudioStatus is the count actually
    // being fed. They should agree whenever AEC is on.
    if (UConvaiSubsystem* Subsystem = ResolveConvaiSubsystem(WorldPtr.Get()))
    {
        // This branch has one Connection per process, so the subsystem's own
        // connection state is the whole count. Connected, not merely opened:
        // the reference client is attached at the end of the handshake, so
        // counting a Connecting session would report a deficit for the length
        // of every handshake and bury the real case.
        if (Subsystem->GetServerConnectionState() == EC_ConnectionState::Connected)
        {
            ++Sample.LiveConnections;
        }
        const UConvaiSubsystem::FReferenceAudioStatus Status = Subsystem->GetReferenceAudioStatus();
        Sample.ReferenceClients = Status.ClientCount;
        Sample.bCapturing = Status.bCapturing;

        Sample.ReferenceChunksInWindow = Status.Feed.ChunksSent - ChunksAtLastSample;
        ChunksAtLastSample = Status.Feed.ChunksSent;
        if (Status.bCapturing)
        {
            LastFeed = Status.Feed;
        }

        // Sticky. The plugin's reference capture and this monitor both drive
        // the one process-global master-submix recorder, each stopping and
        // restarting it, so once they have overlapped at all the capture
        // figures are meaningless for the whole window.
        //
        // Only the recorder tap contends. On the submix listener tap the plugin
        // holds no recorder, so this monitor's own capture is valid and saying
        // otherwise would discard the one measurement that shows the difference.
        bRecorderContended |= Status.bCapturing && !Status.Feed.bSubmixListenerTap;
    }

    const int32 Buffers = GroundTruth.IsValid() ? GroundTruth->BufferCount() : 0;
    Sample.RenderedBuffersInWindow = Buffers - BuffersAtLastSample;
    BuffersAtLastSample = Buffers;
    Sample.RenderedPeakInWindow = GroundTruth.IsValid() ? GroundTruth->PeakSoFar() : 0.0f;

    if (Recorder)
    {
        Recorder->Record(TEXT("reference_sample"),
                         FString::Printf(TEXT("t=%.1f connections=%d clients=%d capturing=%d "
                                              "buffers=%d chunks=%lld"),
                                         Sample.TimeSeconds, Sample.LiveConnections,
                                         Sample.ReferenceClients, Sample.bCapturing ? 1 : 0,
                                         Sample.RenderedBuffersInWindow,
                                         Sample.ReferenceChunksInWindow));
    }

    Samples.Add(Sample);
}

FConvaiReferenceFeedMonitor::FReport FConvaiReferenceFeedMonitor::Finish()
{
    FReport Report;
    if (!bStarted)
    {
        return Report;
    }
    bStarted = false;

    Audio::FMixerDevice* MixerDevice = ResolveMixerDevice(WorldPtr.Get());
    if (MixerDevice)
    {
        // StopRecording writes its out-params and returns the buffer by
        // reference; argument evaluation order is unspecified, so the call has
        // to complete before those values are read.
        float NumChannels = 0.0f;
        float SampleRate = 0.0f;
        const Audio::FAlignedFloatBuffer Captured =
            MixerDevice->StopRecording(nullptr, NumChannels, SampleRate);

        float CapturedPeak = 0.0f;
        for (const float Value : Captured)
        {
            CapturedPeak = FMath::Max(CapturedPeak, FMath::Abs(Value));
        }
        Report.CapturedSamples = Captured.Num();
        Report.CapturedPeak = CapturedPeak;

        if (GroundTruth.IsValid())
        {
            MixerDevice->UnregisterSubmixBufferListener(GroundTruth.ToSharedRef(),
                                                        MixerDevice->GetMainSubmixObject());
        }
    }

    if (GroundTruth.IsValid())
    {
        GroundTruth->FillReport(Report);
    }

    Report.CaptureRatio = Report.RenderedSamples > 0
                              ? static_cast<double>(Report.CapturedSamples) / Report.RenderedSamples
                              : 0.0;

    Report.bRecorderContended = bRecorderContended;
    Report.bAecEnabled = UConvaiUtils::IsAECEnabled();

    // Read once more rather than taking the last 1 Hz sample: the window's
    // final second is as much of the capture as any other.
    if (UConvaiSubsystem* Subsystem = ResolveConvaiSubsystem(WorldPtr.Get()))
    {
        const UConvaiSubsystem::FReferenceAudioStatus Status = Subsystem->GetReferenceAudioStatus();
        if (Status.bCapturing)
        {
            LastFeed = Status.Feed;
        }
    }
    Report.Feed = LastFeed;

    Report.Samples = Samples;
    Report.SamplesTaken = Samples.Num();

    // With AEC off, no Connection has a reference client and that is correct
    // behaviour, not a deficit. Computing it anyway would hand the fix agent a
    // finding for a setting.
    if (Report.bAecEnabled)
    {
        for (const FSample& Sample : Samples)
        {
            const int32 Deficit = Sample.LiveConnections - Sample.ReferenceClients;
            if (Deficit > 0 && Report.FirstDeficitAtSeconds < 0.0)
            {
                Report.FirstDeficitAtSeconds = Sample.TimeSeconds;
            }
            if (Deficit > 0)
            {
                Report.LastDeficitAtSeconds = Sample.TimeSeconds;
            }
            Report.MaxConnectionDeficit = FMath::Max(Report.MaxConnectionDeficit, Deficit);
        }
    }

    Samples.Reset();
    GroundTruth.Reset();
    return Report;
}
