// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ConvaiDefinitions.h"
#include "Sound/SoundWave.h"
#include <atomic>
#include "ConvaiReplayComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiReplayComponentLog, Log, All);

class UConvaiChatbotComponent;
class UConvaiFaceSyncComponent;
class IConvaiConnectionInterface;

/**
 * Struct to hold synchronized recorded audio and lipsync data for replay
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiReplaySession
{
	GENERATED_BODY()

	/** Recorded lipsync animation data (not exposed to Blueprint due to complex nested types) */
	UPROPERTY()
	FAnimationSequenceBP RecordedLipSync;

	/** Recorded audio data */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Replay")
	USoundWave* RecordedAudio = nullptr;

	/** Duration of recorded audio in seconds */
	UPROPERTY(BlueprintReadOnly, Category = "Convai|Replay")
	float AudioDuration = 0.0f;

	/** Duration of recorded lipsync in seconds */
	UPROPERTY(BlueprintReadOnly, Category = "Convai|Replay")
	float LipSyncDuration = 0.0f;

	/** Number of lipsync frames recorded */
	UPROPERTY(BlueprintReadOnly, Category = "Convai|Replay")
	int32 LipSyncFrameCount = 0;

	bool IsValid() const { return RecordedAudio != nullptr || LipSyncFrameCount > 0; }
	bool HasAudio() const { return RecordedAudio != nullptr; }
	bool HasLipSync() const { return LipSyncFrameCount > 0; }
};

// Recording events
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRecordingStartedSignature);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRecordingFinishedSignature, const FConvaiReplaySession&, RecordedSession);

// Replay events
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnReplayStartedSignature);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnReplayFinishedSignature);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnReplayInterruptedSignature);

/**
 * Component for recording and replaying audio and lipsync data through the IConvaiConnectionInterface.
 * Supports real-time chunked streaming, interruption, and serialization.
 */
