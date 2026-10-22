// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiAudioStreamer.h"
#include "Sound/SoundWave.h"
#include "Engine.h"
#include "Net/UnrealNetwork.h"
#include "Kismet/KismetSystemLibrary.h"
#include "LipSyncInterface.h"
#include "VisionInterface.h"
#include "Math/UnrealMathUtility.h"
#include "ConvaiStreamingVoiceWave.h"
#include "ConvaiUtils.h"
#include "Utility/ConvaiEngineCompat.h"
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
		{
			FScopeLock PowerLock(&TalkingPowerLock);
			TalkingPower.Reset();
		}
        // Execute on game thread (blocking if not already on game thread)
        if (IsInGameThread())
        {
            ConfigureVoiceWave(SampleRate, NumChannels);
        }
        else
        {
            // Use FGraphEventRef to make this blocking
            FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady(
                [this, SampleRate, NumChannels]()
                {
                    ConfigureVoiceWave(SampleRate, NumChannels);
                },
                TStatId(),
                nullptr,
                ENamedThreads::GameThread
            );

            // Wait for the task to complete
            FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task);
        }
    }

	if (AudioPlaybackStartTime == 0.0)
	{
		ResetPlaybackTimeTracking();
		AudioPlaybackStartTime = FPlatformTime::Seconds();
	}

    // Audio just (re-)started playing → broadcast OnStartedTalking so downstream
    // animation state tracks buffer state accurately, whether this is a fresh
    // session or stutter recovery within one.
    if (!IsTalking)
    {
        onAudioStarted();
        IsTalking = true;
    }

	// Analyze the PCM that actually reaches playback, after ingress buffering.
	// Metadata follows the same byte timeline as lip sync; no second PCM copy is retained.
	double PowerStartTime = 0.0;
	{
		FScopeLock Lock(&PlaybackTimeLock);
		if (SampleRate > 0 && NumChannels > 0)
			PowerStartTime = static_cast<double>(TotalBytesQueuedToAudio) / (static_cast<double>(SampleRate) * NumChannels * sizeof(int16));
	}
	{
		FScopeLock PowerLock(&TalkingPowerLock);
		TalkingPower.Enqueue(VoiceData, VoiceDataSize, SampleRate, NumChannels, PowerStartTime);
	}

    if (UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural))
    {
        VoiceWave->QueueVoice(VoiceData, VoiceDataSize);
    }
    else
    {
        SoundWaveProcedural->QueueAudio(VoiceData, VoiceDataSize);
    }

    // Track bytes queued for playback time calculation
    {
        FScopeLock Lock(&PlaybackTimeLock);
        TotalBytesQueuedToAudio += VoiceDataSize;
        AudioPlaybackSampleRate = SampleRate;
        AudioPlaybackNumChannels = NumChannels;
    }
}

void UConvaiAudioStreamer::ConfigureVoiceWave(uint32 SampleRate, uint32 NumChannels)
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
	if (UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural))
	{
		VoiceWave->AttachSource(this);
	}
	ForceRecalculateLipsyncStartTime();
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
	if (PCMData.Num() == 0)
	{
		// A failed extraction reports a zero format, which the ring buffer reads as unset, stalling audio already queued.
		return;
	}

	// Enqueue audio data to the ring buffer
	HandleAudioReceived(PCMData.GetData(), PCMData.Num(), false, SampleRate, NumChannels);

	// Force process and play the audio immediately
	ProcessIncomingAudio(true);
}

void UConvaiAudioStreamer::StopVoice()
{
	// The rings are locked, so a stop from any thread empties them at once. The tick never
	// resets them again: by then they may hold the next reply.
	auto DropQueued = [this]
	{
		AudioRingBuffer.Reset();
		LipSyncAudioRingBuffer.Reset();
		LipSyncBuffer.Reset();
	};
	// Locked against the render thread's pull and the tick's push, so it is complete on return from
	// any thread.
	if (UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural))
	{
		VoiceWave->Flush(DropQueued);
	}
	else
	{
		DropQueued();
	}

	if (!IsInGameThread())
	{
		PendingGameThreadWork |= PendingVoiceStop;
		return;
	}
	StopVoiceMedia();
}

