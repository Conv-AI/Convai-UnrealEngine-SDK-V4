// Fill out your copyright notice in the Description page of Project Settings.

#include "ConvaiReferenceAudioThread.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "ConvaiDefinitions.h"
#include "ConvaiUtils.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Utility/Log/ConvaiLogger.h"

DEFINE_LOG_CATEGORY_STATIC(ConvaiReferenceAudioThread, Log, All);

const TCHAR* FConvaiReferenceAudioThread::ThreadName = TEXT("ConvaiReferenceAudioThread");

FConvaiReferenceAudioThread::FConvaiReferenceAudioThread(convai::ConvaiClient* InConvaiClient, UWorld* InWorld)
    : Reblocker(ConvaiConstants::VoiceCaptureSampleRate, FMath::Max(1, ConvaiConstants::VoiceCaptureSampleRate / 100))
    , Thread(nullptr)
    , bStopRequested(false)
    , bIsCapturing(false)
    , ConvaiClient(InConvaiClient)
    , WorldPtr(InWorld)
    , ProcessingChunkSize(0)
    , TargetSampleRate(ConvaiConstants::VoiceCaptureSampleRate)
    , bIsRecording(false)
    , LastCaptureTime(0.0)
    , CaptureInterval(0.01) // 10ms
    , ChunksSent(0)
    , ClientSends(0)
    , CapturedSamples(0)
    , LastDispatchSeconds(0.0)
    , FirstStopSeconds(0.0)
    , LastStopSeconds(0.0)
    , LastCapturedSampleRate(0)
    , LastCapturedChannels(0)
    , RecorderStoppedAtSeconds(0.0)
{
    // Calculate processing chunk size for 10ms frames (also used as a divisor when
    // draining the buffer, so it must never be zero)
    ProcessingChunkSize = FMath::Max(1, TargetSampleRate / 100);

    // Initialize audio buffer
    AudioProcessingBuffer.Empty();
    AudioProcessingBuffer.Reserve(ProcessingChunkSize * 10); // Reserve space for multiple chunks

    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("ConvaiReferenceAudioThread created with chunk size: %d"), ProcessingChunkSize);
}

FConvaiReferenceAudioThread::~FConvaiReferenceAudioThread()
{
    // Join the polling thread *first*. Run() calls AsShared() on this object, which
    // asserts once the last shared reference is gone, and it keeps queueing audio
    // thread commands until it observes bStopRequested. Stopping capture before the
    // join would leave both of those racing against destruction.
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

    // Exit() already called this on the runnable thread when Run() returned; this
    // covers the case where the thread was never created.
    StopCapture();

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
        if (bIsCapturing && !bUseListenerTap)
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
                                // A command queued before StopCapture() can still be
                                // dispatched after it; skip it instead of touching
                                // capture state that has already been torn down.
                                if (WeakWorld.IsValid() && SharedThis->IsCapturing())
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
    
    // Listener first, recorder only if it cannot be attached. The recorder
    // drops every buffer rendered while it is stopped, which is the whole of
    // each cycle's conversion and send.
    bUseListenerTap = !UConvaiUtils::GetReferenceCaptureTap().Equals(TEXT("Recorder"), ESearchCase::IgnoreCase);
    if (bUseListenerTap && !RegisterSubmixTap())
    {
        CONVAI_LOG(ConvaiReferenceAudioThread, Warning,
                   TEXT("Reference submix tap unavailable, falling back to the output recorder"));
        bUseListenerTap = false;
    }

    // The listener is driven by the mixer; only the recorder path needs a poll.
    if (!bUseListenerTap && !Thread)
    {
        Thread = FRunnableThread::Create(this, ThreadName, 0, TPri_Normal);
        if (!Thread)
        {
            CONVAI_LOG(ConvaiReferenceAudioThread, Error, TEXT("Failed to create reference audio thread"));
            UnregisterSubmixTap();
            return;
        }
    }
    
    {
        FScopeLock Lock(&CaptureCS);

        // Clear previous audio data
        AudioProcessingBuffer.Empty();
        AudioProcessingBuffer.Reserve(ProcessingChunkSize * 10);
        Reblocker.Reset();
        bIsRecording = false;

        bIsCapturing = true;
    }

    // Counters describe one capture, not the lifetime of the object: a caller
    // that reads them after a restart would otherwise be shown the previous
    // capture's cadence.
    {
        FScopeLock Lock(&StatsMutex);
        DispatchGaps = FGapAccumulator();
        RecorderOffGaps = FGapAccumulator();
        ChunksSent = 0;
        ClientSends = 0;
        CapturedSamples = 0;
        LastDispatchSeconds = 0.0;
        FirstStopSeconds = 0.0;
        LastStopSeconds = 0.0;
    }
    RecorderStoppedAtSeconds = 0.0;

    CONVAI_LOG(ConvaiReferenceAudioThread, Log, TEXT("Reference audio capture started (tap=%s)"),
               bUseListenerTap ? TEXT("Listener") : TEXT("Recorder"));
}

