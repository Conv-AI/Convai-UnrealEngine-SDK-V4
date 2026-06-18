// Fill out your copyright notice in the Description page of Project Settings.

#include "ConvaiReferenceAudioThread.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "ConvaiDefinitions.h"
#include "ConvaiUtils.h"
#include "HAL/PlatformProcess.h"
#include "Utility/Log/ConvaiLogger.h"

DEFINE_LOG_CATEGORY_STATIC(ConvaiReferenceAudioThread, Log, All);

const TCHAR* FConvaiReferenceAudioThread::ThreadName = TEXT("ConvaiReferenceAudioThread");

FConvaiReferenceAudioThread::FConvaiReferenceAudioThread(convai::ConvaiClient* InConvaiClient, UWorld* InWorld)
    : Thread(nullptr)
    , bStopRequested(false)
    , bIsCapturing(false)
    , ConvaiClient(InConvaiClient)
    , WorldPtr(InWorld)
    , ProcessingChunkSize(0)
    , TargetSampleRate(ConvaiConstants::VoiceCaptureSampleRate)
    , bIsRecording(false)
    , LastCaptureTime(0.0)
    , CaptureInterval(0.01) // 10ms
{
    // Calculate processing chunk size for 10ms frames
    ProcessingChunkSize = TargetSampleRate / 100; 

    // Initialize audio buffer
    AudioProcessingBuffer.Empty();
    AudioProcessingBuffer.Reserve(ProcessingChunkSize * 10); // Reserve space for multiple chunks

    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("ConvaiReferenceAudioThread created with chunk size: %d"), ProcessingChunkSize);
}

FConvaiReferenceAudioThread::~FConvaiReferenceAudioThread()
{
    StopCapture();

    if (Thread)
    {
        // Signal the thread to stop
        Stop();

        // Wait for the thread to complete
        // WaitForCompletion() will block until the thread exits
        Thread->WaitForCompletion();

        delete Thread;
        Thread = nullptr;
    }

    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("ConvaiReferenceAudioThread destroyed"));
}

bool FConvaiReferenceAudioThread::Init()
{
    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("ConvaiReferenceAudioThread initialized"));
    return true;
}

uint32 FConvaiReferenceAudioThread::Run()
{
    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("ConvaiReferenceAudioThread started running"));

    LastCaptureTime = FPlatformTime::Seconds();

    while (!bStopRequested)
    {
        if (bIsCapturing)
        {
            double CurrentTime = FPlatformTime::Seconds();

            // Check if it's time to capture audio (every 10ms)
            if (CurrentTime - LastCaptureTime >= CaptureInterval)
            {
                // Only queue audio thread command if the world is still valid
                if (WorldPtr.IsValid())
                {
                    TWeakPtr<FConvaiReferenceAudioThread> WeakSelf = AsShared();
                    FAudioThread::RunCommandOnAudioThread([WeakSelf, WeakWorld = WorldPtr]()
                    {
                            if (auto SharedThis = WeakSelf.Pin())
                            {
                                if (WeakWorld.IsValid())
                                {
                                    SharedThis->ProcessCapturedAudio();
                                    SharedThis->StartRecordingRefrence(WeakWorld);
                                }
                            }
                    });
                }
                LastCaptureTime = CurrentTime;
            }
        }

        // Sleep for a short time to avoid consuming too much CPU
        FPlatformProcess::Sleep(0.002f); // 2ms sleep
    }

    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("ConvaiReferenceAudioThread stopped running"));
    return 0;
}

void FConvaiReferenceAudioThread::Stop()
{
    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("ConvaiReferenceAudioThread stop requested"));
    bStopRequested = true;
}

void FConvaiReferenceAudioThread::Exit()
{
    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("ConvaiReferenceAudioThread exiting"));
    StopCapture();
}

void FConvaiReferenceAudioThread::StartCapture()
{
    if (bIsCapturing)
    {
        CONVAI_LOG(ConvaiReferenceAudioThread, Warning, TEXT("Reference audio capture already active"));
        return;
    }
    
    if (!ConvaiClient)
    {
        CONVAI_LOG(ConvaiReferenceAudioThread, Error, TEXT("ConvaiClient is null, cannot start reference audio capture"));
        return;
    }
    
    // Create and start the thread if it doesn't exist
    if (!Thread)
    {
        Thread = FRunnableThread::Create(this, ThreadName, 0, TPri_Normal);
        if (!Thread)
        {
            CONVAI_LOG(ConvaiReferenceAudioThread, Error, TEXT("Failed to create reference audio thread"));
            return;
        }
    }
    
    // Clear previous audio data
    AudioProcessingBuffer.Empty();
    bIsRecording = false;
    
    bIsCapturing = true;
    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("Reference audio capture started"));
}