void UConvaiAudioStreamer::StopVoiceMedia()
{
	if (IsTalking)
		onAudioFinished();
	// A stop ends playback even when talking was already false (PauseVoice), or the next reply
	// would skip its start cushion.
	bIsPlayingAudio = false;

	FlushVoiceWave();

	// Reset playback time tracking (bytes queued, start time, clamp state).
	ResetPlaybackTimeTracking();

	ResetVoiceFade();

	if (ConvaiLipSync)
	{
		ConvaiLipSync->ConvaiStopLipSync();
	}
}

void UConvaiAudioStreamer::ApplyPendingGameThreadWork()
{
	const uint8 Playback = PendingLipSyncPlayback.exchange(0);
	if (Playback == PendingLipSyncPause)
	{
		PauseLipSync();
	}
	else if (Playback == PendingLipSyncResume)
	{
		ResumeLipSync();
	}
	const uint8 Work = PendingGameThreadWork.exchange(0);
	// The order a transport callback makes them in: bot-llm-started marks the end, then stops.
	if ((Work & PendingEndOfAudio) && ConvaiLipSync)
	{
		ConvaiLipSync->MarkEndOfAudio();
	}
	if (Work & PendingVoiceStop)
	{
		StopVoiceMedia();
	}
	else if ((Work & PendingLipSyncStop) && ConvaiLipSync)
	{
		ConvaiLipSync->ConvaiStopLipSync();
	}
}

void UConvaiAudioStreamer::FlushVoiceWave()
{
	// StopVoice already flushed a streaming wave, on whichever thread it ran.
	if (SoundWaveProcedural && !SoundWaveProcedural->IsA<UConvaiStreamingVoiceWave>())
	{
		SoundWaveProcedural->ResetAudio();
	}
}

void UConvaiAudioStreamer::PauseVoice()
{
	if (bIsPaused)
		return;

	SetPaused(true);
	IsTalking = false;
	FScopeLock PowerLock(&TalkingPowerLock);
	TalkingPower.Silence();
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

	// The fade fields belong to the tick, which divides by TotalVoiceFadeOutTime.
	if (!IsInGameThread())
	{
		if (InVoiceFadeOutDuration <= 0)
		{
			StopVoice();
			return;
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<UConvaiAudioStreamer>(this), InVoiceFadeOutDuration]
		{
			if (UConvaiAudioStreamer* Streamer = WeakThis.Get())
			{
				Streamer->StopVoiceWithFade(InVoiceFadeOutDuration);
			}
		});
		return;
	}

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
#if WITH_TESTS
	++LipSyncLookupsForTests;
#endif
	// The owner's component set changes on the game thread, so only that thread may walk it.
	if (!IsInGameThread() || !GetOwner())
	{
		return ConvaiLipSync;
	}
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

	// A Blueprint-only implementation of the interface has no native pointer to cast to.
	IConvaiLipSyncInterface* LipSync = Cast<IConvaiLipSyncInterface>(LipSyncComponent);
	if (LipSyncComponent && !LipSync)
	{
		CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("SetLipSyncComponent: %s does not implement IConvaiLipSyncInterface in C++"), *LipSyncComponent->GetName());
	}
	if (LipSync)
	{
		LipSync->OnFacialDataReady.BindUObject(this, &UConvaiAudioStreamer::OnFacialDataReadyCallback);

		// Set up audio playback time provider for audio-synced lipsync
		LipSync->SetAudioPlaybackTimeProvider(FGetAudioPlaybackTimeDelegate::CreateUObject(this, &UConvaiAudioStreamer::GetAudioPlaybackTime));
	}
	BindLipSync(LipSync);
	return LipSync != nullptr;
}

bool UConvaiAudioStreamer::SupportsLipSync()
{
	if (!CanUseLipSync())
		return false;

	if (!IsInGameThread())
	{
		return bLipSyncBound;
	}
	RefreshBindings();
	if (ConvaiLipSync == nullptr)
	{
		FindFirstLipSyncComponent();
	}
	return ConvaiLipSync != nullptr;
}

IConvaiVisionInterface* UConvaiAudioStreamer::FindFirstVisionComponent()
{
	if (!IsInGameThread() || !GetOwner())
	{
		return ConvaiVision;
	}
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

	IConvaiVisionInterface* Vision = Cast<IConvaiVisionInterface>(VisionComponent);
	if (VisionComponent && !Vision)
	{
		CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("SetVisionComponent: %s does not implement IConvaiVisionInterface in C++"), *VisionComponent->GetName());
	}
	BindVision(Vision);
	return Vision != nullptr;
}

