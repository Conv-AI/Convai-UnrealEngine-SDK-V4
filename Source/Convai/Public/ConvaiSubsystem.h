// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "Subsystems/SubsystemCollection.h"
#include "Net/OnlineBlueprintCallProxyBase.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiConnectionSessionProxy.h"
#include "Core/ConvaiConnectionManager.h"
#include "ConvaiDefinitions.h"
#include "ConvaiReferenceAudioThread.h"

#include <convai/convai_client.h>

#include "ConvaiSubsystem.generated.h"

// Forward declarations
namespace convai
{
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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnUserIdleWarningSignature, const int32&, RemainingSeconds);

// Connection thread class
class FConvaiConnectionThread : public FRunnable
{
public:
    FConvaiConnectionThread(const FConvaiConnectionParams &ConnectionParams);
    FConvaiConnectionThread(FConvaiConnectionParams &&ConnectionParams);
    virtual ~FConvaiConnectionThread() override;

    // FRunnable interface
    virtual bool Init() override { return true; }
    virtual uint32 Run() override;
    virtual void Stop() override { bShouldStop = true; }
    virtual void Exit() override {}

private:
    void InitializeThread();

    FConvaiConnectionParams ConnectionParams;
    FThreadSafeBool bShouldStop;
    FRunnableThread *Thread;
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
    
    UPROPERTY(BlueprintAssignable, Category = "Convai|Event")
    FOnUserIdleWarningSignature OnUserIdleWarning;

    // Begin USubsystem
    virtual void Initialize(FSubsystemCollectionBase &Collection) override;
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
    bool ConnectSession(UConvaiConnectionSessionProxy *SessionProxy, const FString &CharacterID);

    /**
     * Disconnect a session from the Convai service
     * @param SessionProxy - The session proxy to disconnect
     */
    void DisconnectSession(const UConvaiConnectionSessionProxy *SessionProxy);

    /**
     * Send audio data through a session
     * @param SessionProxy - The session proxy to send audio through
     * @param AudioData - The audio data to send
     * @param NumFrames - The number of frames in the audio data
     * @return The number of bytes sent, or -1 on failure
     */
    int32 SendAudio(const UConvaiConnectionSessionProxy *SessionProxy, const int16_t *AudioData, size_t NumFrames) const;
    void SendImage(const UConvaiConnectionSessionProxy *SessionProxy, uint32 Width, uint32 Height, TArray<uint8> &Data);
    void StopVideoPublishing(const UConvaiConnectionSessionProxy *SessionProxy);
    void SendTextMessage(const UConvaiConnectionSessionProxy *SessionProxy, const FString &Message) const;
    void SendTriggerMessage(const UConvaiConnectionSessionProxy *SessionProxy, const FString &Trigger_Name, const FString &Trigger_Message) const;
    void UpdateTemplateKeys(const UConvaiConnectionSessionProxy *SessionProxy, TMap<FString, FString> Template_Keys) const;
    void UpdateDynamicInfo(const UConvaiConnectionSessionProxy *SessionProxy, const FString &Context_Text) const;
    void ToggleSTT(const UConvaiConnectionSessionProxy *SessionProxy, bool bMuted) const;
    /** Sends a `context-update` RTVI message. When OptionalAttention is non-null, its name
     *  is folded into the same message under `current_attention_object` (empty string clears
     *  attention server-side). Mode/ShouldRespond/Text follow the existing semantics. */
    void UpdateContext(const UConvaiConnectionSessionProxy *SessionProxy, const FString &Text,
                       EC_ContextUpdateMode Mode, EC_RunLLMOption ShouldRespond,
                       const FConvaiObjectEntry *OptionalAttention = nullptr) const;

    /** Sends an `update-scene-metadata` RTVI message to refresh descriptive scene context.
     *  These entries do NOT expand the allowed action target set — that is fixed by
     *  `action_config.objects` at /connect time. */
    void UpdateSceneMetadata(const UConvaiConnectionSessionProxy *SessionProxy, const TArray<FConvaiObjectEntry> &SceneObjects) const;

    static void OnConnectionFailed();

    UConvaiConnectionManager *GetConnectionManager() const { return ConnectionManager; }

    UFUNCTION(BlueprintCallable, Category = "Convai|Connection")
    void InvalidateOrphanedConnection();
    
