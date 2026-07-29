// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "RingBuffer.h"
#include "ConvaiConversationComponent.h"
#include "ConvaiDefinitions.h"
#include "DSP/BufferVectorOperations.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiAudioProcessingInterface.h"
#include "Engine/EngineTypes.h"
#include "ConvaiPlayerComponent.generated.h"

#define TIME_BETWEEN_VOICE_UPDATES_SECS 0.01

class IConvaiAudioCaptureInterface;
DECLARE_LOG_CATEGORY_EXTERN(ConvaiPlayerLog, Log, All);

class UConvaiAudioCaptureComponent;
class UConvaiConnectionSessionProxy;
class IConvaiAudioProcessingInterface;
class UConvaiObjectComponent;
class AConvaiGazeHighlightActor;
class UConvaiGazeCursorWidget;
class UMaterialInterface;
class UConvaiPlayerComponent;

// Gaze event payloads — emitted by UConvaiPlayerComponent so projects can react to highlight
// transitions (immediate) and attention escalation (after the sustained-gaze threshold).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FConvaiPlayerGazeEvent,
	UConvaiPlayerComponent*, PlayerComponent,
	UConvaiObjectComponent*, ObjectComponent);

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
	 *   End User ID used for long term memory (LTM)
	 */
	UPROPERTY(EditAnywhere, Category = "Convai", Replicated, BlueprintSetter = SetEndUserID)
	FString EndUserID;

	/**
	 *    Sets a new End User ID
	 */
	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void SetEndUserID(FString NewEndUserID);

	UFUNCTION(Server, Reliable, Category = "Convai|Network")
	void SetEndUserIDServer(const FString& NewEndUserID);

	/**
	 *   End User Metadata as a JSON string for long term memory (LTM)
	 */
	UPROPERTY(EditAnywhere, Category = "Convai", Replicated, BlueprintSetter = SetEndUserMetadata)
	FString EndUserMetadata;

	/**
	 *    Sets the End User Metadata
	 */
	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void SetEndUserMetadata(FString NewEndUserMetadata);

	UFUNCTION(Server, Reliable, Category = "Convai|Network")
	void SetEndUserMetadataServer(const FString& NewEndUserMetadata);

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

	/** Returns true while the underlying session proxy is in EC_ConnectionState::Connected. */
	UFUNCTION(BlueprintPure, Category = "Convai|Session")
	bool IsPlayerConnected() const;

	UFUNCTION(BlueprintCallable, Category = "Convai|Session")
	void SendText(UConvaiConversationComponent* ChatbotComponent, FString Text) const;

#ifdef WITH_CONVAI_TESTS
#if WITH_CONVAI_TESTS
	/** Test-only accessor for the active session proxy, used by the in-plugin test harness to pump pre-recorded PCM directly. */
	UConvaiConnectionSessionProxy* GetSessionProxyForTesting() const { return SessionProxyInstance; }
#endif
#endif

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

	/**
	 * True while this player is speaking. The server's voice-activity events are the
	 * primary signal; non-empty partial/final transcriptions repair missing edges.
	 * A short real-time stop grace absorbs premature or duplicated stop packets.
	 * Local session state — not replicated.
	 */
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai", meta = (DisplayName = "Is Speaking"))
	bool IsSpeaking() const
	{
		return bIsSpeaking;
	}