bool UConvaiAudioStreamer::SupportsVision()
{
	if (!CanUseVision())
		return false;

	if (!IsInGameThread())
	{
		return ConvaiVision != nullptr;
	}
	RefreshBindings();
	if (ConvaiVision == nullptr)
	{
		FindFirstVisionComponent();
	}
	return ConvaiVision != nullptr;
}

void UConvaiAudioStreamer::BindLipSync(IConvaiLipSyncInterface* LipSync)
{
	// Cleared first, so the workers stop reading a plain provider the moment it is replaced.
	PlainLipSync = nullptr;
	ConvaiLipSync = LipSync;
	BoundLipSync = LipSync;
	LipSyncObject = Cast<UObject>(LipSync);
	PlainLipSync = LipSyncObject.IsExplicitlyNull() ? LipSync : nullptr;
	PublishLipSyncIngress();
}

void UConvaiAudioStreamer::BindVision(IConvaiVisionInterface* Vision)
{
	ConvaiVision = Vision;
	BoundVision = Vision;
	VisionObject = Cast<UObject>(Vision);
}

void UConvaiAudioStreamer::RefreshBindings()
{
	// DestroyComponent marks the component garbage at once, long before garbage
	// collection frees it, so the weak pointer goes stale while the memory is still valid.
	// A provider that is no UObject leaves its weak pointer null, and its caller owns its lifetime.
	auto IsDestroyed = [](const TWeakObjectPtr<UObject>& Object) { return !Object.IsExplicitlyNull() && !Object.IsValid(); };
	if (ConvaiLipSync != BoundLipSync)
	{
		BindLipSync(ConvaiLipSync);
	}
	else if (ConvaiLipSync && IsDestroyed(LipSyncObject))
	{
		BindLipSync(nullptr);
	}
	if (ConvaiVision != BoundVision)
	{
		BindVision(ConvaiVision);
	}
	else if (ConvaiVision && IsDestroyed(VisionObject))
	{
		BindVision(nullptr);
	}
	PublishLipSyncIngress();
}

void UConvaiAudioStreamer::PublishLipSyncIngress()
{
	const bool bBound = ConvaiLipSync != nullptr && CanUseLipSync();
	bLipSyncBound = bBound;
	bLipSyncInfersFromAudio = bBound && !ConvaiLipSync->RequiresPrecomputedFaceData();
}

IConvaiLipSyncInterface* UConvaiAudioStreamer::GetLiveLipSync() const
{
	// On the game thread a caller may have cleared ConvaiLipSync since the last refresh.
	IConvaiLipSyncInterface* Plain = PlainLipSync.load();
	if (Plain && (!IsInGameThread() || Plain == ConvaiLipSync))
	{
		return Plain;
	}
	return Cast<IConvaiLipSyncInterface>(LipSyncObject.Get());
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

	CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("MinBufferDuration = %f"), MinBufferDuration);

	// Initialize the audio component
	bAutoActivate = true;
	bAlwaysPlay = true;

	SoundWaveProcedural = NewObject<UConvaiStreamingVoiceWave>();

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
	if (bLipSyncInfersFromAudio)
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

    // A stop from another thread ends the wave's reply before the tick clears bIsPlayingAudio; the
    // next reply still waits for its cushion.
    const UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural);
    const bool bReplyPlaying = bIsPlayingAudio && (!VoiceWave || VoiceWave->IsReplyInProgress());

    // Check if we should wait for more buffering
    if (!bReplyPlaying && MinBufferDuration > 0.0f)
    {
        // Calculate how much audio duration we have buffered
        double BufferedDuration = UConvaiUtils::CalculateAudioDuration(AvailableBytes, OutNumChannels, OutSampleRate, 2);

        if (BufferedDuration < MinBufferDuration && !Force)
        {
            // Not enough data buffered yet, wait for more
            return 0;
        }
    }

    // Per-tick dequeue cap in bytes: MaxChunkDurationSec of audio at the current format.
    constexpr uint32 BytesPerSample = 2; // 16-bit PCM
    const uint32 ChunkSize = static_cast<uint32>(MaxChunkDurationSec * OutSampleRate) * OutNumChannels * BytesPerSample;

    // Ensure buffer has capacity (only allocates when growing, keeps slack for reuse)
    if (OutAudioData.Num() < (int32)ChunkSize)
    {
        OutAudioData.SetNumUninitialized(ChunkSize);
    }

    // Dequeue up to ChunkSize bytes - we return the actual byte count
    uint32 BytesRead = AudioRingBuffer.Dequeue(OutAudioData.GetData(), ChunkSize);

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

	// Per-tick dequeue cap in bytes: MaxChunkDurationSec of audio at the current format.
	constexpr uint32 BytesPerSample = 2; // 16-bit PCM
	const uint32 ChunkSize = static_cast<uint32>(MaxChunkDurationSec * SampleRate) * NumChannels * BytesPerSample;

	// Ensure buffer has capacity (only allocates when growing, keeps slack for reuse)
	if (LipSyncAudioChunkBuffer.Num() < (int32)ChunkSize)
	{
		LipSyncAudioChunkBuffer.SetNumUninitialized(ChunkSize);
	}

	// Dequeue audio immediately without buffering constraints (no MinBufferDuration check)
	uint32 BytesRead = LipSyncAudioRingBuffer.Dequeue(LipSyncAudioChunkBuffer.GetData(), ChunkSize);

	if (BytesRead > 0)
	{
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

uint32 UConvaiAudioStreamer::GetVoiceFlushGeneration() const
{
	const UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural);
	return VoiceWave ? VoiceWave->GetFlushGeneration() : 0;
}

