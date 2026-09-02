// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "Tests/ConvaiReplayComponent.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiFaceSync.h"
#include "ConvaiUtils.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Async/Async.h"

DEFINE_LOG_CATEGORY(ConvaiReplayComponentLog);

// File names for saved recordings
static const FString AudioFileName = TEXT("audio.wav");
static const FString LipSyncFileName = TEXT("lipsync.json");

UConvaiReplayComponent::UConvaiReplayComponent()
	: bIsReplaying(false)
	, bStopReplayRequested(false)
	, CurrentAudioByteOffset(0)
	, CurrentLipSyncFrameIndex(0)
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UConvaiReplayComponent::DiscoverChatbotComponents()
{
	// Check if ChatbotComponent is already valid
	if (ChatbotComponent && ChatbotComponent->IsValidLowLevel())
	{
		UE_LOG(ConvaiReplayComponentLog, Log, TEXT("DiscoverChatbotComponents: ChatbotComponent already set and valid"));
	}
	else
	{
		// Try to find ChatbotComponent
		AActor* OwnerActor = GetOwner();
		if (!OwnerActor)
		{
			UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("DiscoverChatbotComponents: Owner actor is null"));
			ChatbotComponent = nullptr;
		}
		else
		{
			ChatbotComponent = OwnerActor->FindComponentByClass<UConvaiChatbotComponent>();
			if (ChatbotComponent)
			{
				UE_LOG(ConvaiReplayComponentLog, Log, TEXT("DiscoverChatbotComponents: Found ChatbotComponent"));
			}
			else
			{
				UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("DiscoverChatbotComponents: Could not find ChatbotComponent on owner actor"));
			}
		}
	}

	// Check if FaceSyncComponent is already valid
	if (FaceSyncComponent && FaceSyncComponent->IsValidLowLevel())
	{
		UE_LOG(ConvaiReplayComponentLog, Log, TEXT("DiscoverChatbotComponents: FaceSyncComponent already set and valid"));
	}
	else
	{
		// Try to find FaceSyncComponent
		AActor* OwnerActor = GetOwner();
		if (!OwnerActor)
		{
			UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("DiscoverChatbotComponents: Owner actor is null"));
			FaceSyncComponent = nullptr;
		}
		else
		{
			FaceSyncComponent = OwnerActor->FindComponentByClass<UConvaiFaceSyncComponent>();
			if (FaceSyncComponent)
			{
				UE_LOG(ConvaiReplayComponentLog, Log, TEXT("DiscoverChatbotComponents: Found FaceSyncComponent"));
			}
			else
			{
				UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("DiscoverChatbotComponents: Could not find FaceSyncComponent on owner actor"));
			}
		}
	}

	// Return true if at least one component is valid
	bool bChatbotValid = ChatbotComponent && ChatbotComponent->IsValidLowLevel();
	bool bFaceSyncValid = FaceSyncComponent && FaceSyncComponent->IsValidLowLevel();
	return bChatbotValid || bFaceSyncValid;
}

