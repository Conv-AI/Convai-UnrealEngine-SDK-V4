// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiAudioStreamer.h"
#include "Sound/SoundWave.h"
#include "Engine.h"
#include "Net/UnrealNetwork.h"
#include "Kismet/KismetSystemLibrary.h"
#include "LipSyncInterface.h"
#include "VisionInterface.h"
#include "Math/UnrealMathUtility.h"
#include "ConvaiUtils.h"
#include "Utility/Log/ConvaiLogger.h"


DEFINE_LOG_CATEGORY(ConvaiAudioStreamerLog);
DEFINE_LOG_CATEGORY(ConvaiThreadSafeBuffersLog);

UConvaiAudioStreamer::UConvaiAudioStreamer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, bIsPlayingAudio(false)
	, TotalAudioBytesReceived(0)
	, LastReceivedSampleRate(0)
	, LastReceivedNumChannels(0)
	, TotalAudioFramesReceived(0)
	, TotalBytesQueuedToAudio(0)
	, AudioPlaybackSampleRate(0)
	, AudioPlaybackNumChannels(0)
	, AudioPlaybackStartTime(0.0)
{
	PrimaryComponentTick.bCanEverTick = true;
	bAutoActivate = true;
}


void UConvaiAudioStreamer::PlayVoiceData(uint8* VoiceData, uint32 VoiceDataSize, bool ContainsHeaderData, uint32 SampleRate, uint32 NumChannels)
{
    if (ContainsHeaderData)
    {
        // Parse Wav header
        FWaveModInfo WaveInfo;
        FString ErrorReason;
        bool ParseSuccess = WaveInfo.ReadWaveInfo(VoiceData, VoiceDataSize, &ErrorReason);
        // Set the number of channels and sample rate for the first time reading from the stream
        if (ParseSuccess)
        {
            // Validate that the world exists
            if (!IsValid(GetWorld()))
                return;

            SampleRate = *WaveInfo.pSamplesPerSec;
            NumChannels = *WaveInfo.pChannels;

			// Play only the PCM data which start after 44 bytes
			VoiceData += 44;
			VoiceDataSize -= 44;
        }
        else if (!ParseSuccess)
        {
            CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("PlayVoiceData: Failed to parse wav header, reason: %s"), *ErrorReason);
        }
    }

    if (!IsValid(SoundWaveProcedural))
        return;

    // Check if we need to reconfigure the sound wave (first time or format change)
    if (SoundWaveProcedural->GetSampleRateForCurrentPlatform() != SampleRate || SoundWaveProcedural->NumChannels != NumChannels)
    {
        // Helper lambda to setup the sound wave (must be called on game thread)
        auto SetupSoundWaveOnGameThread = [this, SampleRate, NumChannels]()
        {
            if (!IsValid(SoundWaveProcedural))
                return;

            SoundWaveProcedural->SetSampleRate(SampleRate);
            SoundWaveProcedural->NumChannels = NumChannels;
            SoundWaveProcedural->Duration = INDEFINITELY_LOOPING_DURATION;
            SoundWaveProcedural->SoundGroup = SOUNDGROUP_Voice;
            SoundWaveProcedural->bLooping = false;
            SoundWaveProcedural->bProcedural = true;
            SoundWaveProcedural->Pitch = 1.0f;
            SoundWaveProcedural->Volume = 1.0f;
            SoundWaveProcedural->AttenuationSettings = nullptr;
            SoundWaveProcedural->bDebug = true;
            SoundWaveProcedural->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;

            CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("New SampleRate: %d"), SampleRate);
            CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("New Channels: %d"), NumChannels);

            SetSound(SoundWaveProcedural);
			Play();
            ForceRecalculateLipsyncStartTime();
        };

        // Execute on game thread (blocking if not already on game thread)
        if (IsInGameThread())
        {
            SetupSoundWaveOnGameThread();
        }
        else
        {
            // Use FGraphEventRef to make this blocking
            FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady(
                [SetupSoundWaveOnGameThread]()
                {
                    SetupSoundWaveOnGameThread();
                },
                TStatId(),
                nullptr,
                ENamedThreads::GameThread
            );

            // Wait for the task to complete
            FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task);
        }
    }
    
    SoundWaveProcedural->QueueAudio(VoiceData, VoiceDataSize);

    // Track bytes queued for playback time calculation
    TotalBytesQueuedToAudio += VoiceDataSize;
    AudioPlaybackSampleRate = SampleRate;
    AudioPlaybackNumChannels = NumChannels;

    if (!IsTalking)
    {
        AudioPlaybackStartTime = FPlatformTime::Seconds();
        onAudioStarted();
        IsTalking = true;
    }

}