// Process incoming audio data (called from game thread - heavy logic)
void UConvaiAudioStreamer::ProcessIncomingAudio(bool Force)
{
	ProcessIncomingAudioSince(Force, GetVoiceFlushGeneration());
}

void UConvaiAudioStreamer::ProcessIncomingAudioSince(bool Force, uint32 ForceGeneration)
{
	// Once a device renders the streaming wave, the render thread pulls the ring itself, so a
	// game-thread hitch cannot starve the voice and no per-tick cap applies.
	UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural);
	if (VoiceWave && VoiceWave->IsRendering())
	{
		TickVoicePull(Force, ForceGeneration);
		return;
	}

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

    // Taken and queued as one step against a stop's flush: a chunk taken from the ring is either
    // queued before the flush, which drops it, or not taken at all.
    uint32 DequeuedBytes = 0;
    auto TakeAndQueue = [&]
    {
        // A force read before a stop belonged to the reply it cut.
        const bool bForceNow = Force && (!VoiceWave || VoiceWave->GetFlushGeneration() == ForceGeneration);
        DequeuedBytes = TryDequeueAudioChunk(AudioChunkBuffer, SampleRate, NumChannels, bForceNow);
        if (DequeuedBytes > 0)
        {
            // Play the audio chunk (use DequeuedBytes, not AudioChunkBuffer.Num() which has slack)
            PlayVoiceData(AudioChunkBuffer.GetData(), DequeuedBytes, false, SampleRate, NumChannels);
        }
    };
    if (VoiceWave)
    {
        VoiceWave->HoldFlush(TakeAndQueue);
    }
    else
    {
        TakeAndQueue();
    }
    if (DequeuedBytes == 0)
    {
        return;
    }

	// Mark as playing and resume lipsync on first chunk
	if (!bIsPlayingAudio)
	{
		bIsPlayingAudio = true;

		// Resume lipsync when starting to play
		if (ConvaiLipSync)
		{
			CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("[AudioStreamer] ProcessIncomingAudio - Starting Lipsync"));
			ForceRecalculateLipsyncStartTime();
			ResumeLipSync();
		}
	}

	// A device may have started rendering the wave meanwhile; the pull continues the reply.
	if (VoiceWave && VoiceWave->IsRendering())
	{
		VoiceWave->ContinuePushedReply();
	}
}