    UFUNCTION(BlueprintCallable, Category = "Convai|Connection")
    void ResetIdleTimer(); 
    
    void RegisterChatbotComponent(class UConvaiChatbotComponent *ChatbotComponent);
    void UnregisterChatbotComponent(class UConvaiChatbotComponent *ChatbotComponent);
    TArray<class UConvaiChatbotComponent *> GetAllChatbotComponents() const;
    void RegisterPlayerComponent(class UConvaiPlayerComponent *PlayerComponent);
    void UnregisterPlayerComponent(class UConvaiPlayerComponent *PlayerComponent);
    TArray<class UConvaiPlayerComponent *> GetAllPlayerComponents() const;

    /**
     * UConvaiObjectComponent registry. The shared poll clock that drives
     * evaluation lives on UConvaiContextSubsystem; registration starts it and the
     * last unregistration stops it. Each poll invokes PollObjectComponents()
     * below, which calls EvaluateTrackedProperties() on every registered object
     * in the same tick instant so the chatbot's ContextDebounceWindow can
     * coalesce all property changes for a frame into one batch.
     */
    void RegisterObjectComponent(class UConvaiObjectComponent *ObjectComponent);
    void UnregisterObjectComponent(class UConvaiObjectComponent *ObjectComponent);
    TArray<class UConvaiObjectComponent *> GetAllObjectComponents() const;

    /**
     * One logical object group produced by BuildObjectGroups(): the set of
     * registered object components that share a name. With merging OFF every
     * group has exactly one member (names are already unique via suffixing);
     * with merging ON, same-named components collapse into a single group.
     */
    struct FConvaiObjectGroup
    {
        /** Canonical name shared by all members (the first member's name). */
        FString Name;
        /** Members in registry order; all entries are valid at build time. */
        TArray<class UConvaiObjectComponent *> Members;
        /** Mean of the members' resolved world locations. */
        FVector Centroid = FVector::ZeroVector;
        /** First non-empty member Description in registry order (may be empty). */
        FString Description;
    };

    /**
     * Build logical object groups from the live registry: components are grouped by
     * their (case-insensitive, non-empty) ObjectEntry.Name. Because registration
     * gives every logical object a unique final name — and makes all members of a
     * merged set share that one name — grouping by name yields exactly the logical
     * objects (a merged set is one multi-member group; every other object is a group
     * of one). Centroid is the mean of member locations; Description is the first
     * non-empty member Description in registry order. Invalid and empty-named
     * components are skipped. Game-thread only (resolves component caches on members).
     */
    void BuildObjectGroups(TArray<FConvaiObjectGroup> &OutGroups) const;

    /**
     * Gather every registered component sharing Member's name (case-insensitive),
     * always including Member itself, in registry order. For a non-merged object
     * (unique name) this returns just {Member}; for a member of a merged set it
     * returns the whole set. Used by gaze to expand one hit into the highlight set.
     */
    void GetObjectGroupMembers(class UConvaiObjectComponent *Member,
        TArray<class UConvaiObjectComponent *> &OutMembers) const;

    /**
     * One poll pass over the registered object components: prunes destroyed
     * entries, then calls EvaluateTrackedProperties() on each live one. Returns
     * true while any remain (so the caller's clock keeps ticking), false once the
     * registry has emptied. Driven by UConvaiContextSubsystem's poll clock.
     */
    bool PollObjectComponents();

    /**
     * Get the current server connection state
     * @return The current server connection state
     */
    UFUNCTION(BlueprintCallable, Category = "Convai|Connection")
    EC_ConnectionState GetServerConnectionState() const;

    /**
     * Get the connection state for a specific session proxy
     * @param SessionProxy - The session proxy to check
     * @return The connection state for the session
     */
    EC_ConnectionState GetSessionConnectionState(const UConvaiConnectionSessionProxy *SessionProxy) const;

private:
    TUniquePtr<convai::ConvaiClient> ConvaiClient;
    TUniquePtr<FConvaiConnectionThread> ConnectionThread;
    TSharedPtr<FConvaiReferenceAudioThread> ReferenceAudioThread;
    FThreadSafeBool bIsConnected;
    FThreadSafeBool bStartedPublishingVideo;
    FThreadSafeBool bIsBotReady;
    FTimerHandle ClientReadyRetryTimerHandle;
    FTimerHandle ClientReadyTimeoutTimerHandle;
    EC_ConnectionState CurrentConnectionState;  // Tracks the current connection state
    mutable FCriticalSection ConvaiClientMutex; // Protects ConvaiClient access from multiple threads
    mutable FCriticalSection SessionMutex;      // Protects session state
    mutable FCriticalSection AttendeeMutex;     // Protects attendee connection map