void UConvaiAudioStreamer::ForcePlayVoice(USoundWave* VoiceToPlay)
{
	if (!IsInGameThread())
	{
		// Dispatch to game thread if not already on it
		TWeakObjectPtr<UConvaiAudioStreamer> WeakThis(this);
		TWeakObjectPtr<USoundWave> WeakVoice(VoiceToPlay);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakVoice]()
		{
			// Check validity before use - we're on game thread so GC won't run mid-function
			if (WeakThis.IsValid() && WeakVoice.IsValid())
			{
				WeakThis->ForcePlayVoice(WeakVoice.Get());
			}
		});
		return;
	}

	int32 SampleRate;
	int32 NumChannels;
	TArray<uint8> PCMData = UConvaiUtils::ExtractPCMDataFromSoundWave(VoiceToPlay, SampleRate, NumChannels);

	// Enqueue audio data to the ring buffer
	HandleAudioReceived(PCMData.GetData(), PCMData.Num(), false, SampleRate, NumChannels);

	// Force process and play the audio immediately
	ProcessIncomingAudio();
}

void UConvaiAudioStreamer::StopVoice()
{    
    // Notify that audio has finished if we were talking
    if (IsTalking)
        onAudioFinished();

    // Reset audio playback
    if (SoundWaveProcedural)
    {
		SoundWaveProcedural->ResetAudio();
	}

    // Clear audio buffer
	AudioRingBuffer.Reset();

	// Reset playback time tracking
	TotalBytesQueuedToAudio = 0;
	AudioPlaybackStartTime = 0.0;

	// Reset voice fade tracking
	ResetVoiceFade();

    // Reset lipsync state
    StopLipSync();
}

void UConvaiAudioStreamer::PauseVoice()
{
	if (bIsPaused)
		return;

	SetPaused(true);
	IsTalking = false;
}

void UConvaiAudioStreamer::ResumeVoice()
{
	if (!bIsPaused)
		return;

	SetPaused(false);
}

void UConvaiAudioStreamer::StopVoiceWithFade(float InVoiceFadeOutDuration)
{
	if (!IsTalking && AudioRingBuffer.IsEmpty())
		return;

	if (!IsValid(GetWorld()))
	{
		CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("PlayVoiceData: GetWorld() is Invalid!"));
		return;
	}

	double CurrentRemainingAudioDuration = GetRemainingContentDuration();
	TotalVoiceFadeOutTime = FMath::Min(InVoiceFadeOutDuration, CurrentRemainingAudioDuration);
	RemainingVoiceFadeOutTime = TotalVoiceFadeOutTime;

	if (TotalVoiceFadeOutTime <= 0)
		StopVoice();
}

void UConvaiAudioStreamer::ResetVoiceFade()
{
	if (IsValid(SoundWaveProcedural))
		SoundWaveProcedural->Volume = 1.0f;
	TotalVoiceFadeOutTime = 0;
	RemainingVoiceFadeOutTime = 0;
}

void UConvaiAudioStreamer::UpdateVoiceFade(float DeltaTime)
{
	if (!IsVoiceCurrentlyFading() || !IsValid(SoundWaveProcedural))
		return;
	RemainingVoiceFadeOutTime -= DeltaTime;
	if (RemainingVoiceFadeOutTime <= 0)
	{
		StopVoice();
		return;
	}
	float AudioVolume = RemainingVoiceFadeOutTime / TotalVoiceFadeOutTime;
	SoundWaveProcedural->Volume = AudioVolume;
}

bool UConvaiAudioStreamer::IsVoiceCurrentlyFading() const
{
	return (TotalVoiceFadeOutTime > 0 && IsTalking);
}

// Not used
bool UConvaiAudioStreamer::IsLocal() const
{
	const ENetMode NetMode = GetNetMode();

	if (NetMode == NM_Standalone)
	{
		// Not networked.
		return true;
	}

	if (NetMode == NM_Client && GetOwner()->GetLocalRole() == ROLE_AutonomousProxy)
	{
		// Networked client in control.
		return true;
	}

	if (GetOwner()->GetRemoteRole() != ROLE_AutonomousProxy && GetOwner()->GetLocalRole() == ROLE_Authority)
	{
		// Local authority in control.
		return true;
	}
	return false;
}

