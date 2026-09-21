// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "Subsystems/SubsystemCollection.h"
#include "Net/OnlineBlueprintCallProxyBase.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "Containers/Ticker.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiConnectionSessionProxy.h"
#include "Core/ConvaiConnectionManager.h"
#include "Core/ConvaiReconnectCoordinator.h"
#include "ConvaiDefinitions.h"
#include "ConvaiReferenceAudioThread.h"

#include <convai/convai_client.h>
#include <atomic>

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
	bool IsThreadStarted() const { return Thread != nullptr; }

private:
    void InitializeThread();

    FConvaiConnectionParams ConnectionParams;
    FThreadSafeBool bShouldStop;
    FRunnableThread *Thread;
};

#if WITH_TESTS
/**
 * The seam between a connect attempt and the network, for tests only.
 *
 * A unit rig has no network and an engine scenario needs to force failures
 * without one. Both hook the single point where ConnectSession would start a
 * connection thread; everything else in ConnectSession still runs, so a test
 * grades the real path rather than a copy of it.
 */
struct CONVAI_API FConvaiConnectionTestSeam
{
    /** Installed by a unit rig. Returning true means "this attempt is mine":
     *  no thread is started and ConnectSession carries on as if one had been. */
    using FAttemptHook = TFunction<bool(const FConvaiConnectionParams&)>;

    static void Install(FAttemptHook Hook);
    static void Reset();

    /** Attempts seen since the last Reset, hook installed or not. */
    static int32 AttemptCount();

    /** The next N attempts fail as a real connect failure would, with no POST.
     *  Set by convai.tests.FailNextConnects. */
    static void FailNextConnects(int32 Count);
    static int32 PendingFailures();

    /** Stands in for the transport's send of the player's text. The hook's
     *  return value is what the transport would have said. */
    using FSendHook = TFunction<bool(const FString& Json)>;
    static void InstallSend(FSendHook Hook);

    // Called from ConnectSession only.
    static bool TakeAttempt(const FConvaiConnectionParams& Params);
    static bool TakeFailure();
    // Called from SendUserText only. False when no hook is installed.
    static bool TakeSend(const FString& Json, bool& bOutSent);

    /** Records the type of each configuration message instead of sending it:
     *  unit rigs have a client that was never initialized. */
    using FMessageTap = TFunction<void(const FString& Type)>;
    static void InstallMessageTap(FMessageTap Tap);
    // True when a tap took the message.
    static bool TakeMessage(const TCHAR* Type);
};
#endif // WITH_TESTS

/** How an idle warning should be answered. */
enum class EConvaiIdleWarningAction : uint8
{
    /** The player is still inside the AFK budget. */
    Renew,
    /** Out of budget, but a character is mid-line and the reprieve is unspent. */
    RenewForSpeech,
    /** Out of budget. Stop deferring and let the server close the session. */
    LetClose,
};

/**
 * The DLL's listener for one client, stamped with that client's Connect Epoch.
 *
 * The subsystem used to be the listener itself, which left it with no way to
 * tell which client a callback came from: a disconnect delivered by a client
 * the game had already abandoned killed the session that had replaced it. One
 * shim per client answers that question by construction — it carries the epoch
 * its client was created in, and the game thread drops anything older.
 */
class CONVAI_API FConvaiClientListenerShim : public convai::IConvaiClientListner
{
public:
    FConvaiClientListenerShim(UConvaiSubsystem *InSubsystem, uint64 InEpoch);

    virtual void OnConnectedToServer(const char *session_id, const char *char_session_id) override;
    virtual void OnDisconnectedFromServer() override;
    virtual void OnAudioData(const char *attendee_id, const int16_t *audio_data, size_t num_frames,
                             uint32_t sample_rate, uint32_t bits_per_sample, uint32_t num_channels) override;
    virtual void OnAttendeeConnected(const char *attendee_id) override;
    virtual void OnAttendeeDisconnected(const char *attendee_id) override;
    virtual void OnActiveSpeakerChanged(const char *Speaker) override;
    virtual void OnDataPacketReceived(const char *JsonData, const char *attendee_id) override;
    virtual void OnLog(const char *log_message) override;

