// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/CriticalSection.h"
#include "AudioMixerBlueprintLibrary.h"
#include "AudioMixerDevice.h"
#include "ThirdParty/ConvaiWebRTC/include/convai/convai_client.h"

/**
 * FRunnableThread class for capturing reference audio (system/speaker audio) 
 * and sending it to convai_client.dll via SendReferenceAudio method
 */
class CONVAI_API FConvaiReferenceAudioThread : public FRunnable, public TSharedFromThis<FConvaiReferenceAudioThread>
{
public:
    explicit FConvaiReferenceAudioThread(convai::ConvaiClient* InConvaiClient, UWorld* InWorld);
    virtual ~FConvaiReferenceAudioThread();

    // FRunnable interface
    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;
    virtual void Exit() override;

    // Control functions
    void StartCapture();
    void StopCapture();
    bool IsCapturing() const { return bIsCapturing; }

    /**
     * How much Reference Audio this loop has produced, and at what cadence.
     *
     * Nothing outside the loop can measure either. It holds the one
     * process-global master-submix recorder and drains any second user of it,
     * so a Connection whose far-end signal is arriving late, in bursts, or
     * barely at all reads as healthy in every log — which is how
     * echo-cancellation faults have been diagnosed by ear.
     */
    struct FStats
    {
        // True when these numbers came from a submix listener rather than the
        // output recorder. Always false here: this branch has only the
        // recorder tap.
        bool bSubmixListenerTap = false;

        int64 ChunksSent = 0;
        // Chunks times the clients each was delivered to.
        int64 ClientSends = 0;

        int64 CapturedSamples = 0;
        double CaptureWindowSeconds = 0.0;
        int32 CapturedSampleRate = 0;
        int32 CapturedChannels = 0;

        // Captured samples over what the mixer rendered in the same window.
        // Below 1 is audio the recorder dropped while it was stopped, which is
        // every buffer rendered across the conversion, resample and fan-out.
        double CaptureRatio = 0.0;

        // Wall gap between consecutive ProcessCapturedAudio executions: a 2 ms
        // poll against a 10 ms interval, then an asynchronous dispatch that the
        // audio thread runs whenever it drains its command queue.
        int64 DispatchCount = 0;
        double DispatchGapLastMs = 0.0;
        double DispatchGapMeanMs = 0.0;
        double DispatchGapMaxMs = 0.0;
        double DispatchGapStdDevMs = 0.0;

        // How long the recorder stays stopped each cycle — StopRecording
        // returning to StartRecordingRefrence restarting it. Everything the
        // mixer renders inside this window is dropped, not buffered.
        int64 RecorderOffCount = 0;
        double RecorderOffLastMs = 0.0;
        double RecorderOffMeanMs = 0.0;
        double RecorderOffMaxMs = 0.0;
        double RecorderOffStdDevMs = 0.0;
    };
    FStats GetStats() const;

private:
    // Thread management
    FRunnableThread* Thread;
    FThreadSafeBool bStopRequested;
    FThreadSafeBool bIsCapturing;

    // ConvaiClient reference
    convai::ConvaiClient* ConvaiClient;

    // World reference for accessing audio mixer
    TWeakObjectPtr<UWorld> WorldPtr;

    // Guards all capture state that is touched from more than one thread:
    // AudioProcessingBuffer, bIsRecording, and the mixer device start/stop calls.
    // StartCapture/StopCapture run on the game thread (and StopCapture also on the
    // runnable thread via Exit()), while ProcessCapturedAudio/StartRecordingRefrence
    // run on the audio thread.
    FCriticalSection CaptureCS;

    // Audio processing variables
    TArray<int16> AudioProcessingBuffer;
    int32 ProcessingChunkSize;
    int32 TargetSampleRate;

    // Audio capture variables
    FThreadSafeBool bIsRecording;
    double LastCaptureTime;
    double CaptureInterval; // Time between captures in seconds (10ms = 0.01s)

    // Streaming, so recording a sample on the audio thread never allocates.
    struct FGapAccumulator
    {
        int64 Count = 0;
        double Sum = 0.0;
        double SumSquares = 0.0;
        double Max = 0.0;
        double Last = 0.0;

        void Add(double ValueMs);
        void Read(int64& OutCount, double& OutLast, double& OutMean, double& OutMax,
                  double& OutStdDev) const;
    };

    // Written from the audio thread, read from wherever asks.
    mutable FCriticalSection StatsMutex;
    FGapAccumulator DispatchGaps;
    FGapAccumulator RecorderOffGaps;
    int64 ChunksSent;
    int64 ClientSends;
    int64 CapturedSamples;
    double LastDispatchSeconds;
    double FirstStopSeconds;
    double LastStopSeconds;
    int32 LastCapturedSampleRate;
    int32 LastCapturedChannels;

    // Audio thread only: ProcessCapturedAudio sets it, StartRecordingRefrence
    // closes it in the same queued command.
    double RecorderStoppedAtSeconds;

    // Helper functions
    Audio::FMixerDevice* GetAudioMixerDevice() const;
    void ProcessCapturedAudio();
    void RecordTapDispatch();
    void SendAudioChunkToConvaiClient(const int16* AudioData, int32 NumSamples);
    void StartRecordingRefrence(TWeakObjectPtr<UWorld> WorldPtr);

    // Thread name
    static const TCHAR* ThreadName;
};