IConvaiLipSyncInterface* UConvaiAudioStreamer::FindFirstLipSyncComponent()
{
	// Find the LipSync component
	auto LipSyncComponents = (GetOwner()->GetComponentsByInterface(UConvaiLipSyncInterface::StaticClass()));
	if (LipSyncComponents.Num())
	{
		SetLipSyncComponent(LipSyncComponents[0]);
	}
	return ConvaiLipSync;
}

bool UConvaiAudioStreamer::SetLipSyncComponent(UActorComponent* LipSyncComponent)
{
	if (!CanUseLipSync())
		return false;

	// Find the LipSync component
	if (LipSyncComponent && LipSyncComponent->GetClass()->ImplementsInterface(UConvaiLipSyncInterface::StaticClass()))
	{
		ConvaiLipSync = Cast<IConvaiLipSyncInterface>(LipSyncComponent);
		ConvaiLipSync->OnFacialDataReady.BindUObject(this, &UConvaiAudioStreamer::OnFacialDataReadyCallback);

		// Set up audio playback time provider for audio-synced lipsync
		ConvaiLipSync->SetAudioPlaybackTimeProvider(FGetAudioPlaybackTimeDelegate::CreateUObject(this, &UConvaiAudioStreamer::GetAudioPlaybackTime));

		return true;
	}
	else
	{
		ConvaiLipSync = nullptr;
		return false;
	}
}

bool UConvaiAudioStreamer::SupportsLipSync()
{
	if (!CanUseLipSync())
		return false;

	if (ConvaiLipSync == nullptr)
	{
		FindFirstLipSyncComponent();
	}
	return ConvaiLipSync != nullptr;
}

IConvaiVisionInterface* UConvaiAudioStreamer::FindFirstVisionComponent()
{
	// Find the Vision component
	auto VisionComponents = (GetOwner()->GetComponentsByInterface(UConvaiVisionInterface::StaticClass()));
	if (VisionComponents.Num())
	{
		SetVisionComponent(VisionComponents[0]);
	}
	return ConvaiVision;
}

bool UConvaiAudioStreamer::SetVisionComponent(UActorComponent* VisionComponent)
{
	if (!CanUseVision())
		return false;

	// Find the Vision component
	if (VisionComponent && VisionComponent->GetClass()->ImplementsInterface(UConvaiVisionInterface::StaticClass()))
	{
		ConvaiVision = Cast<IConvaiVisionInterface>(VisionComponent);
		return true;
	}
	else
	{
		ConvaiVision = nullptr;
		return false;
	}
}

bool UConvaiAudioStreamer::SupportsVision()
{
	if (!CanUseVision())
		return false;

	if (ConvaiVision == nullptr)
	{
		FindFirstVisionComponent();
	}
	return ConvaiVision != nullptr;
}

void UConvaiAudioStreamer::BeginPlay()
{
	Super::BeginPlay();

    // Initialize state
    bIsPlayingAudio = false;
    
    // Initialize configuration parameters

	// Minimum buffer duration in seconds
	MinBufferDuration = UConvaiSettingsUtils::GetParamValueAsFloat("MinBufferDuration", MinBufferDuration) ? MinBufferDuration : 0.2;
	MinBufferDuration = MinBufferDuration < 0 ? 0 : MinBufferDuration;

	// Initialize the audio component
	bAutoActivate = true;
	bAlwaysPlay = true;

	SoundWaveProcedural = NewObject<USoundWaveProcedural>();

	if (ConvaiLipSync == nullptr)
		FindFirstLipSyncComponent();

	if (ConvaiVision == nullptr)
		FindFirstVisionComponent();
}



// Handle received audio data (called from transport thread - lightweight)
void UConvaiAudioStreamer::HandleAudioReceived(uint8* AudioData, uint32 AudioDataSize, bool ContainsHeaderData, uint32 SampleRate, uint32 NumChannels)
{
    AudioRingBuffer.SetFormat(SampleRate, NumChannels);
    if (!AudioRingBuffer.Enqueue(AudioData, AudioDataSize))
    {
        UE_LOG(ConvaiAudioStreamerLog, Warning, TEXT("AudioRingBuffer overflow - dropped %d bytes"), AudioDataSize);
    }

	// Also enqueue to lipsync buffer if lipsync component doesn't use precomputed data
	if (SupportsLipSync() && !ConvaiLipSync->RequiresPrecomputedFaceData())
	{
		LipSyncAudioRingBuffer.SetFormat(SampleRate, NumChannels);
		if (!LipSyncAudioRingBuffer.Enqueue(AudioData, AudioDataSize))
		{
			UE_LOG(ConvaiAudioStreamerLog, Warning, TEXT("LipSyncAudioRingBuffer overflow - dropped %d bytes"), AudioDataSize);
		}
	}

    // Track audio received from WebRTC
    TotalAudioBytesReceived += AudioDataSize;
    LastReceivedSampleRate = SampleRate;
    LastReceivedNumChannels = NumChannels;

    // Record audio if recording is enabled
    OnAudioReceivedForRecording(AudioData, AudioDataSize, SampleRate, NumChannels);
}

