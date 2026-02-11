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
	, BytesSentToLipSync(0)
	, TotalAudioBytesReceived(0)
	, LastReceivedSampleRate(0)
	, LastReceivedNumChannels(0)
	, TotalAudioFramesReceived(0)
	, TotalBytesQueuedToAudio(0)
	, AudioPlaybackSampleRate(0)
	, AudioPlaybackNumChannels(0)
{
	PrimaryComponentTick.bCanEverTick = true;
	bAutoActivate = true;
}

namespace
{

	double CalculateRemainingAudioDuration(TWeakObjectPtr<UConvaiAudioStreamer> WeakSelf)
	{
		if (!WeakSelf.IsValid())
		{
			return 0;
		}

		double CurrentTime = FPlatformTime::Seconds();

		double RemainingAudioDuration = 0.0;
		if (WeakSelf->AudioEndTime > 0.0)
		{
			RemainingAudioDuration = WeakSelf->AudioEndTime - CurrentTime;
			if (RemainingAudioDuration < 0.0)
				RemainingAudioDuration = 0.0;
		}

		return RemainingAudioDuration;
	}

	void HandleAudioTimer(TWeakObjectPtr<UConvaiAudioStreamer> WeakSelf, int32 PCM_DataSize, int32 SampleRate, int32 NumChannels)
	{
		if (!WeakSelf.IsValid() || !IsValid(WeakSelf->GetWorld()))
		{
			CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("PlayVoiceData: Object or World is Invalid!"));
			return;
		}

		double NewAudioDuration = UConvaiUtils::CalculateAudioDuration(PCM_DataSize, NumChannels, SampleRate, 2);

		
		double CurrentTime = FPlatformTime::Seconds();
		
		double RemainingAudioDuration = 0.0;
		if (WeakSelf->AudioEndTime > 0.0)
		{
			RemainingAudioDuration = WeakSelf->AudioEndTime - CurrentTime;
			if (RemainingAudioDuration < 0.0)
				RemainingAudioDuration = 0.0;
		}

		
		double TotalAudioDuration = RemainingAudioDuration + NewAudioDuration;
		
		WeakSelf->AudioEndTime = CurrentTime + TotalAudioDuration;

		if (WeakSelf.IsValid() && IsValid(WeakSelf->GetWorld()))
		{
			WeakSelf->GetWorld()->GetTimerManager().SetTimer(
				WeakSelf->AudioFinishedTimerHandle, 
				WeakSelf.Get(), 
				&UConvaiAudioStreamer::onAudioFinished, 
				TotalAudioDuration, 
				false);
		}
		else
		{
			CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("PlayVoiceData: Object or World became invalid before setting timer!"));
		}
	}

	void SetupAndPlayAudio(TWeakObjectPtr<UConvaiAudioStreamer> WeakThis, TArray<uint8> AudioDataCopy, int32 SampleRate, int32 NumChannels)
	{
		if (!WeakThis.IsValid() || !IsValid(WeakThis->SoundWaveProcedural))
		{
			return;
		}

		WeakThis->SetSound(WeakThis->SoundWaveProcedural);
		WeakThis->SoundWaveProcedural->QueueAudio(AudioDataCopy.GetData(), AudioDataCopy.Num());

		// Track bytes queued for playback time calculation
		WeakThis->TotalBytesQueuedToAudio += AudioDataCopy.Num();
		WeakThis->AudioPlaybackSampleRate = SampleRate;
		WeakThis->AudioPlaybackNumChannels = NumChannels;

		WeakThis->Play();
		WeakThis->ForceRecalculateLipsyncStartTime();
	}
};