void UConvaiAudioStreamer::TickVoicePull(bool bForce, uint32 ForceGeneration)
{
	UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural);
	VoiceWave->AttachSource(this);

	uint32 SampleRate = 0;
	uint32 NumChannels = 0;
	AudioRingBuffer.GetFormat(SampleRate, NumChannels);
	if (SampleRate > 0 && NumChannels > 0)
	{
		if (static_cast<uint32>(VoiceWave->GetSampleRateForCurrentPlatform()) != SampleRate || static_cast<uint32>(VoiceWave->NumChannels) != NumChannels)
		{
			{
				FScopeLock PowerLock(&TalkingPowerLock);
				TalkingPower.Reset();
			}
			AnalyzedVoiceBytes = 0;
			ConfigureVoiceWave(SampleRate, NumChannels);
		}
		FScopeLock Lock(&PlaybackTimeLock);
		AudioPlaybackSampleRate = SampleRate;
		AudioPlaybackNumChannels = NumChannels;
	}

	// A reply the tick began pushing before the device rendered the wave goes on through the pull,
	// without waiting for the start cushion again. bIsPlayingAudio can be stale here, after a stop
	// from another thread, so the wave decides.
	if (bIsPlayingAudio)
	{
		VoiceWave->ContinuePushedReply();
	}

	// A force with nothing waiting would latch and open a later reply below its cushion.
	if (bForce && !VoiceWave->IsReplyOpen() && !AudioRingBuffer.IsEmpty())
	{
		VoiceWave->RequestForcedStart(ForceGeneration);
	}

	// A flush restarts the pulled count from any thread; a new count is compared with nothing.
	const uint32 Generation = VoiceWave->GetFlushGeneration();
	if (Generation != PulledGenerationAtLastTick)
	{
		PulledGenerationAtLastTick = Generation;
		PulledAtLastTick = 0;
	}
	// The render thread opened a reply: talking starts where its first bytes were pulled. A paused
	// source pulls nothing, so PauseVoice holds talking until the pulls resume.
	const uint64 Pulled = VoiceWave->GetPulledBytes();
	const bool bPulledSinceLastTick = Pulled > 0 && Pulled != PulledAtLastTick;
	PulledAtLastTick = Pulled;
	if (!IsTalking && bPulledSinceLastTick && VoiceWave->IsReplyOpen())
	{
		if (AudioPlaybackStartTime == 0.0)
		{
			// The rest of the timeline was reset by the stop; TalkingPower already holds this
			// reply's analysis.
			FScopeLock Lock(&PlaybackTimeLock);
			LastReportedPlaybackTime = 0.0;
			LastReportedWallClock = 0.0;
			AudioPlaybackStartTime = VoiceWave->GetReplyOpenSeconds();
		}
		onAudioStarted();
		IsTalking = true;
		if (!bIsPlayingAudio)
		{
			bIsPlayingAudio = true;
			if (ConvaiLipSync)
			{
				ForceRecalculateLipsyncStartTime();
				ResumeLipSync();
			}
		}
	}

	AnalyzePulledVoice();
}

void UConvaiAudioStreamer::AnalyzePulledVoice()
{
	uint32 SampleRate = 0;
	uint32 NumChannels = 0;
	AudioRingBuffer.GetFormat(SampleRate, NumChannels);
	if (SampleRate == 0 || NumChannels == 0)
	{
		return;
	}
	// TalkingPower keeps 2.56 s of windows; a second ahead of playback leaves room for a hitch.
	const uint64 BytesPerSecond = static_cast<uint64>(SampleRate) * NumChannels * sizeof(int16);
	const uint64 Lookahead = BytesPerSecond;

	// With pulls held, the ring's oldest byte sits at a fixed playback position. They are held for
	// the copy only: the render thread waits for this lock, so the analysis runs after it.
	uint64 Ahead = 0;
	uint32 Peeked = 0;
	Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural)->HoldPulls([&](uint64 PulledSoFar)
	{
		const uint64 Dequeued = TotalBytesQueuedToAudio + PulledSoFar;
		// Bytes already pulled are already played; analysing them would only be discarded.
		AnalyzedVoiceBytes = FMath::Max(AnalyzedVoiceBytes, Dequeued);
		Ahead = AnalyzedVoiceBytes - Dequeued;
		const uint64 Buffered = AudioRingBuffer.GetAvailableBytes();
		if (Buffered <= Ahead || Ahead >= Lookahead)
		{
			return;
		}
		// ponytail: re-copies the up-to-1 s already analysed (two memcpys); a peek at an offset
		// needs one on FAudioRingBuffer.
		const uint32 ToAnalyze = static_cast<uint32>(FMath::Min(Buffered - Ahead, Lookahead - Ahead));
		AnalysisBuffer.SetNumUninitialized(static_cast<int32>(Ahead + ToAnalyze), CONVAI_ALLOW_SHRINKING_NO);
		Peeked = AudioRingBuffer.GetData(AnalysisBuffer.GetData(), static_cast<uint32>(Ahead + ToAnalyze));
	});
	if (Peeked <= Ahead)
	{
		return;
	}
	FScopeLock PowerLock(&TalkingPowerLock);
	TalkingPower.Enqueue(AnalysisBuffer.GetData() + Ahead, Peeked - static_cast<uint32>(Ahead), SampleRate, NumChannels,
		static_cast<double>(AnalyzedVoiceBytes) / BytesPerSecond);
	AnalyzedVoiceBytes += Peeked - Ahead;
}