public:
	/** Whether to automatically initialize the session in BeginPlay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Session")
	bool bAutoInitializeSession;

	// ── Gaze Attention ──────────────────────────────────────────────────
	//
	// When enabled, each tick this component traces forward from the player camera. Any
	// actor with a UConvaiObjectComponent under the cursor is visually highlighted, and
	// after `GazeAttentionDelay` seconds of sustained gaze becomes the chatbot's "object
	// in attention". After `GazeAttentionLossDelay` seconds of looking away (or at
	// nothing), the slot is released. Gaze cannot overwrite an attention slot that a
	// Blueprint/C++ caller explicitly set via UConvaiChatbotComponent::SetObjectInAttention.

	/** Master switch. All Gaze Attention params below are greyed out until this is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention")
	bool bEnableGazeAttention = false;

	/**
	 * Forwarded to the chatbot when this player's gaze promotes a Convai object to "in
	 * attention". Tells the chatbot whether to immediately reply (Always), let the
	 * server decide (Auto), or just update state silently (Never).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention",
		meta = (EditCondition = "bEnableGazeAttention"))
	EC_RunLLMOption GazeShouldRespond = EC_RunLLMOption::Never;

	/**
	 * WHEN a gaze-attention promotion reaches the chatbot (only relevant when
	 * Gaze Should Respond is Auto/Always):
	 *   - Send Normally: batched into the next scheduled send.
	 *   - Wait Until Conversation Is Idle: held until nobody is talking, so a
	 *     glance can't make the character interrupt itself mid-sentence. If the
	 *     player looks away before it lands, the cue is cancelled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention",
		meta = (EditCondition = "bEnableGazeAttention"))
	EConvaiContextDelivery GazeDelivery = EConvaiContextDelivery::SendNormally;

	/**
	 * Optional narrative-event text sent alongside the attention promotion (e.g.
	 * "Player is looking at the vase"). Leave empty for a silent attention switch.
	 * Has effect only when GazeShouldRespond is Auto/Always.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention",
		meta = (EditCondition = "bEnableGazeAttention", MultiLine = true))
	FString GazeAttentionText;

	/** Sustained-look duration (seconds) before a highlighted object is promoted to "in attention". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention",
		meta = (EditCondition = "bEnableGazeAttention", ClampMin = "0.0", Units = "Seconds"))
	float GazeAttentionDelay = 1.0f;

	/** Look-away duration (seconds) before the current attention object is cleared. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention",
		meta = (EditCondition = "bEnableGazeAttention", ClampMin = "0.0", Units = "Seconds"))
	float GazeAttentionLossDelay = 5.f;

	/** Max trace distance from the camera, in world units (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention", AdvancedDisplay,
		meta = (EditCondition = "bEnableGazeAttention", ClampMin = "1.0", Units = "Centimeters"))
	float GazeMaxDistance = 5000.0f;

	/**
	 * Half-angle (degrees) of a soft "gaze cone" that fires only when the strict line trace
	 * doesn't engage anything. 0 = no fallback (current default). Higher = more forgiving;
	 * the player can engage a Convai object that's near the cursor without aiming directly at
	 * it. The fallback iterates every Convai object component the world knows about, skips
	 * any that are out of range / behind the camera / outside the cone, and picks the
	 * best-aligned (highest dot product) candidate as the gaze target.
	 *
	 * Cheaper than a sphere trace: ~tens of vector ops per object, no physics scene query.
	 * Distance-independent — a far-away object needs the same on-screen tolerance as a near
	 * one, which matches "you're roughly looking at it" intuition better than a sphere
	 * radius would.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention",
		meta = (EditCondition = "bEnableGazeAttention", ClampMin = "0.0", ClampMax = "90.0", Units = "Degrees"))
	float GazeAngleTolerance = 5.0f;

	/** Collision channel used by the gaze line trace. Defaults to Visibility. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention", AdvancedDisplay,
		meta = (EditCondition = "bEnableGazeAttention"))
	TEnumAsByte<ECollisionChannel> GazeTraceChannel = ECC_Visibility;

	// ── Highlight (silhouette) ───────────────────────────────────────────

	/**
	 * Highlight actor spawned over the gazed-at Convai object. Defaults to the plugin's
	 * AConvaiGazeHighlightActor (silhouette via UMeshComponent::SetOverlayMaterial on
	 * UE 5.3+, wireframe-box fallback on 5.0-5.2). Subclass it for a fully custom visual.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Highlight",
		meta = (EditCondition = "bEnableGazeAttention"))
	TSubclassOf<AConvaiGazeHighlightActor> GazeHighlightActorClass;

	/** Tint of the silhouette / wireframe. White or pale yellow read well over most meshes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Highlight",
		meta = (EditCondition = "bEnableGazeAttention"))
	FLinearColor GazeHighlightColor = FLinearColor(1.0f, 0.9f, 0.2f, 1.0f);

	/**
	 * Overlay material applied via UMeshComponent::SetOverlayMaterial. Leave unset to use
	 * the plugin's default (AConvaiGazeHighlightActor::OverlayMaterial — a Fresnel-rim
	 * silhouette material shipped at /ConvAI/Highlights/M_ConvaiGazeOverlay; falls back
	 * to /Engine/EngineMaterials/EmissiveMeshMaterial only if the plugin asset is missing
	 * from a cooked build). Swap in any UMaterialInterface that exposes an
	 * "EmissiveColor" vector parameter (and optionally "EmissiveIntensity" scalar).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Highlight",
		meta = (EditCondition = "bEnableGazeAttention"))
	TSoftObjectPtr<UMaterialInterface> GazeOverlayMaterial;

	/** Multiplier on GazeHighlightColor before it goes into the EmissiveColor parameter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Highlight", AdvancedDisplay,
		meta = (EditCondition = "bEnableGazeAttention", ClampMin = "0.0"))
	float GazeHighlightEmissiveIntensity = 2.5f;

	// ── Cursor (center-of-screen reticle) ────────────────────────────────

	/** Show a small dot at screen center while gaze tracking is on. Fades by gaze state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Cursor",
		meta = (EditCondition = "bEnableGazeAttention"))
	bool bShowGazeCursor = true;

	/**
	 * When true, the cursor stays in its Active state (ActiveColor) even when gaze isn't on a
	 * Convai object — i.e. it doesn't fade out. Useful for projects that want a persistent
	 * reticle the player can always see, with the active/idle transition repurposed for "this
	 * is a Convai object I can interact with" feedback rather than "I'm looking at one now."
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Cursor",
		meta = (EditCondition = "bEnableGazeAttention && bShowGazeCursor"))
	bool bAlwaysShowGazeCursor = false;

	/** Widget class spawned to draw the cursor. Defaults to plugin's UConvaiGazeCursorWidget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Cursor",
		meta = (EditCondition = "bEnableGazeAttention && bShowGazeCursor"))
	TSubclassOf<UConvaiGazeCursorWidget> GazeCursorWidgetClass;

	/** Cursor color while gaze is on a Convai object. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Cursor",
		meta = (EditCondition = "bEnableGazeAttention && bShowGazeCursor"))
	FLinearColor GazeCursorActiveColor = FLinearColor::White;

	/** Cursor color while gaze is on nothing (default alpha 0 = fully fades out). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Cursor", AdvancedDisplay,
		meta = (EditCondition = "bEnableGazeAttention && bShowGazeCursor"))
	FLinearColor GazeCursorIdleColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.0f);

	/** Edge length of the cursor square, in Slate units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Cursor", AdvancedDisplay,
		meta = (EditCondition = "bEnableGazeAttention && bShowGazeCursor", ClampMin = "1.0"))
	float GazeCursorDotSize = 6.0f;

	/** Seconds to fade the cursor from idle → active when gaze first hits a Convai object. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Cursor", AdvancedDisplay,
		meta = (EditCondition = "bEnableGazeAttention && bShowGazeCursor", ClampMin = "0.0", Units = "Seconds"))
	float GazeCursorFadeInTime = 0.1f;

	/** Seconds to fade the cursor from active → idle when gaze leaves a Convai object. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze Attention|Cursor", AdvancedDisplay,
		meta = (EditCondition = "bEnableGazeAttention && bShowGazeCursor", ClampMin = "0.0", Units = "Seconds"))
	float GazeCursorFadeOutTime = 0.25f;

	// ── Events ───────────────────────────────────────────────────────────

	/** Fired the instant the player's gaze enters a Convai object (before any attention threshold). */
	UPROPERTY(BlueprintAssignable, Category = "Convai|Gaze Attention|Events")
	FConvaiPlayerGazeEvent OnGazeBegin;

	/** Fired the instant the player's gaze leaves a Convai object (regardless of attention state). */
	UPROPERTY(BlueprintAssignable, Category = "Convai|Gaze Attention|Events")
	FConvaiPlayerGazeEvent OnGazeEnd;

	/** Fired when a gazed-at object is promoted to "in attention" after the sustained-gaze period. */
	UPROPERTY(BlueprintAssignable, Category = "Convai|Gaze Attention|Events")
	FConvaiPlayerGazeEvent OnAttentionGained;

	/** Fired when the in-attention slot is released (loss timer, gaze swap, or target destroyed). */
	UPROPERTY(BlueprintAssignable, Category = "Convai|Gaze Attention|Events")
	FConvaiPlayerGazeEvent OnAttentionLost;