void UConvaiAudioStreamer::ProcessPendingAudio()
{
	if (!IsValid(SoundWaveProcedural) && PendingAudioBuffer.Num() <= 0)
		return;

    // Process the buffer if there's any data
    SoundWaveProcedural->QueueAudio(PendingAudioBuffer.GetData(), PendingAudioBuffer.Num());

    // Track bytes queued for playback time calculation
    TotalBytesQueuedToAudio += PendingAudioBuffer.Num();

	// Lipsync component process the audio data to generate the lipsync
	if (SupportsLipSync() && !(ConvaiLipSync->RequiresPrecomputedFaceData()))
	{
		uint32 SampleRate = SoundWaveProcedural->GetSampleRateForCurrentPlatform();
		uint32 NumChannels = SoundWaveProcedural->NumChannels;
		ConvaiLipSync->ConvaiInferFacialDataFromAudio(PendingAudioBuffer.GetData(), PendingAudioBuffer.Num(), SampleRate>0? SampleRate : 48000, NumChannels > 0? NumChannels : 1);
	}
	
    PendingAudioBuffer.Empty();
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

    TWeakObjectPtr<UConvaiAudioStreamer> WeakSelf = this;

    if (IsInGameThread())
    {
        HandleAudioTimer(WeakSelf, VoiceDataSize, SampleRate, NumChannels);
    }
    else
    {
        AsyncTask(ENamedThreads::GameThread, [WeakSelf, VoiceDataSize, SampleRate, NumChannels]()
        {
            HandleAudioTimer(WeakSelf, VoiceDataSize, SampleRate, NumChannels);
        });
    }
    
    if (!IsValid(SoundWaveProcedural))
        return;

    // If configuring audio then queue the audio and return
    if (IsAudioConfiguring)
    {
        // Lock is already held, queue this audio for later processing
        if (ContainsHeaderData)
        {
            // Skip header for the queue
            PendingAudioBuffer.Append(VoiceData + 44, VoiceDataSize - 44);
        }
        else
        {
            PendingAudioBuffer.Append(VoiceData, VoiceDataSize);
        }
        
        // Try the lock again before exiting - if it's available now, process the queue
        if (!IsAudioConfiguring)
        {
			IsAudioConfiguring.AtomicSet(true);
            ProcessPendingAudio();
			IsAudioConfiguring.AtomicSet(false);
        }
        
        return;
    }
    
    // We have the lock, proceed with processing
    
    // Check that SoundWaveProcedural is valid and able to play input sample rate and channels
    if (SoundWaveProcedural->GetSampleRateForCurrentPlatform() != SampleRate || SoundWaveProcedural->NumChannels != NumChannels)
    {
		IsAudioConfiguring.AtomicSet(true);

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

        // Create a copy of the audio data for thread safety
        TArray<uint8> AudioDataCopy;
		AudioDataCopy.Append(VoiceData, VoiceDataSize);
        
        // Store a copy of the weak pointer for the lambda
        TWeakObjectPtr<UConvaiAudioStreamer> WeakThis = this;
        

        if (IsInGameThread())
		{
            SetupAndPlayAudio(WeakThis, AudioDataCopy, SampleRate, NumChannels);
            
            // Process any pending audio before releasing the lock
            ProcessPendingAudio();

            // Release the lock
			IsAudioConfiguring.AtomicSet(false);
        }
        else
        {            
            AsyncTask(ENamedThreads::GameThread, [WeakThis, AudioDataCopy, SampleRate, NumChannels]()
                {
                    if (WeakThis.IsValid())
						SetupAndPlayAudio(WeakThis, AudioDataCopy, SampleRate, NumChannels);
                    
                    // Process any pending audio before releasing the lock
                    if (WeakThis.IsValid())
                        WeakThis->ProcessPendingAudio();

					// Release the lock
					if (WeakThis.IsValid())
						WeakThis->IsAudioConfiguring.AtomicSet(false);
                });
        }

        if (!IsTalking)
        {
            onAudioStarted();
            IsTalking = true;
        }
        
        return;
    }
    
    SoundWaveProcedural->QueueAudio(VoiceData, VoiceDataSize);

    // Track bytes queued for playback time calculation
    TotalBytesQueuedToAudio += VoiceDataSize;
    AudioPlaybackSampleRate = SampleRate;
    AudioPlaybackNumChannels = NumChannels;

    if (!IsTalking)
    {
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
    // If we're not talking and buffers are empty, nothing to do
    if (!IsTalking)
        return;

    // Reset audio playback
    if (SoundWaveProcedural)
    {
		SoundWaveProcedural->ResetAudio();
	}

    // Clear audio buffer
	AudioRingBuffer.Reset();

	// Reset playback time tracking
	TotalBytesQueuedToAudio = 0;

	// Reset voice fade tracking
	ResetVoiceFade();

    // Reset lipsync state
    StopLipSync();

    // Notify that audio has finished
    onAudioFinished();

	AsyncTask(ENamedThreads::GameThread, [this]()
		{
			ClearAudioFinishedTimer();
		});
}

void UConvaiAudioStreamer::PauseVoice()
{
	if (bIsPaused)
		return;

	GetWorld()->GetTimerManager().PauseTimer(AudioFinishedTimerHandle);
	SetPaused(true);
	IsTalking = false;
}

void UConvaiAudioStreamer::ResumeVoice()
{
	if (!bIsPaused)
		return;

	AsyncTask(ENamedThreads::GameThread, [this]
	{
		GetWorld()->GetTimerManager().UnPauseTimer(AudioFinishedTimerHandle);
	});
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

	double CurrentRemainingAudioDuration = CalculateRemainingAudioDuration(this);
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

void UConvaiAudioStreamer::ClearAudioFinishedTimer()
{
	if (!IsValid(GetWorld()))
	{
		CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("ClearAudioFinishedTimer: GetWorld() is Invalid!"));
		return;
	}
	GetWorld()->GetTimerManager().ClearTimer(AudioFinishedTimerHandle);
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
	MinBufferDuration = UConvaiSettingsUtils::GetParamValueAsFloat("MinBufferDuration", MinBufferDuration) ? MinBufferDuration : 0.12;
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
    AudioRingBuffer.Enqueue(AudioData, AudioDataSize);

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

// Helper function to send new buffered audio to non-precomputed lipsync component
void UConvaiAudioStreamer::SendNewAudioToLipSync(uint32 AvailableBytes, uint32 SampleRate, uint32 NumChannels)
{
    // Calculate how much new data we have that hasn't been sent to lipsync yet
    uint32 NewBytesForLipSync = (AvailableBytes > BytesSentToLipSync) ? (AvailableBytes - BytesSentToLipSync) : 0;

    if (NewBytesForLipSync == 0)
    {
        return;
    }

    // Ensure peek buffer has capacity (only allocates once, keeps slack for reuse)
    if (LipSyncPeekBuffer.Num() < (int32)AvailableBytes)
    {
        LipSyncPeekBuffer.SetNumUninitialized(AvailableBytes);
    }

    // Peek at all buffered data
    uint32 PeekedBytes = AudioRingBuffer.GetData(LipSyncPeekBuffer.GetData(), AvailableBytes);

    if (PeekedBytes >= BytesSentToLipSync + NewBytesForLipSync)
    {
        // Pause the lipsync component so it won't play on its own
        PauseLipSync();

        // Send only the new portion (skip what we've already sent)
        ConvaiLipSync->ConvaiInferFacialDataFromAudio(
            LipSyncPeekBuffer.GetData() + BytesSentToLipSync,
            NewBytesForLipSync,
            SampleRate,
            NumChannels
        );

        // Update tracking
        BytesSentToLipSync += NewBytesForLipSync;
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

	if (!bIsPlayingAudio && SupportsLipSync() && !ConvaiLipSync->RequiresPrecomputedFaceData())
	{
		// Peek and send new audio
		SendNewAudioToLipSync(AvailableBytes, SampleRate, NumChannels);
	}


    // Try to dequeue an audio chunk for playback
    uint32 DequeuedBytes = TryDequeueAudioChunk(AudioChunkBuffer, SampleRate, NumChannels, Force);
    if (DequeuedBytes == 0)
    {
        return;
    }

	if (bIsPlayingAudio && SupportsLipSync() && !ConvaiLipSync->RequiresPrecomputedFaceData())
	{
		// Send dequeued audio directly
        ConvaiLipSync->ConvaiInferFacialDataFromAudio(AudioChunkBuffer.GetData(), DequeuedBytes, SampleRate, NumChannels);
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

// Process incoming lipsync data (called from game thread - heavy logic)
void UConvaiAudioStreamer::ProcessIncomingLipSync()
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

            // Send the accumulated lipsync data
            ConvaiLipSync->ConvaiApplyPrecomputedFacialAnimation(nullptr, 0, 0, 0, Sequence);
        }
    }
}



void UConvaiAudioStreamer::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UpdateVoiceFade(DeltaTime);

	// Process incoming data from transport thread
	ProcessIncomingAudio();
	ProcessIncomingLipSync();
}

void UConvaiAudioStreamer::BeginDestroy()
{
	Super::BeginDestroy();
}

void UConvaiAudioStreamer::StopLipSync()
{
	BytesSentToLipSync = 0;
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
    // If we're not talking, return 0
    if (!IsTalking)
        return 0.0;

    // Calculate buffered audio duration from AudioRingBuffer
    uint32 BufferedBytes = AudioRingBuffer.GetAvailableBytes();
    if (BufferedBytes == 0)
        return 0.0;

    uint32 SampleRate, NumChannels;
    AudioRingBuffer.GetFormat(SampleRate, NumChannels);

    if (SampleRate == 0 || NumChannels == 0)
        return 0.0;

    return UConvaiUtils::CalculateAudioDuration(BufferedBytes, NumChannels, SampleRate, 2);
}

double UConvaiAudioStreamer::GetAudioPlaybackTime() const
{
    // If SoundWaveProcedural is not valid or audio format is not set, return 0
    if (!IsValid(SoundWaveProcedural) || AudioPlaybackSampleRate == 0 || AudioPlaybackNumChannels == 0)
        return 0.0;

    // Get bytes still queued in the audio device (not yet played)
    int32 AvailableBytes = SoundWaveProcedural->GetAvailableAudioByteCount();

    // Calculate bytes that have actually been consumed/played
    int64 BytesConsumed = static_cast<int64>(TotalBytesQueuedToAudio) - AvailableBytes;

    // Ensure we don't return negative time
    if (BytesConsumed <= 0)
        return 0.0;

    // Convert bytes to time: bytes / (sample_rate * channels * bytes_per_sample)
    // Using 2 bytes per sample for int16 PCM audio
    double PlaybackTime = static_cast<double>(BytesConsumed) /
        (static_cast<double>(AudioPlaybackSampleRate) * static_cast<double>(AudioPlaybackNumChannels) * 2.0);

    return PlaybackTime;
}

void UConvaiAudioStreamer::onAudioStarted()
{
	AsyncTask(ENamedThreads::GameThread, [this] {
		OnStartedTalkingDelegate.Broadcast();
		});
	
}

void UConvaiAudioStreamer::onAudioFinished()
{
    CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("onAudioFinished"));

    // Reset the audio end time
    AudioEndTime = 0.0;
    bIsPlayingAudio = false;

    // Broadcast that audio has finished
    AsyncTask(ENamedThreads::GameThread, [this] {
        OnFinishedTalkingDelegate.Broadcast();
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