uint64 UConvaiAudioStreamer::GetPulledVoiceBytes() const
{
	const UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural);
	return VoiceWave ? VoiceWave->GetPulledBytes() : 0;
}

// Force play any buffered audio
void UConvaiAudioStreamer::ForcePlayBufferedAudio()
{
	ProcessIncomingAudio(true);
}

// Handle received lipsync data (called from transport thread - lightweight)
void UConvaiAudioStreamer::HandleLipSyncReceived(FAnimationSequence& FaceSequence)
{
	if (bLipSyncBound)
		LipSyncBuffer.Enqueue(FaceSequence);
}

namespace
{
	// FLipSyncBuffer::Enqueue, appending by move. A moving overload on the buffer would be public C++.
	void EnqueueLipSyncByMove(FLipSyncBuffer& Buffer, FAnimationSequence&& InSequence)
	{
		FScopeLock Lock(&Buffer.SequenceMutex);
		Buffer.Sequence.AnimationFrames.Append(MoveTemp(InSequence.AnimationFrames));
		Buffer.Sequence.Duration += InSequence.Duration;
		if (Buffer.FrameRate == 0 && InSequence.FrameRate > 0)
		{
			Buffer.FrameRate = InSequence.FrameRate;
			Buffer.Sequence.FrameRate = InSequence.FrameRate;
		}
		Buffer.bHasNewData = true;
	}
}

void UConvaiAudioStreamer::HandleLipSyncReceivedByMove(FAnimationSequence&& FaceSequence)
{
	if (bLipSyncBound)
	{
		EnqueueLipSyncByMove(LipSyncBuffer, MoveTemp(FaceSequence));
	}
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
            ConvaiLipSync->ConvaiApplyPrecomputedFacialAnimation(nullptr, 0, 0, 0, MoveTemp(Sequence));
        }
    }
}



void UConvaiAudioStreamer::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	RefreshBindings();
	// A character without a face would otherwise walk its components every frame.
	if (!ConvaiLipSync && CanUseLipSync())
	{
		const double Now = FPlatformTime::Seconds();
		if (Now >= NextLipSyncLookupSeconds)
		{
			NextLipSyncLookupSeconds = Now + 1.0;
			FindFirstLipSyncComponent();
		}
	}
	ApplyPendingGameThreadWork();

#if ConvaiDebugMode
	const double TickStart = FPlatformTime::Seconds();
	const double FadeStart = FPlatformTime::Seconds();
#endif

	UpdateVoiceFade(DeltaTime);

#if ConvaiDebugMode
	const double FadeMs = (FPlatformTime::Seconds() - FadeStart) * 1000.0;
	const double AudioStart = FPlatformTime::Seconds();
#endif

	// Process incoming data from transport thread.
	// If the audio thread signalled a force-flush (grace window expired with
	// sub-MinBufferDuration data still in the ring buffer), pass Force=true so
	// the remainder drains into SoundWaveProcedural and onAudioFinished fires.
	// The generation is read first: a stop clears the request before it flushes, so a request read
	// here whose reply a stop then flushes shows as a newer generation.
	const uint32 ForceGeneration = GetVoiceFlushGeneration();
	ProcessIncomingAudioSince(bForceFlushPending.Exchange(false), ForceGeneration);
	UpdateTalkingPower(DeltaTime);

#if ConvaiDebugMode
	const double AudioMs = (FPlatformTime::Seconds() - AudioStart) * 1000.0;
	const double LipSyncStart = FPlatformTime::Seconds();
#endif

	// Process lipsync based on component type
	if (ConvaiLipSync)
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
			// A pulled reply ends only if no audio arrived since the check, so the next one
			// starts behind the cushion instead of mid-pull. A pushed one ends regardless.
			UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural);
			if (!VoiceWave || VoiceWave->CloseReplyIfDrained() || !VoiceWave->IsRendering())
			{
				onAudioFinished();
			}
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
	// The render thread may still pull through the wave; it must not reach this object once freed.
	if (UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural))
	{
		VoiceWave->DetachSource();
	}
	Super::BeginDestroy();
}