void UConvaiReplayComponent::ReplayThreadFunction()
{
	UE_LOG(ConvaiReplayComponentLog, Log, TEXT("ReplayThreadFunction: Started on background thread"));

	// Create weak pointer for validity checks
	TWeakObjectPtr<UConvaiReplayComponent> WeakThis(this);

	double LastAudioChunkTime = FPlatformTime::Seconds();
	double LastLipSyncChunkTime = FPlatformTime::Seconds();

	// Track completion of both streams
	bool bAudioComplete = (TotalAudioBytes <= 0);
	bool bLipSyncComplete = (TotalLipSyncFrames <= 0);

	while (bIsReplaying && !bStopReplayRequested)
	{
		// Check if component is still valid
		if (!WeakThis.IsValid())
		{
			UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("ReplayThreadFunction: Component destroyed, exiting thread"));
			return;
		}

		double CurrentTime = FPlatformTime::Seconds();

		// Send audio chunks
		if (!bAudioComplete)
		{
			double AudioElapsedTime = CurrentTime - LastAudioChunkTime;
			if (AudioElapsedTime >= AudioChunkIntervalSeconds)
			{
				LastAudioChunkTime = CurrentTime;
				SendNextAudioChunk();

				// Check if we've finished sending all audio
				if (CurrentAudioByteOffset >= TotalAudioBytes)
				{
					bAudioComplete = true;
					UE_LOG(ConvaiReplayComponentLog, Log, TEXT("ReplayThreadFunction: Audio streaming complete"));
				}
			}
		}

		// Send lipsync chunks
		if (!bLipSyncComplete)
		{
			double LipSyncElapsedTime = CurrentTime - LastLipSyncChunkTime;
			if (LipSyncElapsedTime >= LipSyncChunkIntervalSeconds)
			{
				LastLipSyncChunkTime = CurrentTime;
				SendNextLipSyncChunk();

				// Check if we've finished sending all lipsync
				if (CurrentLipSyncFrameIndex >= TotalLipSyncFrames)
				{
					bLipSyncComplete = true;
					UE_LOG(ConvaiReplayComponentLog, Log, TEXT("ReplayThreadFunction: LipSync streaming complete"));
				}
			}
		}

		// Check if both streams are complete
		if (bAudioComplete && bLipSyncComplete)
		{
			break;
		}

		// Sleep to avoid busy-waiting (use the smaller interval)
		float SleepTime = FMath::Min(AudioChunkIntervalSeconds, LipSyncChunkIntervalSeconds) * 0.5f;
		FPlatformProcess::Sleep(SleepTime);
	}

	// Finish replay on game thread
	AsyncTask(ENamedThreads::GameThread, [WeakThis]()
	{
		if (UConvaiReplayComponent* Component = WeakThis.Get())
		{
			Component->FinishReplayOnGameThread();
		}
	});

	UE_LOG(ConvaiReplayComponentLog, Log, TEXT("ReplayThreadFunction: Finished"));
}

bool UConvaiReplayComponent::StartReplay(const FConvaiReplaySession& Session, bool bRealTime)
{
	return StartReplayInternal(Session, bRealTime);
}

bool UConvaiReplayComponent::StartReplayFromFolder(const FString& FolderName, bool bRealTime)
{
	if (FolderName.IsEmpty())
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("StartReplayFromFolder: FolderName is empty"));
		return false;
	}

	FConvaiReplaySession Session;
	if (!LoadSessionFromFolder(FolderName, Session))
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("StartReplayFromFolder: Failed to load session from folder '%s'"), *FolderName);
		return false;
	}

	return StartReplayInternal(Session, bRealTime);
}

bool UConvaiReplayComponent::StartReplayInternal(const FConvaiReplaySession& Session, bool bRealTime)
{
	if (!Session.IsValid())
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("StartReplay: Invalid session"));
		return false;
	}

	// Discover components if not already valid
	if (!DiscoverChatbotComponents())
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("StartReplay: Failed to discover valid ChatbotComponent"));
		return false;
	}

	// Cast to IConvaiConnectionInterface
	ConnectionInterface = Cast<IConvaiConnectionInterface>(ChatbotComponent);
	if (!ConnectionInterface)
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("StartReplay: ChatbotComponent does not implement IConvaiConnectionInterface"));
		return false;
	}

	// Store the session and mode
	CurrentSession = Session;
	bRealTimeReplay = bRealTime;

	// Extract PCM data from the audio if available
	TotalAudioBytes = 0;
	if (Session.HasAudio())
	{
		AudioPCMData = UConvaiUtils::ExtractPCMDataFromSoundWave(Session.RecordedAudio, AudioSampleRate, AudioNumChannels);
		if (AudioPCMData.Num() == 0)
		{
			UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("StartReplay: Failed to extract PCM data from audio"));
			return false;
		}
		TotalAudioBytes = AudioPCMData.Num();
		UE_LOG(ConvaiReplayComponentLog, Log, TEXT("StartReplay: Extracted %d bytes of PCM data, SampleRate: %d, Channels: %d"),
			AudioPCMData.Num(), AudioSampleRate, AudioNumChannels);
	}

	// Extract lipsync frames if available
	TotalLipSyncFrames = 0;
	if (Session.HasLipSync())
	{
		LipSyncFrames = Session.RecordedLipSync.AnimationSequence.AnimationFrames;
		LipSyncFrameRate = Session.RecordedLipSync.AnimationSequence.FrameRate;
		TotalLipSyncFrames = LipSyncFrames.Num();
		UE_LOG(ConvaiReplayComponentLog, Log, TEXT("StartReplay: Loaded %d lipsync frames"), TotalLipSyncFrames);
	}

	// Reset state
	CurrentAudioByteOffset = 0;
	CurrentLipSyncFrameIndex = 0;
	bStopReplayRequested = false;
	bIsReplaying = true;

	// Signal start of talking
	ConnectionInterface->OnStartedTalking();
	UE_LOG(ConvaiReplayComponentLog, Log, TEXT("StartReplay: Signaled OnStartedTalking (RealTime: %s)"), bRealTime ? TEXT("true") : TEXT("false"));

	// Broadcast event
	OnReplayStarted.Broadcast();

	if (bRealTime)
	{
		// Launch replay on background thread with weak pointer for safety
		TWeakObjectPtr<UConvaiReplayComponent> WeakThis(this);
		Async(EAsyncExecution::ThreadPool, [WeakThis]()
		{
			if (UConvaiReplayComponent* Component = WeakThis.Get())
			{
				Component->ReplayThreadFunction();
			}
		});
	}
	else
	{
		// Send all data at once
		SendAllAudioAtOnce();
		SendAllLipSyncAtOnce();
	}

	return true;
}