private:
	void TickGazeAttention(float DeltaTime);
	void SpawnHighlightFor(const TArray<AActor*>& TargetActors, USceneComponent* TargetComponent);
	void DestroyActiveHighlight();
	void PromoteToAttention(AActor* NewActor, UPrimitiveComponent* NewPrimitive);
	void ReleaseCurrentAttention();
	void EnsureGazeCursorWidget();
	void DestroyGazeCursorWidget();

	UFUNCTION()
	void HandleAttentionTargetDestroyed(AActor* DestroyedActor);

	// Returns ObjComps on Actor whose scope matches HitComponent:
	//   - whole-actor scope (no ComponentName filter)              → always matches
	//   - component-scoped, resolved == HitComponent (or ancestor) → matches
	//   - component-scoped, resolved != HitComponent               → skipped
	// Also returns whether ANY matching ObjComp is component-scoped (so the caller knows
	// whether to scope the highlight to HitComponent vs the whole actor).
	void GatherMatchingObjects(AActor* Actor, UPrimitiveComponent* HitComponent,
		TArray<UConvaiObjectComponent*>& OutMatches, bool& bOutAnyComponentScoped) const;

	// Dot-product gaze fallback. Walks every UConvaiObjectComponent in the subsystem-wide pool,
	// applies cheap skip rules (distance, hemisphere, cone), and picks the best-aligned
	// candidate. On a winner, writes the synthesized hit into OutHitActor/OutHitPrim so the
	// rest of the pipeline treats it like a real line-trace hit.
	void FallbackEngageViaDotProduct(const FVector& ViewLoc, const FVector& ViewDir,
		AActor*& OutHitActor, UPrimitiveComponent*& OutHitPrim) const;

	// Gaze target as a (Actor, Primitive) pair. The set of matching UConvaiObjectComponents
	// is computed fresh on each transition via GatherMatchingObjects — keeps state simple
	// and lets one tick handle multiple ObjComps on the same actor (e.g. a door's "Handle"
	// and "Frame" object components both firing when their respective primitive is hit).
	TWeakObjectPtr<AActor> CurrentlyGazedActor;
	TWeakObjectPtr<UPrimitiveComponent> CurrentlyGazedPrimitive;
	TWeakObjectPtr<AActor> CurrentAttentionActor;
	TWeakObjectPtr<UPrimitiveComponent> CurrentAttentionPrimitive;

	/**
	 * Snapshot of the ObjectEntries that were promoted to attention. Captured in
	 * PromoteToAttention, cleared in ReleaseCurrentAttention. Used by
	 * HandleAttentionTargetDestroyed to fan out TryClearObjectInAttentionFromGaze to every
	 * chatbot in the subsystem, even though the original UConvaiObjectComponent(s) are dead
	 * along with the target actor — chatbots can match by Name and clear silently if they
	 * don't currently hold attention on this entry.
	 */
	TArray<FConvaiObjectEntry> CurrentAttentionEntries;
	TWeakObjectPtr<AConvaiGazeHighlightActor> ActiveHighlight;
	UPROPERTY(Transient)
	UConvaiGazeCursorWidget* ActiveGazeCursor = nullptr;
	float GazeAccumulator = 0.f;
	float NoGazeAccumulator = 0.f;