// Helper function to dequeue audio data if ready, returns number of bytes dequeued (0 if not ready)
uint32 UConvaiAudioStreamer::TryDequeueAudioChunk(TArray<uint8>& OutAudioData, uint32& OutSampleRate, uint32& OutNumChannels, bool Force)
{
    // Get audio format
    AudioRingBuffer.GetFormat(OutSampleRate, OutNumChannels);

    if (OutSampleRate == 0 || OutNumChannels == 0)
    {
        return 0; // Format not set yet
    }

    // Check if we have any data to process
    uint32 AvailableBytes = AudioRingBuffer.GetAvailableBytes();
    if (AvailableBytes == 0)
    {
        return 0; // No data to process
    }

    // Check if we should wait for more buffering
    if (!bIsPlayingAudio && MinBufferDuration > 0.0f)
    {
        // Calculate how much audio duration we have buffered
        double BufferedDuration = UConvaiUtils::CalculateAudioDuration(AvailableBytes, OutNumChannels, OutSampleRate, 2);

        if (BufferedDuration < MinBufferDuration && !Force)
        {
            // Not enough data buffered yet, wait for more
            return 0;
        }
    }

    // Ensure buffer has capacity (only allocates once, keeps slack for reuse)
    if (OutAudioData.Num() < (int32)MaxChunkSize)
    {
        OutAudioData.SetNumUninitialized(MaxChunkSize);
    }

    // Dequeue audio chunk - buffer stays at MaxChunkSize with slack, we return valid byte count
    uint32 BytesRead = AudioRingBuffer.Dequeue(OutAudioData.GetData(), MaxChunkSize);

    return BytesRead;
}

// Process incoming audio for lipsync components that infer facial data from audio in real-time (non-precomputed)
void UConvaiAudioStreamer::ProcessIncomingNonPrecomputedLipSync()
{
	if (!SupportsLipSync() || ConvaiLipSync->RequiresPrecomputedFaceData())
		return;

	// Get audio format
	uint32 SampleRate, NumChannels;
	LipSyncAudioRingBuffer.GetFormat(SampleRate, NumChannels);

	if (SampleRate == 0 || NumChannels == 0)
		return;

	// Check if we have any data to process
	uint32 AvailableBytes = LipSyncAudioRingBuffer.GetAvailableBytes();
	if (AvailableBytes == 0)
		return;

	// Ensure buffer has capacity (only allocates once, keeps slack for reuse)
	if (LipSyncAudioChunkBuffer.Num() < (int32)MaxChunkSize)
	{
		LipSyncAudioChunkBuffer.SetNumUninitialized(MaxChunkSize);
	}

	// Dequeue audio immediately without buffering constraints (no MinBufferDuration check)
	uint32 BytesRead = LipSyncAudioRingBuffer.Dequeue(LipSyncAudioChunkBuffer.GetData(), MaxChunkSize);

	if (BytesRead > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("ProcessIncomingNonPrecomputedLipSync: BytesRead = %u, AudioDuration = %f"), BytesRead, UConvaiUtils::CalculateAudioDuration(BytesRead, NumChannels, SampleRate, 2));
		// Send audio to lipsync component for real-time inference
		ConvaiLipSync->ConvaiInferFacialDataFromAudio(
			LipSyncAudioChunkBuffer.GetData(),
			BytesRead,
			SampleRate,
			NumChannels
		);

		// Pause lipsync if we're not currently talking (audio hasn't started playing yet)
		if (!IsTalking)
		{
			PauseLipSync();
		}
	}
}