    // Map of connected attendees (AttendeeId -> true if connected)
    TSet<FString> ConnectedAttendees;

#if ConvaiDebugMode
    // Timestamp tracking for latency measurement
    double LastUserStoppedSpeakingTimestamp = -1;

    // Latency statistics tracking
    TArray<double> LatencySamples;
    void RecordLatency(double LatencyMs);
    void PrintLatencyStatistics() const;
    void ResetLatencyStatistics();
#endif

    UPROPERTY()
    UConvaiConnectionSessionProxy *CurrentCharacterSession;

    UPROPERTY()
    UConvaiConnectionSessionProxy *CurrentPlayerSession;

    UPROPERTY()
    UConvaiConnectionManager *ConnectionManager;

    UPROPERTY()
    TArray<class UConvaiChatbotComponent *> RegisteredChatbotComponents;

    UPROPERTY()
    TArray<class UConvaiPlayerComponent *> RegisteredPlayerComponents;

    UPROPERTY()
    TArray<class UConvaiObjectComponent *> RegisteredObjectComponents;

    /** Resolve the sibling UConvaiContextSubsystem, which owns the poll clock
     *  that drives PollObjectComponents(). Null if unavailable. */
    class UConvaiContextSubsystem* GetContextSubsystem() const;

    void StartClientReadyHandshake();
    void StopClientReadyHandshake();
    void SendClientReadyMessage() const;
    void OnClientReadyTimeout();

    // Client callbacks
    virtual void OnConnectedToServer(const char *session_id, const char *char_session_id) override;
    virtual void OnDisconnectedFromServer() override;
    virtual void OnAudioData(const char *attendee_id, const int16_t *audio_data, size_t num_frames,
                             uint32_t sample_rate, uint32_t bits_per_sample, uint32_t num_channels) override;
    virtual void OnAttendeeConnected(const char *attendee_id) override;
    virtual void OnAttendeeDisconnected(const char *attendee_id) override;
    virtual void OnActiveSpeakerChanged(const char *Speaker) override;
    virtual void OnDataPacketReceived(const char *JsonData, const char *attendee_id) override;
    virtual void OnLog(const char *log_message) override;

    bool InitializeConvaiClient();
    void CleanupConvaiClient();
    void SetupClientCallbacks();

    // Helper function to broadcast server connection state changes on game thread
    void BroadcastServerConnectionStateChanged(EC_ConnectionState State);

    // Helper functions
    void OnUserStartedSpeaking(const char *attendee_id) const;
    void OnUserStoppedSpeaking(const char *attendee_id) const;
    void OnUserTranscript(const char *text, const char *attendee_id, bool final, const char *timestamp) const;
    void OnBotLLMStarted(const char *attendee_id, const FString &ResponseId) const;
    void OnBotLLMStopped(const char *attendee_id) const;
    void OnBotStartedSpeaking(const char *attendee_id, const FString &ResponseId) const;
    void OnBotStoppedSpeaking(const char *attendee_id, const FString &ResponseId) const;
    void OnBotTurnCompleted(const char *attendee_id, const FString &ResponseId,
                            bool bWasInterrupted, bool bWasAborted, const FString &ErrorReason) const;
    void OnBotTranscript(const char *text, const char *attendee_id) const;
    void OnNarrativeSectionReceived(const FString &BT_Code, const FString &BT_Constants, const FString &ReceivedNarrativeSectionID) const;
    void OnEmotionReceived(const FString &ReceivedEmotionResponse, const FAnimationFrame &EmotionBlendshapesFrame, bool MultipleEmotions) const;
    void OnFaceDataReceived(const FAnimationSequence &VisemeAnimationSequence) const;
    /** Resolve action targets against the active chatbot's environment and forward to it. */
    void DispatchActionSequence(TArray<FConvaiResultAction> &Actions) const;
    static void OnError(const FString &ErrorMessage);
};
