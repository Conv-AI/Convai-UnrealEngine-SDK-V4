// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RingBuffer.h"
#include "ConvaiConversationComponent.h"
#include "DSP/BufferVectorOperations.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiAudioProcessingInterface.h"
#include "ConvaiPlayerComponent.generated.h"

#define TIME_BETWEEN_VOICE_UPDATES_SECS 0.01

DECLARE_LOG_CATEGORY_EXTERN(ConvaiPlayerLog, Log, All);

class UConvaiAudioCaptureComponent;
class UConvaiConnectionSessionProxy;
class IConvaiAudioProcessingInterface;

USTRUCT(BlueprintType)
struct FCaptureDeviceInfoBP
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Microphone")
	FString DeviceName = "";

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Microphone")
	int DeviceIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Microphone")
	FString LongDeviceId = "";

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Microphone")
	int InputChannels = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Microphone")
	int PreferredSampleRate = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Microphone")
	bool bSupportsHardwareAEC = 0;
};


UCLASS(Blueprintable, BlueprintType, meta = (BlueprintSpawnableComponent), DisplayName = "Convai Player")
class CONVAI_API UConvaiPlayerComponent : public UConvaiConversationComponent, public IConvaiConnectionInterface , public IConvaiProcessedAudioReceiver
{
	GENERATED_BODY()

	UConvaiPlayerComponent();

	virtual void OnComponentCreated() override;

	void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	bool Init();

	//~ Begin ActorComponent Interface.
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
	//~ End ActorComponent Interface.

public:

	UPROPERTY(EditAnywhere, Category = "Convai", Replicated, BlueprintSetter = SetPlayerName)
	FString PlayerName;

	/**
	 *    Sets a new player name
	 */
	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void SetPlayerName(FString NewPlayerName);

	UFUNCTION(Server, Reliable, Category = "Convai|Network")
	void SetPlayerNameServer(const FString& NewPlayerName);

	/**
	 *   Speaker ID used for long term memory (LTM)
	 */
	UPROPERTY(EditAnywhere, Category = "Convai", Replicated, BlueprintSetter = SetSpeakerID)
	FString SpeakerID;

	/**
	 *    Sets a new Speaker ID
	 */
	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void SetSpeakerID(FString NewSpeakerID);

	UFUNCTION(Server, Reliable, Category = "Convai|Network")
	void SetSpeakerIDServer(const FString& NewSpeakerID);

	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	bool GetDefaultCaptureDeviceInfo(FCaptureDeviceInfoBP& OutInfo);

	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	bool GetCaptureDeviceInfo(FCaptureDeviceInfoBP& OutInfo, int DeviceIndex);

	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	TArray<FCaptureDeviceInfoBP> GetAvailableCaptureDeviceDetails();

	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	TArray<FString> GetAvailableCaptureDeviceNames();

	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	void GetActiveCaptureDevice(FCaptureDeviceInfoBP& OutInfo);

	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	bool SetCaptureDeviceByIndex(int DeviceIndex);

	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	bool SetCaptureDeviceByName(FString DeviceName);

	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	void SetMicrophoneVolumeMultiplier(float InVolumeMultiplier, bool& Success);

	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Microphone")
	void GetMicrophoneVolumeMultiplier(float& OutVolumeMultiplier, bool& Success);

