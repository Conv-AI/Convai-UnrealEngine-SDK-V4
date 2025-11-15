// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "Subsystems/SubsystemCollection.h"
#include "Net/OnlineBlueprintCallProxyBase.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiDefinitions.h"
#include "ConvaiReferenceAudioThread.h"

#include <convai/convai_client.h>

#include "ConvaiSubsystem.generated.h"

// Forward declarations
namespace convai {
    class ConvaiClient;
    class IConvaiClientListner;
}

#ifdef __APPLE__
extern bool GetAppleMicPermission();
#endif

DECLARE_LOG_CATEGORY_EXTERN(ConvaiSubsystemLog, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(ConvaiClientLog, Log, All);

// Connection state delegate
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnServerConnectionStateChangedSignature, EC_ConnectionState, ConnectionState);

// Connection thread class
class FConvaiConnectionThread : public FRunnable
{
public:
    FConvaiConnectionThread(const FConvaiConnectionParams& ConnectionParams);
    FConvaiConnectionThread(FConvaiConnectionParams&& ConnectionParams);
    virtual ~FConvaiConnectionThread() override;

    // FRunnable interface
    virtual bool Init() override{ return true;}
    virtual uint32 Run() override;
    virtual void Stop() override{ bShouldStop = true;}
    virtual void Exit() override{}
  
private:
    void InitializeThread();
    
    FConvaiConnectionParams ConnectionParams;
    FThreadSafeBool bShouldStop;
    FRunnableThread* Thread;
};

UCLASS(meta = (DisplayName = "Convai Subsystem"))
class CONVAI_API UConvaiSubsystem : public UGameInstanceSubsystem, public convai::IConvaiClientListner
{
    GENERATED_BODY()

public:
    UConvaiSubsystem();

    /** Called when server connection state changes */
    UPROPERTY(BlueprintAssignable, Category = "Convai|Connection")
    FOnServerConnectionStateChangedSignature OnServerConnectionStateChangedEvent;

    // Begin USubsystem
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    // End USubsystem

    // Get Android microphone permission
    static void GetAndroidMicPermission();
    
    /**
     * Connect a session to the Convai service
     * @param SessionProxy - The session proxy to connect
     * @param CharacterID - The ID of the character to connect to (ignored for player sessions)
     * @return True if connection was initiated successfully
     */
    bool ConnectSession(UConvaiConnectionSessionProxy* SessionProxy, const FString& CharacterID);
    
    /**
     * Disconnect a session from the Convai service
     * @param SessionProxy - The session proxy to disconnect
     */
    void DisconnectSession(const UConvaiConnectionSessionProxy* SessionProxy);
    
    /**
     * Send audio data through a session
     * @param SessionProxy - The session proxy to send audio through
     * @param AudioData - The audio data to send
     * @param NumFrames - The number of frames in the audio data
     * @return The number of bytes sent, or -1 on failure
     */
    int32 SendAudio(const UConvaiConnectionSessionProxy* SessionProxy, const int16_t* AudioData, size_t NumFrames) const;
    void SendImage(const UConvaiConnectionSessionProxy* SessionProxy, uint32 Width, uint32 Height, TArray<uint8>& Data);
    void SendTextMessage(const UConvaiConnectionSessionProxy* SessionProxy,const FString& Message) const;
    void SendTriggerMessage(const UConvaiConnectionSessionProxy* SessionProxy,const FString& Trigger_Name, const FString& Trigger_Message) const;
    void UpdateTemplateKeys(const UConvaiConnectionSessionProxy* SessionProxy,TMap<FString, FString> Template_Keys) const;
    void UpdateDynamicInfo(const UConvaiConnectionSessionProxy* SessionProxy,const FString& Context_Text) const;
    static void OnConnectionFailed();
    
    void RegisterChatbotComponent(class UConvaiChatbotComponent* ChatbotComponent);
    void UnregisterChatbotComponent(class UConvaiChatbotComponent* ChatbotComponent);
    TArray<class UConvaiChatbotComponent*> GetAllChatbotComponents() const;
    void RegisterPlayerComponent(class UConvaiPlayerComponent* PlayerComponent);
    void UnregisterPlayerComponent(class UConvaiPlayerComponent* PlayerComponent);
    TArray<class UConvaiPlayerComponent*> GetAllPlayerComponents() const;

    /**
     * Start test recording of reference audio (system/speaker audio)
     * This will record audio before and after resampling to WAV files
     */
    UFUNCTION(BlueprintCallable, Category = "Convai|Testing")
    void StartTestRecording();

    /**
     * Stop test recording and save audio files
     * Files will be saved to: C:\Space\Unreal project\5.3\tutorialAddon\
     * - test_recording_before_resample.wav (original sample rate)
     * - test_recording_after_resample.wav (resampled to 16kHz)
     */
    UFUNCTION(BlueprintCallable, Category = "Convai|Testing")
    void StopTestRecording();

private:    
    convai::ConvaiClient* ConvaiClient;
    TUniquePtr<FConvaiConnectionThread> ConnectionThread;
    TSharedPtr<FConvaiReferenceAudioThread> ReferenceAudioThread;
    FThreadSafeBool bIsConnected;
    FThreadSafeBool bStartedPublishingVideo;
    FCriticalSection CriticalSection;
    
    UPROPERTY()
    UConvaiConnectionSessionProxy* CurrentCharacterSession;
    
    UPROPERTY()
    UConvaiConnectionSessionProxy* CurrentPlayerSession;

    UPROPERTY()
    TArray<class UConvaiChatbotComponent*> RegisteredChatbotComponents;

    UPROPERTY()
    TArray<class UConvaiPlayerComponent*> RegisteredPlayerComponents;
    
// Client callbacks
    virtual void OnConnectedToServer() override ;
    virtual void OnDisconnectedFromServer() override;
    virtual void OnAudioData(const char* attendee_id, const int16_t* audio_data, size_t num_frames,
                             uint32_t sample_rate, uint32_t bits_per_sample, uint32_t num_channels) override;
    virtual void OnAttendeeConnected(const char* attendee_id) override;
    virtual void OnAttendeeDisconnected(const char* attendee_id) override;
    virtual void OnActiveSpeakerChanged(const char* Speaker) override;
    virtual void OnDataPacketReceived(const char *JsonData, const char *attendee_id) override;
    virtual void OnLog(const char *log_message) override;
    

    bool InitializeConvaiClient();
    void CleanupConvaiClient();
    void SetupClientCallbacks();
    
    // Helper functions
    void OnUserStartedSpeaking(const char* attendee_id) const;
    void OnUserStoppedSpeaking(const char* attendee_id) const;
    void OnUserTranscript(const char* text, const char* attendee_id, bool final, const char* timestamp) const;
    void OnBotLLMStopped(const char* attendee_id) const;
    void OnBotStartedSpeaking(const char* attendee_id) const;
    void OnBotStoppedSpeaking(const char* attendee_id) const;
    void OnBotTranscript(const char* text, const char* attendee_id) const;
    void OnNarrativeSectionReceived(const FString& BT_Code, const FString& BT_Constants, const FString& ReceivedNarrativeSectionID) const;
    void OnEmotionReceived(const FString& ReceivedEmotionResponse, const FAnimationFrame& EmotionBlendshapesFrame, bool MultipleEmotions) const;
    void OnFaceDataReceived(const FAnimationSequence& VisemeAnimationSequence) const;
    void OnActionsReceived(TArray<FString>& Actions) const;
    void OnError(const FString& ErrorMessage) const;

};