// Process incoming audio data (called from game thread - heavy logic)
void UConvaiAudioStreamer::ProcessIncomingAudio(bool Force)
{
    // Get audio format
    uint32 SampleRate, NumChannels;
    AudioRingBuffer.GetFormat(SampleRate, NumChannels);

    if (SampleRate == 0 || NumChannels == 0)
    {
        return;
    }

    // Check if we have any data to process
    uint32 AvailableBytes = AudioRingBuffer.GetAvailableBytes();
    if (AvailableBytes == 0)
    {
        return;
    }

    // Try to dequeue an audio chunk for playback
    uint32 DequeuedBytes = TryDequeueAudioChunk(AudioChunkBuffer, SampleRate, NumChannels, Force);
    if (DequeuedBytes == 0)
    {
        return;
    }

    // Play the audio chunk (use DequeuedBytes, not AudioChunkBuffer.Num() which has slack)
    PlayVoiceData(AudioChunkBuffer.GetData(), DequeuedBytes, false, SampleRate, NumChannels);

	// Mark as playing and resume lipsync on first chunk
	if (!bIsPlayingAudio)
	{
		bIsPlayingAudio = true;

		// Resume lipsync when starting to play
		if (SupportsLipSync())
		{
			CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("[AudioStreamer] ProcessIncomingAudio - Starting Lipsync"));
			ForceRecalculateLipsyncStartTime();
			ResumeLipSync();
		}
	}
}

// Force play any buffered audio
void UConvaiAudioStreamer::ForcePlayBufferedAudio()
{
	ProcessIncomingAudio(true);
}

// Handle received lipsync data (called from transport thread - lightweight)
void UConvaiAudioStreamer::HandleLipSyncReceived(FAnimationSequence& FaceSequence)
{
	if(SupportsLipSync())
		LipSyncBuffer.Enqueue(FaceSequence);
}

// Process precomputed facial animation data from server (called from game thread - heavy logic)
void UConvaiAudioStreamer::ProcessIncomingPrecomputedLipSync()
{
	if (!ConvaiLipSync || !SupportsLipSync() || !ConvaiLipSync->RequiresPrecomputedFaceData())
		return;

    FAnimationSequence Sequence;
    if (LipSyncBuffer.Dequeue(Sequence))
    {
        // Send accumulated lipsync data to the component
        if (SupportsLipSync() && ConvaiLipSync)
        {
            // If audio is not playing, pause the lipsync before sending
            if (!bIsPlayingAudio)
            {
                ConvaiLipSync->ConvaiPauseLipSync();
            }

            // Send the precomputed facial animation data
            ConvaiLipSync->ConvaiApplyPrecomputedFacialAnimation(nullptr, 0, 0, 0, Sequence);
        }
    }
}



void UConvaiAudioStreamer::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

#if ConvaiDebugMode
	const double TickStart = FPlatformTime::Seconds();
	const double FadeStart = FPlatformTime::Seconds();
#endif

	UpdateVoiceFade(DeltaTime);

#if ConvaiDebugMode
	const double FadeMs = (FPlatformTime::Seconds() - FadeStart) * 1000.0;
	const double AudioStart = FPlatformTime::Seconds();
#endif

	// Process incoming data from transport thread
	ProcessIncomingAudio();

#if ConvaiDebugMode
	const double AudioMs = (FPlatformTime::Seconds() - AudioStart) * 1000.0;
	const double LipSyncStart = FPlatformTime::Seconds();
#endif

	// Process lipsync based on component type
	if (SupportsLipSync())
	{
		if (ConvaiLipSync->RequiresPrecomputedFaceData())
		{
			// Process precomputed facial animation data from server
			ProcessIncomingPrecomputedLipSync();
		}
		else
		{
			// Process audio for real-time lipsync inference
			ProcessIncomingNonPrecomputedLipSync();
		}
	}

#if ConvaiDebugMode
	const double LipSyncMs = (FPlatformTime::Seconds() - LipSyncStart) * 1000.0;
#endif

#if ConvaiDebugMode
	if (IsTalking && IsValid(SoundWaveProcedural) && AudioPlaybackSampleRate > 0)
	{
		static int32 DebugTickCount = 0;
		if (++DebugTickCount % 30 == 0)
		{
			double PlaybackTime = GetAudioPlaybackTime();
			double WallClock    = FPlatformTime::Seconds() - AudioPlaybackStartTime;
			int64 BytesEnqueued = static_cast<int64>(TotalBytesQueuedToAudio);
			int32 BytesAvail    = SoundWaveProcedural->GetAvailableAudioByteCount();
			int64 BytesPlayed   = FMath::Max<int64>(0, BytesEnqueued - BytesAvail);
			double ByteTime     = static_cast<double>(BytesPlayed) / (static_cast<double>(AudioPlaybackSampleRate) * AudioPlaybackNumChannels * 2.0);
			double RemainingDuration = GetRemainingContentDuration();
			bool bRingBufferEmpty = AudioRingBuffer.IsEmpty();
			CONVAI_LOG(ConvaiAudioStreamerLog, Log,
				TEXT("[AudioDbg] PlaybackTime=%.3fs | WallClock=%.3fs | ByteTime=%.3fs | Enqueued=%lld Avail=%d Played=%lld | RemainingDuration=%.6fs | RingBufferEmpty=%d"),
				PlaybackTime, WallClock, ByteTime, BytesEnqueued, BytesAvail, BytesPlayed, RemainingDuration, bRingBufferEmpty ? 1 : 0);
		}
	}