void UConvaiReplayComponent::InterruptReplay()
{
	if (!bIsReplaying)
	{
		return;
	}

	UE_LOG(ConvaiReplayComponentLog, Log, TEXT("InterruptReplay: Replay interrupted at offset %d / %d bytes"),
		CurrentAudioByteOffset.Load(), AudioPCMData.Num());

	// Signal the thread to stop - the thread will handle cleanup via FinishReplayOnGameThread
	bStopReplayRequested = true;
}

float UConvaiReplayComponent::GetReplayProgress() const
{
	if (!bIsReplaying || TotalAudioBytes <= 0)
	{
		return 0.0f;
	}
	return static_cast<float>(CurrentAudioByteOffset.Load()) / static_cast<float>(TotalAudioBytes);
}

void UConvaiReplayComponent::SendNextAudioChunk()
{
	if (!ConnectionInterface || AudioPCMData.Num() == 0)
	{
		return;
	}

	// Calculate chunk size in bytes
	// AudioChunkDurationSeconds * SampleRate * NumChannels * BytesPerSample (2 for int16)
	const int32 BytesPerSample = 2; // int16
	const int32 ChunkSizeBytes = FMath::RoundToInt(AudioChunkDurationSeconds * AudioSampleRate * AudioNumChannels * BytesPerSample);

	// Get current offset atomically
	const int32 CurrentOffset = CurrentAudioByteOffset.Load();

	// Check if we've sent all audio
	if (CurrentOffset >= AudioPCMData.Num())
	{
		return;
	}

	// Calculate how many bytes to send this chunk
	const int32 RemainingBytes = AudioPCMData.Num() - CurrentOffset;
	const int32 BytesToSend = FMath::Min(ChunkSizeBytes, RemainingBytes);

	// Ensure we're sending complete samples (aligned to BytesPerSample * NumChannels)
	const int32 SampleAlignment = BytesPerSample * AudioNumChannels;
	const int32 AlignedBytesToSend = (BytesToSend / SampleAlignment) * SampleAlignment;

	if (AlignedBytesToSend <= 0)
	{
		return;
	}

	// Calculate number of frames (samples per channel)
	const int32 NumFrames = AlignedBytesToSend / (BytesPerSample * AudioNumChannels);

	// Get pointer to audio data
	const int16_t* AudioDataPtr = reinterpret_cast<const int16_t*>(AudioPCMData.GetData() + CurrentOffset);

	// Send audio through the connection interface
	ConnectionInterface->OnAudioDataReceived(AudioDataPtr, NumFrames, AudioSampleRate, 16, AudioNumChannels);

	// Update offset atomically
	CurrentAudioByteOffset.Store(CurrentOffset + AlignedBytesToSend);

	UE_LOG(ConvaiReplayComponentLog, Verbose, TEXT("SendNextAudioChunk: Sent %d frames (%d bytes), progress: %.1f%%"),
		NumFrames, AlignedBytesToSend, GetReplayProgress() * 100.0f);
}