private:
	UPROPERTY()
	USoundWaveProcedural* VoiceCaptureSoundWaveProcedural;

	// Buffer used with recording
	TArray<uint8> VoiceCaptureBuffer;

	// Buffer used with streaming
	TRingBuffer<uint8> VoiceCaptureRingBuffer;

	UPROPERTY()
	UConvaiAudioCaptureComponent* AudioCaptureComponent = nullptr;

	UPROPERTY()
	TScriptInterface<IConvaiAudioCaptureInterface> ConvaiAudioCaptureComponent;
	
	UFUNCTION(BlueprintCallable, Category = "Convai|AudioCapture")
	bool SetAudioCaptureComponent(UActorComponent* InAudioCaptureComponent);
	
	IConvaiAudioCaptureInterface* FindFirstAudioCaptureComponent();
		
	/** The session proxy instance */
	UPROPERTY()
	UConvaiConnectionSessionProxy* SessionProxyInstance;

	void UpdateVoiceCapture(float DeltaTime);
	void StartVoiceChunkCapture(float ExpectedRecordingTime = 0.01) const;
	void StopVoiceChunkCapture();
	void ReadRecordedBuffer(Audio::AlignedFloatBuffer& RecordedBuffer, float& OutNumChannels, float& OutSampleRate) const;

	void StartAudioCaptureComponent();
	void StopAudioCaptureComponent();

	void EnsureMicrophonePermission();
	void ScheduleMicOpenRetry();
	void CheckMicCaptureHealth();

	FTimerHandle MicOpenRetryTimer;
	int32 MicOpenRetryCount = 0;

	bool IsRecording = false;
	bool IsStreaming = false;
	bool IsInit = false;

	/** Normalized user-speaking state. Written only on the game thread —
	 *  connection callbacks marshal there before touching it. */
	bool bIsSpeaking = false;

	/** A server stop waits briefly for a continuing partial transcript. This
	 *  prevents an early/duplicated VAD stop from splitting one utterance into
	 *  false Finished -> Started edges. */
	bool bSpeakingStopPending = false;
	FTSTicker::FDelegateHandle SpeakingStopTickerHandle;
	static constexpr float SpeakingStopGraceSeconds = 0.25f;

	void CancelPendingSpeakingStop();
	void ScheduleSpeakingStop();
	bool HandleSpeakingStopGraceElapsed(float DeltaTime);
	void FinishSpeakingNow();
	void ResetSpeakingStateSilently();

	float RemainingTimeUntilNextUpdate = 0;

	USoundSubmixBase* _FoundSubmix;

	// Override from UConvaiConversationComponent
	virtual bool IsPlayer() const override { return true; }
	virtual FString GetConversationalName() const override { return PlayerName; }
	
	// IConvaiConnectionInterface implementation
	virtual FString GetEndUserID() override { return EndUserID; }
	virtual FString GetEndUserMetadata() override { return EndUserMetadata; }
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

	// Helper function to broadcast connection state changes on game thread
	void BroadcastConnectionStateChanged(const FString& AttendeeId, EC_ConnectionState State);

	// Audio processing
	UPROPERTY()
	TScriptInterface<IConvaiAudioProcessingInterface> ConvaiAudioProcessing;
	class IConvaiAudioProcessingInterface* FindFirstAudioProcessingComponent();

	UFUNCTION(BlueprintCallable, Category = "Convai|AudioProcessing")
	bool SetAudioProcessingComponent(UActorComponent* AudioProcessingComponent);

	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|AudioProcessing")
	bool SupportsAudioProcessing();

	// Thread-safe audio processing wrapper
	void SafeProcessAudioData(const int16* AudioData, int32 NumSamples, int32 SampleRate) const;

	// IConvaiProcessedAudioReceiver interface implementation
	virtual void OnProcessedAudioDataReceived(const int16* ProcessedAudioData, int32 NumSamples, int32 SampleRate) override;

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|AudioProcessing")
	bool bMute = false;

	UFUNCTION(BlueprintCallable , Category = "Convai|AudioProcessing")
	bool UpdateVadBP(bool EnableVAD);

	bool bEnableAudioProcessingParse = true; //(for enable and disable audio processing )
};