#endif

#if ConvaiDebugMode
	const double FinishCheckStart = FPlatformTime::Seconds();
#endif

	// Check if audio has finished playing
	// Use a small epsilon to avoid floating-point precision issues where
	// RemainingContentDuration hovers at a tiny positive value and never reaches zero
	constexpr double AudioFinishEpsilon = 0.005; // 5ms tolerance
	if (IsTalking && IsValid(SoundWaveProcedural))
	{
		if (GetRemainingContentDuration() <= AudioFinishEpsilon && AudioRingBuffer.IsEmpty())
		{
			onAudioFinished();
		}
	}

#if ConvaiDebugMode
	const double FinishCheckMs = (FPlatformTime::Seconds() - FinishCheckStart) * 1000.0;
	const double TickMs = (FPlatformTime::Seconds() - TickStart) * 1000.0;

	if (IsTalking)
	{
		// Accumulate per-tick stats
		AudioStreamerStats.SampleCount++;
		auto AccumStat = [](FConvaiTimingStat& Stat, double Val)
		{
			Stat.Total += Val;
			if (Val < Stat.Min) Stat.Min = Val;
			if (Val > Stat.Max) Stat.Max = Val;
		};
		AccumStat(AudioStreamerStats.Tick, TickMs);
		AccumStat(AudioStreamerStats.Fade, FadeMs);
		AccumStat(AudioStreamerStats.ProcessAudio, AudioMs);
		AccumStat(AudioStreamerStats.LipSync, LipSyncMs);
		AccumStat(AudioStreamerStats.FinishCheck, FinishCheckMs);

		if (AudioStreamerStats.SampleCount >= 60)
		{
			const int32 N = AudioStreamerStats.SampleCount;
			auto LogStat = [N](const FConvaiTimingStat& S) -> FString
			{
				return FString::Printf(TEXT("mean=%.3f min=%.3f max=%.3f"), S.Total / N, S.Min, S.Max);
			};
			CONVAI_LOG(ConvaiAudioStreamerLog, Log,
				TEXT("[AudioStreamer %d ticks] Tick(%s) | ProcessAudio(%s) | LipSync(%s) | FinishCheck(%s) | Fade(%s) | RingBuf=%u bytes"),
				N,
				*LogStat(AudioStreamerStats.Tick),
				*LogStat(AudioStreamerStats.ProcessAudio),
				*LogStat(AudioStreamerStats.LipSync),
				*LogStat(AudioStreamerStats.FinishCheck),
				*LogStat(AudioStreamerStats.Fade),
				AudioRingBuffer.GetAvailableBytes());
			AudioStreamerStats.Reset();
		}
	}
	else
	{
		AudioStreamerStats.Reset();
	}
#endif
}

void UConvaiAudioStreamer::BeginDestroy()
{
	Super::BeginDestroy();
}

void UConvaiAudioStreamer::StopLipSync()
{
	// Clear the lipsync audio buffer
	LipSyncAudioRingBuffer.Reset();

	if (ConvaiLipSync)
	{
		ConvaiLipSync->ConvaiStopLipSync();
	}
}

void UConvaiAudioStreamer::PauseLipSync() const
{
	if (ConvaiLipSync)
	{
		ConvaiLipSync->ConvaiPauseLipSync();
	}
}

void UConvaiAudioStreamer::ResumeLipSync() const
{
	if (ConvaiLipSync)
	{
		ConvaiLipSync->ConvaiResumeLipSync();
	}
}