void UConvaiReplayComponent::SendNextLipSyncChunk()
{
	if (!ConnectionInterface || LipSyncFrames.Num() == 0)
	{
		return;
	}

	// Get current frame index atomically
	const int32 CurrentIndex = CurrentLipSyncFrameIndex.Load();

	// Check if we've sent all lipsync frames
	if (CurrentIndex >= LipSyncFrames.Num())
	{
		return;
	}

	// Calculate how many frames to send this chunk
	const int32 RemainingFrames = LipSyncFrames.Num() - CurrentIndex;
	const int32 FramesToSend = FMath::Min(LipSyncFramesPerChunk, RemainingFrames);

	if (FramesToSend <= 0)
	{
		return;
	}

	// Create a partial animation sequence with just the frames for this chunk
	FAnimationSequence ChunkSequence;
	ChunkSequence.FrameRate = LipSyncFrameRate;
	ChunkSequence.AnimationFrames.Reserve(FramesToSend);
	ChunkSequence.Duration = double(FramesToSend) / double(LipSyncFrameRate);
	for (int32 i = 0; i < FramesToSend; ++i)
	{
		ChunkSequence.AnimationFrames.Add(LipSyncFrames[CurrentIndex + i]);
	}

	// Send lipsync chunk through the connection interface
	ConnectionInterface->OnFaceDataReceived(ChunkSequence);

	// Update index atomically
	CurrentLipSyncFrameIndex.Store(CurrentIndex + FramesToSend);

	UE_LOG(ConvaiReplayComponentLog, Verbose, TEXT("SendNextLipSyncChunk: Sent %d frames, progress: %d/%d"),
		FramesToSend, CurrentIndex + FramesToSend, TotalLipSyncFrames);
}

void UConvaiReplayComponent::SendAllAudioAtOnce()
{
	if (!ConnectionInterface || AudioPCMData.Num() == 0)
	{
		return;
	}

	const int32 BytesPerSample = 2; // int16
	const int32 NumFrames = AudioPCMData.Num() / (BytesPerSample * AudioNumChannels);
	const int16_t* AudioDataPtr = reinterpret_cast<const int16_t*>(AudioPCMData.GetData());

	// Send all audio at once
	ConnectionInterface->OnAudioDataReceived(AudioDataPtr, NumFrames, AudioSampleRate, 16, AudioNumChannels);
	CurrentAudioByteOffset.Store(AudioPCMData.Num());

	UE_LOG(ConvaiReplayComponentLog, Log, TEXT("SendAllAudioAtOnce: Sent %d frames (%d bytes)"), NumFrames, AudioPCMData.Num());
}

void UConvaiReplayComponent::SendAllLipSyncAtOnce()
{
	if (!ConnectionInterface || LipSyncFrames.Num() == 0)
	{
		FinishReplayOnGameThread();
		return;
	}

	// Create animation sequence with all frames
	FAnimationSequence FullSequence;
	FullSequence.AnimationFrames = LipSyncFrames;

	// Send all lipsync at once
	ConnectionInterface->OnFaceDataReceived(FullSequence);
	CurrentLipSyncFrameIndex.Store(LipSyncFrames.Num());

	UE_LOG(ConvaiReplayComponentLog, Log, TEXT("SendAllLipSyncAtOnce: Sent %d frames"), LipSyncFrames.Num());

	// Finish immediately
	FinishReplayOnGameThread();
}