void UConvaiAudioStreamer::StopLipSync()
{
	// Clear the lipsync audio buffer
	LipSyncAudioRingBuffer.Reset();
	// Also retire precomputed sequences that have not reached the component yet.
	// Otherwise an explicit stop can leak queued face data into the next turn.
	LipSyncBuffer.Reset();

	if (!IsInGameThread())
	{
		PendingGameThreadWork |= PendingLipSyncStop;
		return;
	}
	if (ConvaiLipSync)
	{
		ConvaiLipSync->ConvaiStopLipSync();
	}
}

void UConvaiAudioStreamer::PauseLipSync() const
{
	if (!IsInGameThread())
	{
		PendingLipSyncPlayback = PendingLipSyncPause;
		return;
	}
	if (ConvaiLipSync)
	{
		ConvaiLipSync->ConvaiPauseLipSync();
	}
}

void UConvaiAudioStreamer::ResumeLipSync() const
{
	if (!IsInGameThread())
	{
		PendingLipSyncPlayback = PendingLipSyncResume;
		return;
	}
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
	if (!IsInGameThread())
	{
		PendingGameThreadWork |= PendingEndOfAudio;
		return;
	}
	if (SupportsLipSync())
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
	if (ConvaiLipSync)
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
	if (const IConvaiLipSyncInterface* LipSync = GetLiveLipSync())
	{
		return LipSync->ConvaiGetFacialData();
	}
	return TArray<float>();
}

TArray<FString> UConvaiAudioStreamer::GetFacialDataNames() const
{
	if (const IConvaiLipSyncInterface* LipSync = GetLiveLipSync())
	{
		return LipSync->ConvaiGetFacialDataNames();
	}
	return TArray<FString>();
}

// Called on the animation worker too (AnimNode_ConvaiFaceSync).
TMap<FName, float> UConvaiAudioStreamer::ConvaiGetFaceBlendshapes() const
{
	if (const IConvaiLipSyncInterface* LipSync = GetLiveLipSync())
	{
		return LipSync->ConvaiGetFaceBlendshapes();
	}
	return TMap<FName, float>();
}