void FConvaiReferenceAudioThread::StopCapture()
{
    // Held for the whole teardown so an in-flight ProcessCapturedAudio() on the audio
    // thread cannot observe a half-torn-down state (or restart the mixer recording
    // after we have stopped it).
    FScopeLock Lock(&CaptureCS);

    if (!bIsCapturing)
    {
        CONVAI_LOG(ConvaiReferenceAudioThread, Warning, TEXT("Reference audio capture not active"));
        return;
    }

    bIsCapturing = false;

    UnregisterSubmixTap();

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

    RecordTapDispatch();

    // Chunks are staged here under the lock and sent to the client after releasing it,
    // so a blocking call into convai_client.dll never stalls StopCapture().
    TArray<int16> ChunksToSend;

    {
        FScopeLock Lock(&CaptureCS);

        // StopCapture() may have run between this command being queued and it being
        // dispatched on the audio thread.
        if (!bIsCapturing)
        {
            return;
        }

        // Get current audio data from the mixer device by stopping and restarting recording
        Audio::FMixerDevice* MixerDevice = GetAudioMixerDevice();
        if (!MixerDevice)
        {
            return;
        }

        float NumChannels = 0.0f, SampleRate = 0.0f;
        Audio::AlignedFloatBuffer CurrentBuffer;

        if (bIsRecording)
        {
            // Stop recording to get the current buffer
            CurrentBuffer = MixerDevice->StopRecording(nullptr, NumChannels, SampleRate);

            RecorderStoppedAtSeconds = FPlatformTime::Seconds();

            FScopeLock StatsLock(&StatsMutex);
            if (FirstStopSeconds <= 0.0)
            {
                // This buffer holds audio from before the window opened, so it
                // is timed but not counted.
                FirstStopSeconds = RecorderStoppedAtSeconds;
            }
            else
            {
                CapturedSamples += CurrentBuffer.Num();
            }
            LastStopSeconds = RecorderStoppedAtSeconds;
            LastCapturedSampleRate = static_cast<int32>(SampleRate);
            LastCapturedChannels = static_cast<int32>(NumChannels);
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

            // Detach every complete chunk in a single pass. The buffer is only ever
            // resized while holding CaptureCS, so the count measured here is still the
            // count being removed - previously StopCapture() could empty the buffer
            // between the size check and the RemoveAt, tripping the TArray range check.
            const int32 NumCompleteChunks = AudioProcessingBuffer.Num() / ProcessingChunkSize;
            if (NumCompleteChunks > 0)
            {
                const int32 NumSamplesToSend = NumCompleteChunks * ProcessingChunkSize;
                ChunksToSend.Append(AudioProcessingBuffer.GetData(), NumSamplesToSend);
                AudioProcessingBuffer.RemoveAt(0, NumSamplesToSend);
            }
        }
    }

    // Sent outside the lock: SendReferenceAudio() crosses into convai_client.dll and
    // must not be able to block a game thread StopCapture().
    for (int32 Offset = 0; Offset + ProcessingChunkSize <= ChunksToSend.Num(); Offset += ProcessingChunkSize)
    {
        SendAudioChunkToConvaiClient(ChunksToSend.GetData() + Offset, ProcessingChunkSize);
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

    FScopeLock Lock(&StatsMutex);
    ++ChunksSent;
    // One client on this branch; the field stays chunks-times-clients so the
    // metric means the same thing wherever the capture fans out to more.
    ++ClientSends;
}

void FConvaiReferenceAudioThread::RecordTapDispatch()
{
    // Cadence is measured where the work happens rather than where it is asked
    // for: the recorder tap dispatches asynchronously, so the interval between
    // these executions is the one the canceller actually receives.
    const double Now = FPlatformTime::Seconds();

    FScopeLock Lock(&StatsMutex);
    if (LastDispatchSeconds > 0.0)
    {
        DispatchGaps.Add((Now - LastDispatchSeconds) * 1000.0);
    }
    LastDispatchSeconds = Now;
}

void FConvaiReferenceAudioThread::FGapAccumulator::Add(double ValueMs)
{
    ++Count;
    Sum += ValueMs;
    SumSquares += ValueMs * ValueMs;
    Max = FMath::Max(Max, ValueMs);
    Last = ValueMs;
}

void FConvaiReferenceAudioThread::FGapAccumulator::Read(int64& OutCount, double& OutLast,
                                                        double& OutMean, double& OutMax,
                                                        double& OutStdDev) const
{
    OutCount = Count;
    OutLast = Last;
    OutMax = Max;
    if (Count <= 0)
    {
        return;
    }
    OutMean = Sum / Count;
    OutStdDev = FMath::Sqrt(FMath::Max(0.0, SumSquares / Count - OutMean * OutMean));
}

FConvaiReferenceAudioThread::FStats FConvaiReferenceAudioThread::GetStats() const
{
    FStats Stats;

    FScopeLock Lock(&StatsMutex);
    Stats.ChunksSent = ChunksSent;
    Stats.ClientSends = ClientSends;
    Stats.CapturedSamples = CapturedSamples;
    Stats.bSubmixListenerTap = bUseListenerTap;
    Stats.CapturedSampleRate = LastCapturedSampleRate;
    Stats.CapturedChannels = LastCapturedChannels;

    DispatchGaps.Read(Stats.DispatchCount, Stats.DispatchGapLastMs, Stats.DispatchGapMeanMs,
                      Stats.DispatchGapMaxMs, Stats.DispatchGapStdDevMs);
    RecorderOffGaps.Read(Stats.RecorderOffCount, Stats.RecorderOffLastMs, Stats.RecorderOffMeanMs,
                         Stats.RecorderOffMaxMs, Stats.RecorderOffStdDevMs);

    // The window runs between the first and last StopRecording, and the first
    // buffer is excluded from CapturedSamples because it holds audio from
    // before the window opened. So the ratio is what this loop retained of what
    // the mixer rendered while it was looping — the rest was rendered while the
    // recorder was stopped and dropped.
    const double Window = LastStopSeconds - FirstStopSeconds;
    const double ExpectedSamples =
        Window * LastCapturedSampleRate * FMath::Max(1, LastCapturedChannels);
    Stats.CaptureWindowSeconds = FMath::Max(0.0, Window);
    if (ExpectedSamples > 0.0)
    {
        Stats.CaptureRatio = CapturedSamples / ExpectedSamples;
    }
    return Stats;
}



void FConvaiReferenceAudioThread::StartRecordingRefrence(TWeakObjectPtr<UWorld> WeakWorld)
{
    // Same lock as StopCapture(), otherwise a stop landing between the bIsCapturing
    // check and the call below would restart output recording with nothing left to
    // ever stop it.
    FScopeLock Lock(&CaptureCS);

    if (!WeakWorld.IsValid() || !bIsCapturing)
    {
        // The recorder is not coming back, so this cycle has no window to
        // close — timing it would report a puncture as an ordinary gap.
        RecorderStoppedAtSeconds = 0.0;
        return;
    }

    UAudioMixerBlueprintLibrary::StartRecordingOutput(WeakWorld.Get(), 60.0f, nullptr);

    if (RecorderStoppedAtSeconds > 0.0)
    {
        const double OffMs = (FPlatformTime::Seconds() - RecorderStoppedAtSeconds) * 1000.0;
        RecorderStoppedAtSeconds = 0.0;

        FScopeLock StatsLock(&StatsMutex);
        RecorderOffGaps.Add(OffMs);
    }
}



void FConvaiReferenceAudioThread::FSubmixTap::Detach()
{
    FScopeLock Lock(&OwnerLock);
    Owner = nullptr;
}

void FConvaiReferenceAudioThread::FSubmixTap::OnNewSubmixBuffer(const USoundSubmix*, float* AudioData,
                                                                int32 NumSamples, int32 InNumChannels,
                                                                const int32 InSampleRate, double)
{
    FScopeLock Lock(&OwnerLock);
    if (Owner)
    {
        Owner->HandleSubmixBuffer(AudioData, NumSamples, InNumChannels, InSampleRate);
    }
}

void FConvaiReferenceAudioThread::HandleSubmixBuffer(float* AudioData, int32 NumSamples,
                                                     int32 InNumChannels, int32 InSampleRate)
{
    if (!ConvaiClient || !bIsCapturing || !AudioData || NumSamples <= 0)
    {
        return;
    }

    // Render thread, and the only thread that touches the reblocker: StopCapture
    // detaches the tap before anything else can.
    const double Now = FPlatformTime::Seconds();
    Reblocker.PushInterleavedFloat(AudioData, NumSamples, InNumChannels, InSampleRate);

    int32 Sent = 0;
    Reblocker.Drain([this, &Sent](const int16* Chunk, int32 Num)
    {
        ConvaiClient->SendReferenceAudio(Chunk, Num);
        ++Sent;
    });

    FScopeLock Lock(&StatsMutex);
    if (LastDispatchSeconds > 0.0)
    {
        DispatchGaps.Add((Now - LastDispatchSeconds) * 1000.0);
    }
    LastDispatchSeconds = Now;

    // Same window convention as the recorder path: the first buffer opens the
    // window rather than counting toward it, so the ratio measures what this
    // tap retained of what the mixer rendered while it was attached.
    if (FirstStopSeconds <= 0.0)
    {
        FirstStopSeconds = Now;
    }
    else
    {
        CapturedSamples += NumSamples;
    }
    LastStopSeconds = Now;
    LastCapturedSampleRate = InSampleRate;
    LastCapturedChannels = InNumChannels;
    ChunksSent += Sent;
    ClientSends += Sent;
}

bool FConvaiReferenceAudioThread::RegisterSubmixTap()
{
    Audio::FMixerDevice* MixerDevice = GetAudioMixerDevice();
    if (!MixerDevice)
    {
        return false;
    }

    SubmixTap = MakeShared<FSubmixTap, ESPMode::ThreadSafe>(this);
    ConvaiRegisterSubmixListener(MixerDevice, SubmixTap.ToSharedRef(), nullptr);
    return true;
}

void FConvaiReferenceAudioThread::UnregisterSubmixTap()
{
    if (!SubmixTap.IsValid())
    {
        return;
    }

    if (Audio::FMixerDevice* MixerDevice = GetAudioMixerDevice())
    {
        ConvaiUnregisterSubmixListener(MixerDevice, SubmixTap.ToSharedRef(), nullptr);
    }
    // After this returns no callback is running or can start, so the reblocker
    // and the stats belong to this thread again.
    SubmixTap->Detach();
    SubmixTap.Reset();
}