void UConvaiReplayComponent::FinishReplayOnGameThread()
{
	if (!bIsReplaying)
	{
		return;
	}

	// Check if interrupted
	bool bWasInterrupted = bStopReplayRequested;

	UE_LOG(ConvaiReplayComponentLog, Log, TEXT("FinishReplayOnGameThread: Replay %s"),
		bWasInterrupted ? TEXT("interrupted") : TEXT("completed"));

	bIsReplaying = false;
	bStopReplayRequested = false;

	// Signal end of talking
	if (ConnectionInterface)
	{
		ConnectionInterface->OnFinishedTalking();
		UE_LOG(ConvaiReplayComponentLog, Log, TEXT("FinishReplayOnGameThread: Signaled OnFinishedTalking"));
	}

	// Clear state
	AudioPCMData.Empty();
	CurrentAudioByteOffset.Store(0);
	TotalAudioBytes = 0;
	ConnectionInterface = nullptr;

	// Broadcast appropriate event
	if (bWasInterrupted)
	{
		OnReplayInterrupted.Broadcast();
	}
	else
	{
		OnReplayFinished.Broadcast();
	}
}

//~ Recording Functions

bool UConvaiReplayComponent::StartRecording()
{
	// Discover components if not already valid
	if (!DiscoverChatbotComponents())
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("StartRecording: Failed to discover valid components"));
		return false;
	}

	if (bIsRecording)
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("StartRecording: Already recording"));
		return false;
	}

	// Start recording audio
	if (ChatbotComponent)
	{
		ChatbotComponent->StartRecordingIncomingAudio();
		UE_LOG(ConvaiReplayComponentLog, Log, TEXT("StartRecording: Started audio recording"));
	}

	// Start recording lipsync
	if (FaceSyncComponent)
	{
		FaceSyncComponent->StartRecordingLipSync();
		UE_LOG(ConvaiReplayComponentLog, Log, TEXT("StartRecording: Started lipsync recording"));
	}

	bIsRecording = true;
	OnRecordingStarted.Broadcast();
	return true;
}

FConvaiReplaySession UConvaiReplayComponent::FinishRecording(const FString& FolderName)
{
	FConvaiReplaySession Session;

	if (!bIsRecording)
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("FinishRecording: Not currently recording"));
		return Session;
	}

	// Finish recording audio
	if (ChatbotComponent)
	{
		Session.RecordedAudio = ChatbotComponent->FinishRecordingIncomingAudio();
		if (Session.RecordedAudio)
		{
			Session.AudioDuration = Session.RecordedAudio->GetDuration();
			UE_LOG(ConvaiReplayComponentLog, Log, TEXT("FinishRecording: Recorded audio duration: %.2f seconds"), Session.AudioDuration);
		}
	}

	// Finish recording lipsync
	if (FaceSyncComponent)
	{
		Session.RecordedLipSync = FaceSyncComponent->FinishRecordingLipSync();
		Session.LipSyncDuration = Session.RecordedLipSync.AnimationSequence.Duration;
		Session.LipSyncFrameCount = Session.RecordedLipSync.AnimationSequence.AnimationFrames.Num();
		UE_LOG(ConvaiReplayComponentLog, Log, TEXT("FinishRecording: Recorded lipsync duration: %.2f seconds, frames: %d"),
			Session.LipSyncDuration, Session.LipSyncFrameCount);
	}

	bIsRecording = false;

	// Save to folder if specified
	if (!FolderName.IsEmpty())
	{
		if (SaveSessionToFolder(Session, FolderName))
		{
			UE_LOG(ConvaiReplayComponentLog, Log, TEXT("FinishRecording: Saved session to folder '%s'"), *FolderName);
		}
		else
		{
			UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("FinishRecording: Failed to save session to folder '%s'"), *FolderName);
		}
	}

	OnRecordingFinished.Broadcast(Session);
	return Session;
}

//~ File I/O Functions

FString UConvaiReplayComponent::GetRecordingsBasePath()
{
	return FPaths::ProjectSavedDir() / TEXT("ConvaiRecordings");
}