void UConvaiAudioStreamer::MarkEndOfAudio()
{
	// Log total audio received from WebRTC
	if (TotalAudioBytesReceived > 0 && LastReceivedSampleRate > 0 && LastReceivedNumChannels > 0)
	{
		const uint32 BytesPerSample = 2; // 16-bit audio
		float TotalDurationSeconds = static_cast<float>(TotalAudioBytesReceived) /
			(LastReceivedSampleRate * LastReceivedNumChannels * BytesPerSample);
		CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("[AudioStreamer] MarkEndOfAudio - Total audio received from WebRTC: %u bytes, %.3f seconds (SR: %u, CH: %u)"),
			TotalAudioBytesReceived, TotalDurationSeconds, LastReceivedSampleRate, LastReceivedNumChannels);
	}

	// Reset tracking for next session
	TotalAudioBytesReceived = 0;
	LastReceivedSampleRate = 0;
	LastReceivedNumChannels = 0;
	TotalAudioFramesReceived = 0;

	// Notify lipsync component that no more audio is expected
	if (SupportsLipSync() && ConvaiLipSync)
	{
		ConvaiLipSync->MarkEndOfAudio();
	}
}

bool UConvaiAudioStreamer::CanUseLipSync()
{
	return false;
}

void UConvaiAudioStreamer::ForceRecalculateLipsyncStartTime()
{
	if (SupportsLipSync())
	{
		ConvaiLipSync->ForceRecalculateStartTime();
	}
}

bool UConvaiAudioStreamer::CanUseVision()
{
	return false;
}

void UConvaiAudioStreamer::OnFacialDataReadyCallback()
{
	OnFacialDataReadyDelegate.Broadcast();
}

void UConvaiAudioStreamer::OnLipSyncTimeOut()
{
}

TArray<float> UConvaiAudioStreamer::GetFacialData() const
{
	if (ConvaiLipSync)
	{
		return ConvaiLipSync->ConvaiGetFacialData();
	}
	return TArray<float>();
}

TArray<FString> UConvaiAudioStreamer::GetFacialDataNames() const
{
	if (ConvaiLipSync)
	{
		return ConvaiLipSync->ConvaiGetFacialDataNames();
	}
	return TArray<FString>();
}

TMap<FName, float> UConvaiAudioStreamer::ConvaiGetFaceBlendshapes() const
{
	if (ConvaiLipSync)
	{
		return ConvaiLipSync->ConvaiGetFaceBlendshapes();
	}
	return TMap<FName, float>();
}

EC_LipSyncMode UConvaiAudioStreamer::GetLipSyncMode()
{
	if (SupportsLipSync())
	{
		return ConvaiLipSync->GetLipSyncMode();
	}
	return EC_LipSyncMode::Off;
}

bool UConvaiAudioStreamer::GeneratesFacialDataAsBlendshapes()
{
	if (SupportsLipSync())
	{
		return ConvaiLipSync->GeneratesFacialDataAsBlendshapes();
	}
	return false;
}

double UConvaiAudioStreamer::GetRemainingContentDuration() const
{
    if (!IsTalking)
        return 0.0;

    // Remaining audio already queued to the sound wave but not yet played
    double RemainingInSoundWave = 0.0;
    if (AudioPlaybackSampleRate > 0 && AudioPlaybackNumChannels > 0 && TotalBytesQueuedToAudio > 0)
    {
        double TotalQueuedDuration = UConvaiUtils::CalculateAudioDuration(
            TotalBytesQueuedToAudio, AudioPlaybackNumChannels, AudioPlaybackSampleRate, 2);
        RemainingInSoundWave = FMath::Max(0.0, TotalQueuedDuration - GetAudioPlaybackTime());
    }

    // Remaining audio in the ring buffer (not yet queued to the sound wave)
    double RemainingInBuffer = 0.0;
    uint32 BufferedBytes = AudioRingBuffer.GetAvailableBytes();
    if (BufferedBytes > 0)
    {
        uint32 SR, NC;
        AudioRingBuffer.GetFormat(SR, NC);
        if (SR > 0 && NC > 0)
            RemainingInBuffer = UConvaiUtils::CalculateAudioDuration(BufferedBytes, NC, SR, 2);
    }

    return RemainingInSoundWave + RemainingInBuffer;
}