UCLASS(Blueprintable, BlueprintType, meta = (BlueprintSpawnableComponent), DisplayName = "Convai Record Replay")
class CONVAI_API UConvaiReplayComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UConvaiReplayComponent();

	/** The chatbot component to record/replay through (implements IConvaiConnectionInterface) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|RecordReplay")
	UConvaiChatbotComponent* ChatbotComponent;

	/** The face sync component to record lipsync from (optional) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|RecordReplay")
	UConvaiFaceSyncComponent* FaceSyncComponent;

	//~ Audio Streaming Settings

	/** Duration of each audio chunk in seconds for real-time replay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|RecordReplay|Audio", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float AudioChunkDurationSeconds = 0.1f;

	/** Interval between sending audio chunks in seconds for real-time replay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|RecordReplay|Audio", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float AudioChunkIntervalSeconds = 0.1f;

	//~ LipSync Streaming Settings

	/** Number of lipsync frames to send per chunk for real-time replay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|RecordReplay|LipSync", meta = (ClampMin = "1", ClampMax = "120"))
	int32 LipSyncFramesPerChunk = 30;

	/** Interval between sending lipsync chunks in seconds for real-time replay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|RecordReplay|LipSync", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float LipSyncChunkIntervalSeconds = 0.1f;

	//~ Recording Events
	UPROPERTY(BlueprintAssignable, Category = "Convai|RecordReplay")
	FOnRecordingStartedSignature OnRecordingStarted;

	UPROPERTY(BlueprintAssignable, Category = "Convai|RecordReplay")
	FOnRecordingFinishedSignature OnRecordingFinished;

	//~ Replay Events
	UPROPERTY(BlueprintAssignable, Category = "Convai|RecordReplay")
	FOnReplayStartedSignature OnReplayStarted;

	UPROPERTY(BlueprintAssignable, Category = "Convai|RecordReplay")
	FOnReplayFinishedSignature OnReplayFinished;

	UPROPERTY(BlueprintAssignable, Category = "Convai|RecordReplay")
	FOnReplayInterruptedSignature OnReplayInterrupted;

	//~ Component Discovery

	/**
	 * Automatically find and set the ChatbotComponent and FaceSyncComponent from the owner actor if not already valid.
	 * Checks if components are already set and valid before searching.
	 * Searches the owner actor and its children for these components.
	 * @return True if at least one component is now valid (either was already set or was found)
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|RecordReplay")
	bool DiscoverChatbotComponents();

	//~ Recording Functions

	/**
	 * Start recording audio and lipsync data.
	 * Automatically discovers components if they are not already set.
	 * @return True if recording started successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|RecordReplay")
	bool StartRecording();

	/**
	 * Stop recording and return the recorded session.
	 * If FolderName is set, saves audio and lipsync to Saved/ConvaiRecordings/{FolderName}/
	 * @param FolderName Optional folder name to save recording to disk
	 * @return The recorded session containing audio and lipsync data
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|RecordReplay")
	FConvaiReplaySession FinishRecording(const FString& FolderName = TEXT(""));

	/**
	 * Check if currently recording.
	 */
	UFUNCTION(BlueprintPure, Category = "Convai|RecordReplay")
	bool IsRecording() const { return bIsRecording; }

	//~ Replay Functions

	/**
	 * Start replaying a recorded session.
	 * @param Session The recorded session to replay
	 * @param bRealTime If true, streams audio/lipsync in real-time chunks. If false, sends all at once.
	 * @return True if replay started successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|RecordReplay")
	bool StartReplay(const FConvaiReplaySession& Session, bool bRealTime = true);

	/**
	 * Start replaying from a saved folder.
	 * Loads audio and lipsync from Saved/ConvaiRecordings/{FolderName}/
	 * @param FolderName The folder name to load recording from
	 * @param bRealTime If true, streams audio/lipsync in real-time chunks. If false, sends all at once.
	 * @return True if replay started successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|RecordReplay")
	bool StartReplayFromFolder(const FString& FolderName, bool bRealTime = true);

	/**
	 * Interrupt the current replay.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|RecordReplay")
	void InterruptReplay();

	/**
	 * Check if currently replaying.
	 */
	UFUNCTION(BlueprintPure, Category = "Convai|RecordReplay")
	bool IsReplaying() const { return bIsReplaying; }

	/**
	 * Get the current replay progress (0.0 to 1.0).
	 */
	UFUNCTION(BlueprintPure, Category = "Convai|RecordReplay")
	float GetReplayProgress() const;

	//~ File I/O Functions

	/**
	 * Get the base path for recordings: Saved/ConvaiRecordings/
	 */
	UFUNCTION(BlueprintPure, Category = "Convai|RecordReplay")
	static FString GetRecordingsBasePath();

	/**
	 * Save a session to a folder.
	 * @param Session The session to save
	 * @param FolderName The folder name under Saved/ConvaiRecordings/
	 * @return True if save was successful
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|RecordReplay")
	static bool SaveSessionToFolder(const FConvaiReplaySession& Session, const FString& FolderName);

	/**
	 * Load a session from a folder.
	 * @param FolderName The folder name under Saved/ConvaiRecordings/
	 * @param OutSession The loaded session
	 * @return True if load was successful
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|RecordReplay")
	static bool LoadSessionFromFolder(const FString& FolderName, FConvaiReplaySession& OutSession);

protected:
	// Recording state
	bool bIsRecording = false;

	// Replay state
	std::atomic<bool> bIsReplaying;
	std::atomic<bool> bStopReplayRequested;
	bool bRealTimeReplay = true;
	FConvaiReplaySession CurrentSession;

	// Audio streaming state
	TArray<uint8> AudioPCMData;
	int32 AudioSampleRate = 0;
	int32 AudioNumChannels = 0;
	TAtomic<int32> CurrentAudioByteOffset;
	int32 TotalAudioBytes = 0;

	// LipSync streaming state
	TArray<FAnimationFrame> LipSyncFrames;
	int32 LipSyncFrameRate;
	TAtomic<int32> CurrentLipSyncFrameIndex;
	int32 TotalLipSyncFrames = 0;

	// Connection interface (cached from ChatbotComponent)
	IConvaiConnectionInterface* ConnectionInterface = nullptr;

	// Thread-based replay functions
	void ReplayThreadFunction();
	void SendNextAudioChunk();
	void SendNextLipSyncChunk();
	void SendAllAudioAtOnce();
	void SendAllLipSyncAtOnce();
	void FinishReplayOnGameThread();

	// Helper to start replay with loaded session
	bool StartReplayInternal(const FConvaiReplaySession& Session, bool bRealTime);
};

