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


DEFINE_LOG_CATEGORY(ConvaiAudioStreamerLog);
DEFINE_LOG_CATEGORY(ConvaiThreadSafeBuffersLog);

UConvaiAudioStreamer::UConvaiAudioStreamer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	bAutoActivate = true;
}

namespace
{
	void HandleAudioTimer(TWeakObjectPtr<UConvaiAudioStreamer> WeakSelf, int32 PCM_DataSize, int32 SampleRate)
	{
		if (!WeakSelf.IsValid() || !IsValid(WeakSelf->GetWorld()))
		{
			CONVAI_LOG(ConvaiAudioStreamerLog, Warning, TEXT("PlayVoiceData: Object or World is Invalid!"));
			return;
		}

		double NewAudioDuration = UConvaiUtils::CalculateAudioDuration(PCM_DataSize, 1, SampleRate, 2);

		
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
		WeakThis->Play();

		// Lipsync component process the audio data to generate the lipsync
		if (WeakThis.IsValid() && WeakThis->SupportsLipSync() && !(WeakThis->ConvaiLipSync->RequiresPrecomputedFaceData()))
		{
			uint8* NonConstData = const_cast<uint8*>(AudioDataCopy.GetData());
			WeakThis->ConvaiLipSync->ConvaiInferFacialDataFromAudio(NonConstData, AudioDataCopy.Num(), SampleRate, NumChannels);
		}
		
		WeakThis->ForceRecalculateLipsyncStartTime();
	}
};