double UConvaiAudioStreamer::GetAudioPlaybackTime() const
{
    if (!IsValid(SoundWaveProcedural) || AudioPlaybackSampleRate == 0 || AudioPlaybackNumChannels == 0)
        return 0.0;

    // Method 1: Byte-consumption tracking
    // Bytes queued minus bytes still available = bytes actually played by the audio device
    int32 AvailableBytes = SoundWaveProcedural->GetAvailableAudioByteCount();
    int64 BytesConsumed = static_cast<int64>(TotalBytesQueuedToAudio) - AvailableBytes;
    double ByteBasedTime = 0.0;
    if (BytesConsumed > 0)
    {
        ByteBasedTime = static_cast<double>(BytesConsumed) /
            (static_cast<double>(AudioPlaybackSampleRate) * static_cast<double>(AudioPlaybackNumChannels) * 2.0);
    }

    // Method 2: Wall-clock time since playback started
    double WallClockTime = 0.0;
    if (AudioPlaybackStartTime > 0.0)
    {
        WallClockTime = FPlatformTime::Seconds() - AudioPlaybackStartTime;
    }

    // Return the lower of the two estimates.
    // Wall-clock can overshoot during buffer starvation (audio stalls but clock keeps ticking).
    // Byte-based can overshoot if bytes are consumed faster than real-time during catch-up.
    // Taking the minimum gives us the most conservative (accurate) playback position.
    if (ByteBasedTime <= 0.0)
        return FMath::Max(0.0, WallClockTime);
    if (WallClockTime <= 0.0)
        return FMath::Max(0.0, ByteBasedTime);

    return FMath::Min(ByteBasedTime, WallClockTime);
}

void UConvaiAudioStreamer::onAudioStarted()
{
	TWeakObjectPtr<UConvaiAudioStreamer> WeakSelf(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelf] {
		if (WeakSelf.IsValid())
		{
			WeakSelf->OnStartedTalkingDelegate.Broadcast();
		}
		});
	
}

void UConvaiAudioStreamer::onAudioFinished()
{
    CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("onAudioFinished"));

    bIsPlayingAudio = false;

    // Broadcast that audio has finished
    TWeakObjectPtr<UConvaiAudioStreamer> WeakSelf(this);
    AsyncTask(ENamedThreads::GameThread, [WeakSelf] {
        if (WeakSelf.IsValid())
        {
            WeakSelf->OnFinishedTalkingDelegate.Broadcast();
        }
    });
    IsTalking = false;

    // Default behavior: stop lipsync when audio finishes
    // This can be overridden in derived classes (e.g., ConvaiChatbotComponent)
    if (SupportsLipSync())
    {
        StopLipSync();
    }
}

void UConvaiAudioStreamer::StartRecordingIncomingAudio()
{
    FScopeLock Lock(&IncomingAudioRecordLock);

    if (bIsRecordingIncomingAudio)
    {
        CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("StartRecordingIncomingAudio: Already recording"));
        return;
    }

    IncomingAudioRecordBuffer.Empty();
    IncomingAudioRecordSampleRate = 0;
    IncomingAudioRecordNumChannels = 0;
    bIsRecordingIncomingAudio = true;

    CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("StartRecordingIncomingAudio: Recording started"));
}

USoundWave* UConvaiAudioStreamer::FinishRecordingIncomingAudio()
{
    FScopeLock Lock(&IncomingAudioRecordLock);

    if (!bIsRecordingIncomingAudio)
    {
        CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("FinishRecordingIncomingAudio: Not currently recording"));
        return nullptr;
    }

    bIsRecordingIncomingAudio = false;

    if (IncomingAudioRecordBuffer.Num() == 0)
    {
        CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("FinishRecordingIncomingAudio: No audio data recorded"));
        return nullptr;
    }

    float Duration = UConvaiUtils::CalculateAudioDuration(IncomingAudioRecordBuffer.Num(), IncomingAudioRecordNumChannels, IncomingAudioRecordSampleRate, 2);
    CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("FinishRecordingIncomingAudio: Recorded %d bytes, duration: %.2f seconds"), IncomingAudioRecordBuffer.Num(), Duration);

    USoundWave* SoundWave = UConvaiUtils::PCMDataToSoundWav(IncomingAudioRecordBuffer, IncomingAudioRecordNumChannels, IncomingAudioRecordSampleRate);
    IncomingAudioRecordBuffer.Empty();

    return SoundWave;
}

void UConvaiAudioStreamer::OnAudioReceivedForRecording(uint8* AudioData, uint32 AudioDataSize, uint32 SampleRate, uint32 NumChannels)
{
    FScopeLock Lock(&IncomingAudioRecordLock);

    if (!bIsRecordingIncomingAudio)
    {
        return;
    }

    IncomingAudioRecordBuffer.Append(AudioData, AudioDataSize);
    IncomingAudioRecordSampleRate = SampleRate;
    IncomingAudioRecordNumChannels = NumChannels;
}