	/**
	 *    Start recording audio from the microphone, use "Finish Recording" function afterwards
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	void StartRecording();

	/**
	 *    Stops recording from the microphone and outputs the recorded audio as a Sound Wave
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	USoundWave* FinishRecording();

	/**
	 * Initializes the session for this player component
	 * @return True if the session was initialized successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Session")
	bool StartSession();

	/**
	 * Shuts down the session for this player component
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Session")
	void StopSession();

	UFUNCTION(BlueprintCallable, Category = "Convai|Session")
	void SendText(UConvaiConversationComponent* ChatbotComponent, FString Text) const;

	/**
	 * Starts streaming audio to the session
	 * @return True if streaming was started successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Session")
	bool UnmuteStreamingAudio();

	/**
	 * Stops streaming audio to the session
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Session")
	void MuteStreamingAudio();

	// UActorComponent interface
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	bool ConsumeStreamingBuffer(TArray<uint8>& Buffer);

	// Returns true if microphone audio is being streamed, false otherwise.
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Microphone", meta = (DisplayName = "Is Streaming"))
	bool GetIsStreaming()
	{
		return(IsStreaming);
	}

	// Returns true if microphone audio is being recorded, false otherwise.
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Microphone", meta = (DisplayName = "Is Recording"))
	bool GetIsRecording()
	{
		return(IsRecording);
	}

public:
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai|PixelStreaming")
	TWeakObjectPtr<USynthComponent> PixelStreamingAudioComponent;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai|PixelStreaming")
	bool UsePixelStreamingMicInput;

	bool IsPixelStreamingEnabledAndAllowed();

	/** Whether to automatically initialize the session in BeginPlay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Session")
	bool bAutoInitializeSession;

private:
	UPROPERTY()
	USoundWaveProcedural* VoiceCaptureSoundWaveProcedural;

	// Buffer used with recording
	TArray<uint8> VoiceCaptureBuffer;

	// Buffer used with streaming
	TRingBuffer<uint8> VoiceCaptureRingBuffer;

	UPROPERTY()
	UConvaiAudioCaptureComponent* AudioCaptureComponent = nullptr;

	/** The session proxy instance */
	UPROPERTY()
	UConvaiConnectionSessionProxy* SessionProxyInstance;

	void UpdateVoiceCapture(float DeltaTime);
	void StartVoiceChunkCapture(float ExpectedRecordingTime = 0.01);
	void StopVoiceChunkCapture();
	void ReadRecordedBuffer(Audio::AlignedFloatBuffer& RecordedBuffer, float& OutNumChannels, float& OutSampleRate);

	void StartAudioCaptureComponent();
	void StopAudioCaptureComponent();

	bool IsRecording = false;
	bool IsStreaming = false;
	bool IsInit = false;

	float RemainingTimeUntilNextUpdate = 0;

	USoundSubmixBase* _FoundSubmix;

	// Override from UConvaiConversationComponent
	virtual bool IsPlayer() const override { return false; }
	virtual FString GetConversationalName() const override { return PlayerName; }
	
	// IConvaiConnectionInterface implementation
	virtual void OnConnectedToServer() override;
	virtual void OnDisconnectedFromServer() override;
	virtual void OnAttendeeConnected(FString AttendeeId) override;
	virtual void OnAttendeeDisconnected(FString AttendeeId) override;
	virtual void OnTranscriptionReceived(FString Transcription, bool IsTranscriptionReady, bool IsFinal) override;
	virtual void OnStartedTalking() override;
	virtual void OnFinishedTalking() override;
	virtual void OnAudioDataReceived(const int16_t* AudioData, size_t NumFrames, uint32_t SampleRate, uint32_t BitsPerSample, uint32_t NumChannels) override;
	virtual void OnFailure(FString Message) override;

	// Server connection state handling
	UFUNCTION()
	void OnServerConnectionStateChanged(EC_ConnectionState ConnectionState);

	// Audio processing
	UPROPERTY()
	TScriptInterface<IConvaiAudioProcessingInterface> ConvaiAudioProcessing;
	class IConvaiAudioProcessingInterface* FindFirstAudioProcessingComponent();

	UFUNCTION(BlueprintCallable, Category = "Convai|AudioProcessing")
	bool SetAudioProcessingComponent(UActorComponent* AudioProcessingComponent);

	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|AudioProcessing")
	bool SupportsAudioProcessing();

	// Thread-safe audio processing wrapper
	void SafeProcessAudioData(const int16* AudioData, int32 NumSamples, int32 SampleRate);

	// IConvaiProcessedAudioReceiver interface implementation
	virtual void OnProcessedAudioDataReceived(const int16* ProcessedAudioData, int32 NumSamples, int32 SampleRate) override;

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|AudioProcessing")
	bool bMute = false;

	UFUNCTION(BlueprintCallable , Category = "Convai|AudioProcessing")
	bool UpdateVadBP(bool EnableVAD);

	bool bEnableAudioProcessingParse = true; //(for enable and disable audio processing )
};