void UConvaiAudioStreamer::ProcessPendingAudio()
{
	if (!IsValid(SoundWaveProcedural) && PendingAudioBuffer.Num() <= 0)
		return;

    // Process the buffer if there's any data
    SoundWaveProcedural->QueueAudio(PendingAudioBuffer.GetData(), PendingAudioBuffer.Num());

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
    if (IsVoiceCurrentlyFading())
        StopVoice();
    ResetVoiceFade();

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
        HandleAudioTimer(WeakSelf, VoiceDataSize, SampleRate);
    }
    else
    {
        AsyncTask(ENamedThreads::GameThread, [WeakSelf, VoiceDataSize, SampleRate]()
        {
            HandleAudioTimer(WeakSelf, VoiceDataSize, SampleRate);
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

    if (!IsTalking)
    {
        onAudioStarted();
        IsTalking = true;
    }

    // Lipsync component process the audio data to generate the lipsync
    if (SupportsLipSync() && !ConvaiLipSync->RequiresPrecomputedFaceData())
    {
		ConvaiLipSync->ConvaiInferFacialDataFromAudio(VoiceData, VoiceDataSize, SampleRate, NumChannels);
	}
}

void UConvaiAudioStreamer::ForcePlayVoice(USoundWave* VoiceToPlay)
{
	int32 SampleRate;
	int32 NumChannels;
	TArray<uint8> PCMData = UConvaiUtils::ExtractPCMDataFromSoundWave(VoiceToPlay, SampleRate, NumChannels);
	PlayVoiceData(PCMData.GetData(), PCMData.Num(), false, SampleRate, NumChannels);
}

void UConvaiAudioStreamer::StopVoice()
{    
    // Reset the audio end time
    AudioEndTime = 0.0;
    
    // Clear audio buffer
	AudioRingBuffer.Reset();
    bIsPlayingAudio = false;

    // If we're not talking and buffers are empty, nothing to do
    if (!IsTalking)
        return;

    // Reset audio playback
    if (SoundWaveProcedural)
        SoundWaveProcedural->ResetAudio();

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

	float CurrentRemainingAudioDuration = GetWorld()->GetTimerManager().GetTimerRemaining(AudioFinishedTimerHandle);
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

bool UConvaiAudioStreamer::IsVoiceCurrentlyFading()
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
bool UConvaiAudioStreamer::IsLocal()
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
	MinBufferDuration = UConvaiSettingsUtils::GetParamValueAsFloat("MinBufferDuration", MinBufferDuration) ? MinBufferDuration : 0.9f;
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
}

// Process incoming audio data (called from game thread - heavy logic)
void UConvaiAudioStreamer::ProcessIncomingAudio()
{
    // Get audio format
    uint32 SampleRate, NumChannels;
    AudioRingBuffer.GetFormat(SampleRate, NumChannels);

    if (SampleRate == 0 || NumChannels == 0)
    {
        return; // Format not set yet
    }

    // Check if we have any data to process
    uint32 AvailableBytes = AudioRingBuffer.GetAvailableBytes();
    if (AvailableBytes == 0)
    {
        return; // No data to process
    }

    // Calculate how much audio duration we have buffered
    double BufferedDuration = UConvaiUtils::CalculateAudioDuration(AvailableBytes, NumChannels, SampleRate, 2);

    // Decide whether to wait for more buffering or start playing
    bool ShouldWaitForBuffer = !bIsPlayingAudio && (MinBufferDuration > 0.0f) && (BufferedDuration < MinBufferDuration);

    if (ShouldWaitForBuffer)
    {
        // Not enough data buffered yet, wait for more
        return;
    }

    // Ensure buffer is sized correctly (only allocates if needed)
    if (AudioChunkBuffer.Num() != MaxChunkSize)
    {
        AudioChunkBuffer.SetNumUninitialized(MaxChunkSize);
    }

    // Process all available audio in chunks
	uint32 BytesRead = AudioRingBuffer.Dequeue(AudioChunkBuffer.GetData(), MaxChunkSize);
	if (BytesRead == 0)
	{
		return;
	}

	// Play the audio chunk
	PlayVoiceData(AudioChunkBuffer.GetData(), BytesRead, false, SampleRate, NumChannels);

	// Mark as playing and resume lipsync on first chunk
	if (!bIsPlayingAudio)
	{
		bIsPlayingAudio = true;

		// Resume lipsync when starting to play
		if (SupportsLipSync())
		{
			ConvaiLipSync->ConvaiResumeLipSync();
		}
	}
}

// Force play any buffered audio
void UConvaiAudioStreamer::ForcePlayBufferedAudio()
{
    if (!AudioRingBuffer.IsEmpty())
    {
        // Get audio format
        uint32 SampleRate, NumChannels;
        AudioRingBuffer.GetFormat(SampleRate, NumChannels);

        if (SampleRate == 0 || NumChannels == 0)
        {
            return; // Format not set yet
        }

        // Get all buffered audio data
        uint32 BufferedBytes = AudioRingBuffer.GetAvailableBytes();

        // Reuse AudioChunkBuffer (resize if needed)
        if (AudioChunkBuffer.Num() < (int32)BufferedBytes)
        {
            AudioChunkBuffer.SetNumUninitialized(BufferedBytes);
        }

        AudioRingBuffer.Dequeue(AudioChunkBuffer.GetData(), BufferedBytes);

        // Play all buffered audio
        PlayVoiceData(AudioChunkBuffer.GetData(), BufferedBytes, false, SampleRate, NumChannels);

        if (!bIsPlayingAudio)
        {
            bIsPlayingAudio = true;

            // Resume lipsync when starting to play
            if (SupportsLipSync())
            {
                ConvaiLipSync->ConvaiResumeLipSync();
            }
        }
    }
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
	if (ConvaiLipSync)
	{
		ConvaiLipSync->ConvaiStopLipSync();
		ConvaiLipSync->OnFacialDataReady.ExecuteIfBound(); // TODO (Mohamed): This is redundant and should be removed once all users update their OVR lipsync plugin
	}
}

void UConvaiAudioStreamer::PauseLipSync()
{
	if (ConvaiLipSync)
	{
		//ConvaiLipSync->ConvaiPauseLipSync();
	}
}

void UConvaiAudioStreamer::ResumeLipSync()
{
	if (ConvaiLipSync)
	{
		//ConvaiLipSyncExtended->ConvaiResumeLipSync();
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

const TArray<float> UConvaiAudioStreamer::GetFacialData() const
{
	if (ConvaiLipSync)
	{
		return ConvaiLipSync->ConvaiGetFacialData();
	}
	return TArray<float>();
}

const TArray<FString> UConvaiAudioStreamer::GetFacialDataNames() const
{
	if (ConvaiLipSync)
	{
		return ConvaiLipSync->ConvaiGetFacialDataNames();
	}
	return TArray<FString>();
}

const TMap<FName, float> UConvaiAudioStreamer::ConvaiGetFaceBlendshapes() const
{
	if (ConvaiLipSync)
	{
		return ConvaiLipSync->ConvaiGetFaceBlendshapes();
	}
	return TMap<FName, float>();
}

bool UConvaiAudioStreamer::GeneratesFacialDataAsBlendshapes()
{
	if (SupportsLipSync())
	{
		return ConvaiLipSync->GeneratesFacialDataAsBlendshapes();
	}
	return false;
}



double UConvaiAudioStreamer::GetRemainingContentDuration()
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