void FConvaiReferenceAudioThread::StopCapture()
{
    if (!bIsCapturing)
    {
        CONVAI_LOG(ConvaiReferenceAudioThread, Warning, TEXT("Reference audio capture not active"));
        return;
    }
    
    bIsCapturing = false;
    
    // Stop recording if active
    if (bIsRecording)
    {
        if (Audio::FMixerDevice* MixerDevice = GetAudioMixerDevice())
        {
            float NumChannels, SampleRate;
            MixerDevice->StopRecording(nullptr, NumChannels, SampleRate);
            CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("Stopped recording reference audio"));
        }
        bIsRecording = false;
    }
    
    // Clear audio processing buffers
    AudioProcessingBuffer.Empty();
    
    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("Reference audio capture stopped"));
}

Audio::FMixerDevice* FConvaiReferenceAudioThread::GetAudioMixerDevice() const
{
    if (!WorldPtr.IsValid())
    {
        return nullptr;
    }
    
    UWorld* World = WorldPtr.Get();
    if (!World)
    {
        return nullptr;
    }
    
    if (FAudioDevice* AudioDevice = World->GetAudioDevice().GetAudioDevice())
    {
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
        bool Found = AudioDevice != nullptr;
#else
        bool Found = AudioDevice != nullptr && AudioDevice->IsAudioMixerEnabled();
#endif
        if (Found)
        {
            return static_cast<Audio::FMixerDevice*>(AudioDevice);
        }
    }
    return nullptr;
}

void FConvaiReferenceAudioThread::ProcessCapturedAudio()
{
    if (!WorldPtr.IsValid() || !ConvaiClient)
    {
        return;
    }

    // Get current audio data from the mixer device by stopping and restarting recording
    if (Audio::FMixerDevice* MixerDevice = GetAudioMixerDevice())
    {
        float NumChannels = 0.0f, SampleRate = 0.0f;
        Audio::AlignedFloatBuffer CurrentBuffer;

        if (bIsRecording)
        {
            // Stop recording to get the current buffer
            CurrentBuffer = MixerDevice->StopRecording(nullptr, NumChannels, SampleRate);
        }

        // Always restart recording to continue capture
        // We need to execute this on the game thread since UAudioMixerBlueprintLibrary requires it
        /*AsyncTask(ENamedThreads::GameThread, [this, WeakWorld = WorldPtr]()
        {
            if (WeakWorld.IsValid() && bIsCapturing)
            {
                UAudioMixerBlueprintLibrary::StartRecordingOutput(WeakWorld.Get(), 60.0f, nullptr);
            }
        });*/

        bIsRecording = true;

        if (CurrentBuffer.Num() > 0)
        {
            // Convert float samples to int16
            TArray<int16> PCMData;
            PCMData.Reserve(CurrentBuffer.Num());

            for (float Sample : CurrentBuffer)
            {
                int16 PCMSample = FMath::Clamp(Sample * 32767.0f, -32768.0f, 32767.0f);
                PCMData.Add(PCMSample);
            }



            // Resample if necessary
            TArray<int16> ResampledData;
            if (SampleRate != TargetSampleRate || NumChannels != 1)
            {
                UConvaiUtils::ResampleAudio(SampleRate, TargetSampleRate, (int)NumChannels, true, PCMData, PCMData.Num(), ResampledData);
            }
            else
            {
                ResampledData = PCMData;
            }

            // Add resampled data to processing buffer
            AudioProcessingBuffer.Append(ResampledData);

            // Process all complete chunks
            while (AudioProcessingBuffer.Num() >= ProcessingChunkSize)
            {
                SendAudioChunkToConvaiClient(AudioProcessingBuffer.GetData(), ProcessingChunkSize);

                // Remove processed samples from buffer
                AudioProcessingBuffer.RemoveAt(0, ProcessingChunkSize);
            }
        }
    }
}

void FConvaiReferenceAudioThread::SendAudioChunkToConvaiClient(const int16* AudioData, int32 NumSamples)
{
    if (!ConvaiClient || !AudioData || NumSamples <= 0)
    {
        return;
    }

    // Send reference audio to ConvaiClient
    ConvaiClient->SendReferenceAudio(AudioData, NumSamples);

    //CONVAI_LOG(ConvaiReferenceAudioThread, VeryVerbose, TEXT("Sent %d samples of reference audio to ConvaiClient"), NumSamples);
}



void FConvaiReferenceAudioThread::StartRecordingRefrence(TWeakObjectPtr<UWorld> WeakWorld)
{
    if (WeakWorld.IsValid() && bIsCapturing)
    {
        UAudioMixerBlueprintLibrary::StartRecordingOutput(WeakWorld.Get(), 60.0f, nullptr);
    }
}