bool UConvaiReplayComponent::SaveSessionToFolder(const FConvaiReplaySession& Session, const FString& FolderName)
{
	if (FolderName.IsEmpty())
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("SaveSessionToFolder: FolderName is empty"));
		return false;
	}

	FString FolderPath = GetRecordingsBasePath() / FolderName;

	// Create directory if it doesn't exist
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	bool bSuccess = true;

	// Save audio
	if (Session.HasAudio())
	{
		FString AudioPath = FolderPath / AudioFileName;
		if (!UConvaiUtils::WriteSoundWaveToWavFile(Session.RecordedAudio, AudioPath))
		{
			UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("SaveSessionToFolder: Failed to save audio to '%s'"), *AudioPath);
			bSuccess = false;
		}
		else
		{
			UE_LOG(ConvaiReplayComponentLog, Log, TEXT("SaveSessionToFolder: Saved audio to '%s'"), *AudioPath);
		}
	}

	// Save lipsync
	if (Session.HasLipSync())
	{
		FString LipSyncPath = FolderPath / LipSyncFileName;
		FString JsonString = Session.RecordedLipSync.AnimationSequence.ToJson();
		if (!UConvaiUtils::WriteStringToFile(JsonString, LipSyncPath))
		{
			UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("SaveSessionToFolder: Failed to save lipsync to '%s'"), *LipSyncPath);
			bSuccess = false;
		}
		else
		{
			UE_LOG(ConvaiReplayComponentLog, Log, TEXT("SaveSessionToFolder: Saved lipsync to '%s'"), *LipSyncPath);
		}
	}

	return bSuccess;
}

bool UConvaiReplayComponent::LoadSessionFromFolder(const FString& FolderName, FConvaiReplaySession& OutSession)
{
	if (FolderName.IsEmpty())
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("LoadSessionFromFolder: FolderName is empty"));
		return false;
	}

	FString FolderPath = GetRecordingsBasePath() / FolderName;

	// Check if folder exists
	if (!IFileManager::Get().DirectoryExists(*FolderPath))
	{
		UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("LoadSessionFromFolder: Folder does not exist: '%s'"), *FolderPath);
		return false;
	}

	bool bLoadedAnything = false;

	// Load audio
	FString AudioPath = FolderPath / AudioFileName;
	if (IFileManager::Get().FileExists(*AudioPath))
	{
		TArray<uint8> WavData;
		if (UConvaiUtils::ReadFileAsByteArray(AudioPath, WavData))
		{
			OutSession.RecordedAudio = UConvaiUtils::WavDataToSoundWave(WavData);
			if (OutSession.RecordedAudio)
			{
				OutSession.AudioDuration = OutSession.RecordedAudio->GetDuration();
				UE_LOG(ConvaiReplayComponentLog, Log, TEXT("LoadSessionFromFolder: Loaded audio from '%s', duration: %.2f seconds"),
					*AudioPath, OutSession.AudioDuration);
				bLoadedAnything = true;
			}
		}
		else
		{
			UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("LoadSessionFromFolder: Failed to read audio file '%s'"), *AudioPath);
		}
	}

	// Load lipsync
	FString LipSyncPath = FolderPath / LipSyncFileName;
	if (IFileManager::Get().FileExists(*LipSyncPath))
	{
		FString JsonString;
		if (UConvaiUtils::ReadStringFromFile(JsonString, LipSyncPath))
		{
			if (OutSession.RecordedLipSync.AnimationSequence.FromJson(JsonString))
			{
				OutSession.LipSyncDuration = OutSession.RecordedLipSync.AnimationSequence.Duration;
				OutSession.LipSyncFrameCount = OutSession.RecordedLipSync.AnimationSequence.AnimationFrames.Num();
				UE_LOG(ConvaiReplayComponentLog, Log, TEXT("LoadSessionFromFolder: Loaded lipsync from '%s', frames: %d, duration: %.2f seconds"),
					*LipSyncPath, OutSession.LipSyncFrameCount, OutSession.LipSyncDuration);
				bLoadedAnything = true;
			}
			else
			{
				UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("LoadSessionFromFolder: Failed to parse lipsync JSON from '%s'"), *LipSyncPath);
			}
		}
		else
		{
			UE_LOG(ConvaiReplayComponentLog, Warning, TEXT("LoadSessionFromFolder: Failed to read lipsync file '%s'"), *LipSyncPath);
		}
	}

	return bLoadedAnything;
}

#endif // WITH_TESTS