// Called on the animation worker too (AnimNode_ConvaiFaceSync).
bool UConvaiAudioStreamer::HasPlayableFaceFrames() const
{
	if (!IsTalking)
	{
		return false;
	}

	if (const IConvaiLipSyncInterface* LipSync = GetLiveLipSync())
	{
		return LipSync->HasPlayableFrames();
	}
	return false;
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

    // Remaining audio already handed to the sound wave (queued, or pulled by the render thread)
    // but not yet played
    double RemainingInSoundWave = 0.0;
    const uint64 BytesToMixer = TotalBytesQueuedToAudio + GetPulledVoiceBytes();
    if (AudioPlaybackSampleRate > 0 && AudioPlaybackNumChannels > 0 && BytesToMixer > 0)
    {
        double TotalQueuedDuration = UConvaiUtils::CalculateAudioDuration(
            BytesToMixer, AudioPlaybackNumChannels, AudioPlaybackSampleRate, 2);
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

    FScopeLock Lock(&PlaybackTimeLock);

    // Byte-consumption tracking: bytes queued minus bytes still queued but
    // unplayed = bytes the mixer has pulled from SoundWaveProcedural, plus
    // what the render thread pulled from the ring itself.
    const int32 AvailableBytes = SoundWaveProcedural->GetAvailableAudioByteCount();
    const int64 BytesConsumed  = static_cast<int64>(TotalBytesQueuedToAudio) - AvailableBytes
        + static_cast<int64>(GetPulledVoiceBytes());
    // Nothing consumes a streaming wave no device renders, so the bytes it consumed before would
    // hold the clock still; the wall clock moves it, as for a wave that never rendered.
    const UConvaiStreamingVoiceWave* VoiceWave = Cast<UConvaiStreamingVoiceWave>(SoundWaveProcedural);
    double ByteBasedTime = 0.0;
    if (BytesConsumed > 0 && (!VoiceWave || VoiceWave->IsRendering()))
    {
        ByteBasedTime = static_cast<double>(BytesConsumed) /
            (static_cast<double>(AudioPlaybackSampleRate) * static_cast<double>(AudioPlaybackNumChannels) * 2.0);
    }

    // Wall-clock time since playback started.
    double WallClockTime = 0.0;
    if (AudioPlaybackStartTime > 0.0)
    {
        WallClockTime = FPlatformTime::Seconds() - AudioPlaybackStartTime;
    }

    // Take the lower of the two estimates (conservative).
    double Candidate = 0.0;
    if (ByteBasedTime <= 0.0)
        Candidate = FMath::Max(0.0, WallClockTime);
    else if (WallClockTime <= 0.0)
        Candidate = FMath::Max(0.0, ByteBasedTime);
    else
        Candidate = FMath::Min(ByteBasedTime, WallClockTime);

    // Forward-rate cap: never advance faster than wall-clock since the last
    // read. A bursty sink (e.g. Pixel Streaming's WebRTC encoder draining
    // SoundWaveProcedural in a catch-up burst after a hitch) would otherwise
    // make ByteBasedTime spike by hundreds of ms in a single frame and cause
    // FaceSync to skip ahead of the rendered video frames.
    const double Now = FPlatformTime::Seconds();
    if (LastReportedWallClock > 0.0)
    {
        const double WallDelta  = Now - LastReportedWallClock;
        const double CapEpsilon = 0.002; // 2 ms slack for jitter
        Candidate = FMath::Min(Candidate, LastReportedPlaybackTime + WallDelta + CapEpsilon);
    }

    // Monotonic clamp: never go backwards. The FaceSync frame selector uses
    // FloorToInt(t / FrameDuration) and would regress the frame index on a
    // backward jump.
    double Result = FMath::Max(Candidate, LastReportedPlaybackTime);
    Result = FMath::Max(0.0, Result);

    LastReportedPlaybackTime = Result;
    LastReportedWallClock = Now;
    return Result;
}

void UConvaiAudioStreamer::ResetPlaybackTimeTracking()
{
	{
		FScopeLock Lock(&PlaybackTimeLock);
		TotalBytesQueuedToAudio = 0;
		AudioPlaybackStartTime = 0.0;
		LastReportedPlaybackTime = 0.0;
		LastReportedWallClock = 0.0;
	}
	AnalyzedVoiceBytes = 0;
	FScopeLock PowerLock(&TalkingPowerLock);
	TalkingPower.Reset();
	TalkingPowerLastPlaybackTime = 0.0;
	TalkingPowerStallTime = 0.0f;
}

float UConvaiAudioStreamer::GetTalkingPower() const
{
	FScopeLock PowerLock(&TalkingPowerLock);
	return TalkingPower.GetPower();
}

void UConvaiAudioStreamer::UpdateTalkingPower(float DeltaTime)
{
	if (bIsPaused || !IsValid(SoundWaveProcedural))
	{
		FScopeLock PowerLock(&TalkingPowerLock);
		TalkingPower.Silence();
		return;
	}
	// The playback clock has a wall-clock fallback before the first mixer pull.
	// Keep cosmetics silent until the mixer has actually consumed some PCM.
	bool bHasConsumedAudio = false;
	{
		FScopeLock Lock(&PlaybackTimeLock);
		bHasConsumedAudio = GetPulledVoiceBytes() > 0
			|| TotalBytesQueuedToAudio > static_cast<uint64>(FMath::Max(0, SoundWaveProcedural->GetAvailableAudioByteCount()));
	}
	if (!bHasConsumedAudio)
	{
		FScopeLock PowerLock(&TalkingPowerLock);
		TalkingPower.Silence();
		return;
	}
	const double PlaybackTime = GetAudioPlaybackTime();
	FScopeLock PowerLock(&TalkingPowerLock);
	TalkingPowerStallTime = PlaybackTime > TalkingPowerLastPlaybackTime + 0.000001
		? 0.0f : TalkingPowerStallTime + FMath::Max(0.0f, DeltaTime);
	TalkingPowerLastPlaybackTime = PlaybackTime;
	// A suspended audio device can stop inside a voiced window. Release cosmetics
	// after 100 ms without progress, retaining queued metadata for recovery.
	TalkingPower.Advance(PlaybackTime, DeltaTime, TalkingPowerStallTime <= 0.1f);
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
    ensure(IsInGameThread());
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
    if (ConvaiLipSync)
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
