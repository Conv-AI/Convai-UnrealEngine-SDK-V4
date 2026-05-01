// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
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

private:
    // Thread management
    FRunnableThread* Thread;
    FThreadSafeBool bStopRequested;
    FThreadSafeBool bIsCapturing;

    // ConvaiClient reference
    convai::ConvaiClient* ConvaiClient;

    // World reference for accessing audio mixer
    TWeakObjectPtr<UWorld> WorldPtr;

    // Audio processing variables
    TArray<int16> AudioProcessingBuffer;
    int32 ProcessingChunkSize;
    int32 TargetSampleRate;

    // Audio capture variables
    bool bIsRecording;
    double LastCaptureTime;
    double CaptureInterval; // Time between captures in seconds (10ms = 0.01s)

    // Helper functions
    Audio::FMixerDevice* GetAudioMixerDevice() const;
    void ProcessCapturedAudio();
    void SendAudioChunkToConvaiClient(const int16* AudioData, int32 NumSamples);
    void StartRecordingRefrence(TWeakObjectPtr<UWorld> WorldPtr);

    // Thread name
    static const TCHAR* ThreadName;
};