    uint64 GetEpoch() const { return Epoch; }

private:
    TWeakObjectPtr<UConvaiSubsystem> Subsystem;
    uint64 Epoch = 0;
};

UCLASS(meta = (DisplayName = "Convai Subsystem"))
class CONVAI_API UConvaiSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    UConvaiSubsystem();

    /** Called when server connection state changes */
    UPROPERTY(BlueprintAssignable, Category = "Convai|Connection")
    FOnServerConnectionStateChangedSignature OnServerConnectionStateChangedEvent;
    
    UPROPERTY(BlueprintAssignable, Category = "Convai|Event")
    FOnUserIdleWarningSignature OnUserIdleWarning;

    /** Where the current reconnect chain is: the overlay's token and the log.
     *  A game reads the same story from On Server Connection State Changed. */
    const FConvaiReconnectStatus& GetLastReconnectStatus() const { return LastReconnectStatus; }

#if WITH_TESTS
    /** Test observers only: every status as it is reported, and every session
     *  that becomes ready. Neither is an API a game can bind. */
    TFunction<void(const FConvaiReconnectStatus&)> ReconnectStatusTestTap;
    TFunction<void(const UConvaiConnectionSessionProxy*)> SessionReadyTestTap;
#endif

    // Begin USubsystem
    virtual void Initialize(FSubsystemCollectionBase &Collection) override;
    virtual void Deinitialize() override;
    // End USubsystem

    // Get Android microphone permission
    static void GetAndroidMicPermission();

    /** Whether Reference Audio is flowing, and to how many Connections.
     *
     *  Exists because the answer is otherwise unobtainable from outside: a
     *  Connection whose echo canceller is receiving no far-end signal at all
     *  behaves identically to a healthy one and emits nothing to any log, which
     *  is why echo-cancellation faults have been diagnosed by ear. */
    struct FReferenceAudioStatus
    {
        bool bCapturing = false;
        int32 ClientCount = 0;

        // Whether it is flowing is only half the question — a feed arriving in
        // bursts, or missing most of what the game rendered, is also invisible
        // from outside. Zero everywhere when no capture exists.
        FConvaiReferenceAudioThread::FStats Feed;
    };
    FReferenceAudioStatus GetReferenceAudioStatus() const;

    /** The canceller's own counters, read from the client.
     *
     *  MicChunks against ReferenceChunks is the measurement that matters: the
     *  two streams must track each other over a window, because a canceller
     *  fed streams that drift apart cannot stay aligned however good it is.
     *  Everything zero and bValid false when no canceller is running. */
    struct FConvaiAECStats
    {
        bool bValid = false;
        int64 MicChunks = 0;
        int64 ReferenceChunks = 0;
        int64 ReferenceCalls = 0;
        int64 PublishedFrames = 0;
        int64 DroppedFrames = 0;
        int64 QueueDepth = 0;
        double LastRmsIn = 0.0;
        double LastRmsOut = 0.0;
    };
    FConvaiAECStats GetAECStats() const;

    /**
     * Connect a session to the Convai service
     * @param SessionProxy - The session proxy to connect
     * @param CharacterID - The ID of the character to connect to (ignored for player sessions)
     * @return True if connection was initiated successfully
     */
    bool ConnectSession(UConvaiConnectionSessionProxy *SessionProxy, const FString &CharacterID);

    /** While one of these is alive, a character ConnectSession is deferred to
     *  the next tick instead of running inside the broadcast that asked for it.
     *
     *  Restarting the session from a disconnect event is the wiring Convai's
     *  own guidance taught, so it is in shipped games. Run inline it re-enters
     *  the teardown it was called from — 59 levels deep on one customer's
     *  build, each level joining the previous connect on the game thread. One
     *  tick's delay costs nothing and makes the restart a plain new session. */
    struct CONVAI_API FDisconnectBroadcastScope
    {
        explicit FDisconnectBroadcastScope(UConvaiSubsystem* InSubsystem);
        ~FDisconnectBroadcastScope();

    private:
        TWeakObjectPtr<UConvaiSubsystem> Subsystem;
    };

    bool IsInsideDisconnectBroadcast() const { return DisconnectBroadcastDepth > 0; }

    /** Whether this session's character has said it is ready: bot-ready for
     *  the live client. Connected is only the transport; a character that is
     *  Connected and not ready cannot answer yet. */
    bool IsSessionReady(const UConvaiConnectionSessionProxy *SessionProxy) const;

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
    /** Sends the player's text to the current character. Typed before the
     *  character is ready, it is held and delivered once, at Ready. */
    void SendTextMessage(const UConvaiConnectionSessionProxy *SessionProxy, const FString &Message);
    void SendTriggerMessage(const UConvaiConnectionSessionProxy *SessionProxy, const FString &Trigger_Name, const FString &Trigger_Message) const;
    void UpdateTemplateKeys(const UConvaiConnectionSessionProxy *SessionProxy, TMap<FString, FString> Template_Keys) const;
    void UpdateDynamicInfo(const UConvaiConnectionSessionProxy *SessionProxy, const FString &Context_Text) const;
    void ToggleSTT(const UConvaiConnectionSessionProxy *SessionProxy, bool bMuted) const;

    /** Sends `force-user-stopped-speaking`, ending the user's turn now instead of waiting for
     *  the server VAD's silence timer. Push-to-talk release is the only caller that should
     *  need it: the button, not a pause in speech, is what says "I am done". */
    void ForceUserStoppedSpeaking(const UConvaiConnectionSessionProxy *SessionProxy) const;
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

    /** A connect attempt that never reached a session. Stamped with the epoch
     *  of the attempt, so a failure of an attempt already abandoned cannot end
     *  the session that replaced it. */
    void OnConnectAttemptFailed(uint64 Epoch);

    UConvaiConnectionManager *GetConnectionManager() const { return ConnectionManager; }

    UFUNCTION(BlueprintCallable, Category = "Convai|Connection")
    void InvalidateOrphanedConnection();
    
    UFUNCTION(BlueprintCallable, Category = "Convai|Connection")
    /** True when the renewal was actually sent; a renewal that went out also
     *  clears the pending-idle flag. */
    bool ResetIdleTimer(); 

    /** Records that the player did something that counts against AFK Time —
     *  spoke, or sent text. The character speaking must not call this. */
    void MarkPlayerActivity();

    /** Records that this disconnect was asked for, so it is not mistaken for a
     *  fault. Call before tearing a session down deliberately. */
    void MarkExplicitDisconnect();

    /** The one thing that schedules reconnect attempts. Public so a test can
     *  drive a whole drop chain; production code goes through Trigger/Wake. */
    FConvaiReconnectCoordinator& GetReconnectCoordinator() { return ReconnectCoordinator; }

    /** Player Activity: wakes a dormant session and fires a pending backoff. */
    void WakeReconnect();

    /** The game stopped this session: nothing the coordinator had pending for
     *  it may reopen it. A no-op for a proxy no drop chain belongs to. */
    void StopReconnect(const UConvaiConnectionSessionProxy *SessionProxy);

    /** A Start Session from inside the broadcast of an idle close, for the
     *  session that is going Dormant: held as intent rather than opening a
     *  session for an empty room, and honoured by the player's next activity.
     *  Warns once per Visit. */
    bool ShouldHoldStartForWake(const UConvaiConnectionSessionProxy *SessionProxy);

    /** A Start Session from the session a retry is pending for: fires that
     *  retry instead of tearing the chain down and opening a new Visit.
     *  @return true when adopted, and the caller must not start a session. */
    bool AdoptStartSession(const UConvaiConnectionSessionProxy *SessionProxy, const FString &CharacterID);

    /** The handshake gave up waiting for bot-ready on the live session, which
     *  stays up without it (reconnect off). */
    bool HasHandshakeGivenUp() const { return HandshakeGaveUpEpoch.load() != 0 && HandshakeGaveUpEpoch.load() == ConnectEpoch.load(); }

    /** Set while a Start Session deferred out of a disconnect broadcast runs:
     *  it is still the game re-running its wiring, not choosing a character. */
    bool bStartDeferredFromDisconnect = false;

    /** The game started another character while one's reconnect was pending
     *  or asleep: that chain ends, as Superseded, and its lease is released.
     *  @return true when there was a chain to end. */
    bool SupersedeReconnect(const FString &NewCharacterID);


    /** Starts a Visit: the player's presence, which spans every session one
     *  drop chain opens. Resets the AFK clock, the talking reprieve and the
     *  disconnect verdict. Called only for a start the game asked for. */
    void BeginVisit();

    /** Why the last session ended. Reset when a new session connects. */
    UFUNCTION(BlueprintPure, Category = "Convai|Connection")
    EC_DisconnectReason GetLastDisconnectReason() const { return LastDisconnectReason; }

    /** Why the last session ended, in more detail than the reason: Quota and
     *  Preflight are why nothing retried. Unknown when nothing more is known. */
    EC_DisconnectCause GetLastDisconnectCause() const { return LastDisconnectCause; }

    /** Whether an idle warning carrying RemainingSeconds should be answered with
     *  a reset, given how long the player has been away and the project's AFK
     *  budget.
     *
     *  Renew while the deadline the server is currently counting down to still
     *  falls inside the budget. Comparing ElapsedSeconds alone would renew one
     *  last time on the final warning before the budget ran out, overshooting by
     *  a whole warning interval; adding RemainingSeconds stops at the first
     *  server deadline at or after the budget instead.
     *
     *  Static and free of engine state so the decision is unit-testable without
     *  a live session. */
    static bool ShouldRenewIdleTimer(double ElapsedSeconds, int32 RemainingSeconds, float AfkTimeSeconds);

    /** The whole answer to one idle warning, given the budget and whether a
     *  character is still speaking. Kept free of engine state so every branch is
     *  reachable from a unit test rather than only from a ten-minute session. */
    static EConvaiIdleWarningAction DecideIdleWarningAction(
        double ElapsedSeconds, int32 RemainingSeconds, float AfkTimeSeconds,
        bool bCharacterTalking, bool bGraceAlreadySpent);
    
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
    friend struct FConvaiSubsystemTestAccessor;
    // The lease's orphan reuse is decided by the live client's epoch.
    friend class UConvaiConnectionManager;
    // The shim is the transport's half of this class: it forwards every
    // callback to the epoch-stamped entry points below.
    friend class FConvaiClientListenerShim;

    TUniquePtr<convai::ConvaiClient> ConvaiClient;
    TUniquePtr<FConvaiConnectionThread> ConnectionThread;
    TSharedPtr<FConvaiReferenceAudioThread> ReferenceAudioThread;
    FThreadSafeBool bIsConnected;
    FThreadSafeBool bStartedPublishingVideo;
    /** The epoch whose bot said it was ready, or 0. Written on the game thread
     *  only, after the epoch check: a bot-ready belongs to the client that
     *  delivered it, and a late one from a client already let go is nobody's.
     *  Read on the audio thread too. */
    std::atomic<uint64> ReadyEpoch{0};
    /** The epoch whose handshake timed out with reconnect off. That session
     *  stays up without ever being ready, so text goes straight out again. */
    std::atomic<uint64> HandshakeGaveUpEpoch{0};

    /** Text typed while the character was not ready yet, for the character
     *  whose session it was typed into. Game thread only. */
    struct FQueuedText
    {
        FString Text;
        double QueuedAt = 0.0;
        TWeakObjectPtr<const UConvaiConnectionSessionProxy> Owner;
    };
    TArray<FQueuedText> QueuedText;

    /** True only when the transport took the message. */
    bool SendUserText(const FString &Message);
    /** Delivers what was held for the current character. Held text older
     *  than the queue's lifetime is dropped unless bIgnoreAge. */
    void FlushQueuedText(bool bIgnoreAge);
    void DropQueuedText(const TCHAR *Why);
    /** Core tickers, not world timers: the handshake must run in a paused
     *  game and without a world, and a world timer silently did neither. */
    FTSTicker::FDelegateHandle ClientReadyRetryTicker;
    FTSTicker::FDelegateHandle ClientReadyTimeoutTicker;
    EC_ConnectionState CurrentConnectionState;  // Tracks the current connection state
    mutable FCriticalSection ConvaiClientMutex; // Protects ConvaiClient access from multiple threads
    mutable FCriticalSection SessionMutex;      // Protects session state
    mutable FCriticalSection AttendeeMutex;     // Protects attendee connection map

    // Map of connected attendees (AttendeeId -> true if connected)
    TSet<FString> ConnectedAttendees;

    // Attendees reported before OnConnectedToServer reached the game thread, announced
    // right after it. Game thread only: reset beside CleanupConvaiClient(), not inside
    // it, because OnConnectedToServer can call that from the transport thread.
    TArray<FString> PendingAttendees;

    // The bot sometimes never subscribes to the microphone track it joined
    // alongside, and then every frame the player sends is dropped server-side
    // with no error on any channel: text still works and the transport reports
    // healthy. Nothing below the plugin can see it (the server does not report
    // subscribers), so this is keyed on what the player actually experiences:
    // audible speech going into the microphone that the server has never once
    // acknowledged this session. Event-driven: SendAudio accumulates speech
    // until the threshold, then a one-shot timer waits for the player to go
    // quiet and decides. Counters are written from the audio and transport
    // threads, so they are atomics; the timer is game-thread only.
    mutable std::atomic<int64> MicWatchdogSpeechFrames{0};
    mutable std::atomic<double> MicWatchdogLastSpeechTime{0.0};
    mutable std::atomic<double> MicWatchdogNoiseFloor{0.0};
    mutable std::atomic<double> MicWatchdogBotSpeakingUntil{0.0};
    mutable std::atomic<bool> bMicWatchdogArmed{false};
    mutable std::atomic<double> MicWatchdogLastTextSendTime{0.0};
    std::atomic<bool> bMicWatchdogServerHeardUser{false};
    std::atomic<bool> bMicWatchdogReconnected{false};
    // AFK enforcement. Wall clock, not world time: a paused game with the player
    // out of the room is still the player being away. Only touched on the game
    // thread — the idle warning is marshalled there before it is read.
    FConvaiReconnectCoordinator ReconnectCoordinator;

    /** Work the coordinator scheduled, by the handle it was given. Kept here,
     *  not only inside the ticker, so Deinitialize can remove what has not
     *  fired and a unit row can run it without waiting out a real backoff. */
    struct FScheduledReconnect
    {
        FTSTicker::FDelegateHandle Ticker;
        TFunction<void()> Work;
    };
    TMap<int32, FScheduledReconnect> ScheduledReconnects;
    int32 NextScheduledReconnect = 0;
    void RunScheduledReconnect(int32 Handle);

    /** Set while the coordinator's own attempt is inside ConnectSession: that
     *  attempt belongs to a chain the game was already told is Reconnecting. */
    bool bStartingReconnectAttempt = false;

    /** Connecting for an attempt the game asked for; a retry stays Reconnecting. */
    void AnnounceConnecting();

    /** What a retry reopens: the character and proxy the drop chain began
     *  with, snapshotted before the teardown clears them. */
    TWeakObjectPtr<UConvaiConnectionSessionProxy> ReconnectProxy;
    FString ReconnectCharacterID;

    /** Wires the coordinator's hooks to the engine. */
    void InitializeReconnectCoordinator();

    /** Everything the reconnect decision needs, read on the game thread. */
    FConvaiReconnectContext SnapshotReconnectContext() const;

    /** Hands the coordinator the cause the session has earned so far, then
     *  asks it to decide. True when a retry is pending. */
    bool TriggerReconnect(EConvaiReconnectTrigger Trigger);

    /** Whether reconnect is on at all. A live session is only torn down to
     *  reconnect it when this holds; otherwise what happens is exactly what
     *  happened before reconnect existed. */
    bool IsReconnectEnabled() const;

    /** Ends a session whose transport is still up - a fatal server error -
     *  the way a drop ends one, so a retry sees no live client behind it. */
    void EndLiveSessionForReconnect(uint64 Epoch, EConvaiReconnectTrigger Trigger);

    /** Everything that follows a session ending, on the game thread, for the
     *  epoch that ended: the verdict, the handlers, the decision, the teardown. */
    void EndSession(uint64 Epoch, const TSet<FString> &DepartedAttendees, EConvaiReconnectTrigger Trigger);

    /** The epoch whose character left a transport that stayed up; the Bot
     *  Absence Window it opened belongs to that session and no later one. */
    uint64 BotAbsentEpoch = 0;
    void OnBotAbsenceExpired();

    /** Refuses a connect that cannot succeed - no character, no key - before
     *  anything is sent, and ends whatever chain asked for it. */
    bool FailPreflight(UConvaiConnectionSessionProxy *SessionProxy, const FString &CharacterID);

    /** Nesting of disconnect broadcasts on the game thread; see
     *  FDisconnectBroadcastScope. */
    int32 DisconnectBroadcastDepth = 0;

    /** The generation of the live client. Bumped whenever a client is created,
     *  torn down or abandoned, so every callback and every queued task can say
     *  which client it belongs to. Read from transport threads, so atomic.
     *
     *  Starts at 1 so that 0 can mean "no epoch" without ever naming a real
     *  one — ExplicitEpoch depends on that. */
    std::atomic<uint64> ConnectEpoch{1};

    /** The epoch whose teardown the game asked for, or 0 for none. Only that
     *  epoch's disconnect is Explicit — a player's Stop Session no longer
     *  relabels a character's drop, and a later drop of a different client
     *  does not inherit the verdict. */
    std::atomic<uint64> ExplicitEpoch{0};

    /** The listener of the live client, parked with that client when it is
     *  replaced so a late callback lands on a shim rather than on freed memory. */
    TSharedPtr<FConvaiClientListenerShim> ClientListenerShim;

    /** Bumps the epoch and returns the new one. Every teardown and every new
     *  client goes through here. */
    uint64 BumpConnectEpoch();

    /** False for anything a client this subsystem has already let go produced. */
    bool IsCurrentEpoch(uint64 Epoch) const;

    /** Announces the session end for a teardown the game asked for. The
     *  transport never reports that one: its listener is detached before the
     *  Disconnect, precisely so the DLL's own client-initiated callback cannot
     *  come back as a drop. */
    void SynthesizeExplicitDisconnect(const UConvaiConnectionSessionProxy *SessionProxy);

    /** The next-tick half of the above. */
    void BroadcastExplicitDisconnect(const UConvaiConnectionSessionProxy *SessionProxy);

    double LastPlayerActivityTime = 0.0;
    // Set when an idle warning goes unanswered, so the disconnect that follows
    // can be told apart from a fault. Cleared by activity, because the player
    // coming back means the server stops counting down and no idle disconnect
    // is coming after all.
    bool bIdleDisconnectPending = false;
    // One reprieve per AFK window, spent to let a character finish the line it
    // is already speaking. Never more: a character still talking after that is
    // talking to an empty chair, which is the thing AFK Time exists to end.
    bool bSpentTalkingGrace = false;
    EC_DisconnectReason LastDisconnectReason = EC_DisconnectReason::Unexpected;
    EC_DisconnectCause LastDisconnectCause = EC_DisconnectCause::Unknown;
    /** A renewal of the server's idle timer went out and the server has not
     *  acknowledged it yet. Written on the transport thread by the ack. */
    std::atomic<bool> bRenewalAwaitingAck{false};
    /** What the server's session-ending said about the coming close: 1 idle,
     *  0 something else, -1 nothing said. Consumed by the next verdict. */
    std::atomic<int8> PendingServerIdle{-1};
    bool bWarnedHeldStart = false;

    FConvaiReconnectStatus LastReconnectStatus;
    /** The live session's id, and the one a reconnect chain replaced: the
     *  per-attempt log names both. */
    FString LiveSessionID;
    FString ChainPreviousSessionID;

    // Voice wake while Dormant. The microphone keeps capturing with no session
    // to send to, and a player who starts talking again is the player back.
    // Fed on the audio thread, so atomics; acted on on the game thread.
    mutable std::atomic<double> MicWakeFloor{-1.0};
    mutable std::atomic<double> MicWakeSpeechSeconds{0.0};
    mutable std::atomic<double> MicWakeQuietSeconds{0.0};
    mutable std::atomic<bool> bMicWakePosted{false};
    /** No voice wake before this, after an idle close: the room that just went
     *  quiet is the room most likely to make one noise and then nothing. */
    double IdleWakeHoldUntil = 0.0;
    void FeedMicWake(const int16_t *AudioData, size_t NumFrames) const;
    void OnMicWakeEnergy();
    /** What the live session has told us about how it will end, before it
     *  does - usage-limit-reached arrives ahead of the close. Written on the
     *  transport thread, consumed by the next decision. */
    std::atomic<EC_DisconnectCause> PendingDisconnectCause{EC_DisconnectCause::Unknown};
    void HandleUserIdleWarning(int32 RemainingSeconds);
    bool IsAnyChatbotTalking();
    /** Decides the verdict for the epoch that dropped, before any handler
     *  runs. Explicit only when the game tore THAT transport down. */
    void ResolveDisconnectReason(uint64 Epoch);

    /** A ticker, not the world's timer: with no world the check used to be
     *  dropped without a word. */
    FTSTicker::FDelegateHandle MicWatchdogTicker;
    /** One MicUnheard reconnect per Visit: the second time, it is not the
     *  session that is deaf. */
    bool bMicUnheardSpentThisVisit = false;
    void MicWatchdogScheduleCheck(double DelaySeconds, uint64 ArmedEpoch);
    void MicWatchdogCheck(uint64 ArmedEpoch);
    void MicWatchdogReset();
    void MicWatchdogServerHeardUser();

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

    /** Warn-once for the displaced-producer refusal in SendAudio. */
    mutable bool bWarnedDisplacedAudioProducer = false;

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

    void StartClientReadyHandshake(uint64 Epoch);
    void StopClientReadyHandshake();
    void SendClientReadyMessage() const;
    void OnClientReadyTimeout(uint64 Epoch);
    /** bot-ready for this epoch, on the game thread: the only success signal. */
    void HandleBotReady(uint64 Epoch);

    // Transport callbacks, each stamped with the Connect Epoch of the client
    // that produced it. Anything from an older epoch belongs to a client this
    // subsystem has already let go, and acting on it is how a stale disconnect
    // used to kill the session that replaced it.
    void OnTransportConnected(uint64 Epoch, const FString &SessionID, const FString &CharSessionID);
    void OnTransportDisconnected(uint64 Epoch);
    void OnTransportAudioData(uint64 Epoch, const char *attendee_id, const int16_t *audio_data,
                              size_t num_frames, uint32_t sample_rate, uint32_t bits_per_sample,
                              uint32_t num_channels);
    void OnTransportAttendeeConnected(uint64 Epoch, const FString &AttendeeId);
    void OnTransportAttendeeDisconnected(uint64 Epoch, const FString &AttendeeId);
    void OnTransportActiveSpeakerChanged(uint64 Epoch, const FString &Speaker);
    void OnTransportDataPacket(uint64 Epoch, const FString &JsonData, const FString &AttendeeId);
    void OnTransportLog(const FString &LogMessage);

    bool InitializeConvaiClient();
    void CleanupConvaiClient();

    /** Hands the live client, its listener and its connection thread to the
     *  reaper. Returns immediately: nothing here waits on the network. */
    void AbandonLiveClient();
    void SetupClientCallbacks();

    // Helper function to broadcast server connection state changes on game thread
    void BroadcastServerConnectionStateChanged(EC_ConnectionState State);
    void NotifyAttendeeConnected(const FString &AttendeeId) const;

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
    // Not static. It was, which is why it could only ever log -- a static member
    // cannot see CurrentCharacterSession, so no error the server sent had a route
    // to the game (FINDINGS F9).
    void OnError(const FString &ErrorMessage) const;

    /** Routes a server-reported error to both sessions' connection interfaces. */
    void OnServerError(const FString &ErrorMessage, bool bFatal) const;
};
