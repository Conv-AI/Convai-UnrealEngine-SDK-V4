// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiConversationComponent.h"
#include "ConvaiDefinitions.h"
#include "ConvaiConnectionInterface.h"
#include "DynamicContext/ConvaiDynamicContextTracker.h"
#include "DynamicContext/ConvaiPendingContextBatch.h"
#include "Environment/ConvaiEnvironment.h"
#include "Environment/ConvaiPendingEnvironmentBatch.h"
#include "ConvaiChatbotComponent.generated.h"

// Forward declarations
class UConvaiPlayerComponent;
class UConvaiObjectComponent;
class USoundWaveProcedural;
class UConvaiGRPCGetResponseProxy;
class UConvaiConnectionSessionProxy;
class UConvaiChatBotGetDetailsProxy;

DECLARE_LOG_CATEGORY_EXTERN(ConvaiChatbotComponentLog, Log, All);

// New V2 delegates with component references
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_ThreeParams(FOnActionReceivedSignature_V2, UConvaiChatbotComponent, OnActionReceivedEvent_V2, UConvaiChatbotComponent*, ChatbotComponent, UConvaiPlayerComponent*, InteractingPlayerComponent, const TArray<FConvaiResultAction>&, SequenceOfActions);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_TwoParams(FOnCharacterDataLoadSignature_V2, UConvaiChatbotComponent, OnCharacterDataLoadEvent_V2, UConvaiChatbotComponent*, ChatbotComponent, bool, Success); 
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_TwoParams(FOnNarrativeSectionReceivedSignature, UConvaiChatbotComponent, OnNarrativeSectionReceivedEvent, UConvaiChatbotComponent*, ChatbotComponent, FString, NarrativeSectionID);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_TwoParams(FOnEmotionReceivedSignature, UConvaiChatbotComponent, OnEmotionStateChangedEvent, UConvaiChatbotComponent*, ChatbotComponent, UConvaiPlayerComponent*, InteractingPlayerComponent);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_ThreeParams(FOnInteractionIDReceivedSignature, UConvaiChatbotComponent, OnInteractionIDReceivedEvent, UConvaiChatbotComponent*, ChatbotComponent, UConvaiPlayerComponent*, InteractingPlayerComponent, FString, InteractionID);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE(FOnFailureSignature, UConvaiChatbotComponent, OnFailureEvent);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_TwoParams(FOnInterruptedSignature, UConvaiChatbotComponent, OnInterruptedEvent, UConvaiChatbotComponent*, ChatbotComponent, UConvaiPlayerComponent*, InteractingPlayerComponent);

UCLASS(Blueprintable, BlueprintType, meta = (BlueprintSpawnableComponent), DisplayName = "Convai Chatbot")
class CONVAI_API UConvaiChatbotComponent : public UConvaiConversationComponent, public IConvaiConnectionInterface
{
	GENERATED_BODY()
public:
	UConvaiChatbotComponent();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	
	// Override from UConvaiConversationComponent
	virtual bool IsPlayer() const override { return false; }
	virtual FString GetConversationalName() const override { return CharacterName; }

	/**
	* Returns true, if the character is being talked to, is talking, or is processing the response.
	*/
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai")
	bool IsInConversation();

	/**
	* Returns true, if the character is still processing and has not received the full response yet.
	*/
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai", meta = (DisplayName = "Is Thinking"))
	bool IsProcessing();

	/**
	* Returns true, if the character is currently listening to a player.
	*/
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai")
	bool IsListening();

	/**
	* Returns true, if the character is currently talking.
	*/
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai", meta = (DisplayName = "Is Talking"))
	bool GetIsTalking();

	/** Returns time elapsed since the character started talking */
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Voice")
	float GetTalkingTimeElapsed();

	/** Returns time remaining audio time for the character to speak */
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Voice")
	float GetTalkingTimeRemaining();

	UPROPERTY(EditAnywhere, Category = "Convai", Replicated, BlueprintSetter = LoadCharacter)
	FString CharacterID;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString CharacterName;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString VoiceType;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString Backstory;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString LanguageCode;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString ReadyPlayerMeLink;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString AvatarImageLink;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Actions")
	TArray<FConvaiResultAction> ActionsQueue;

	/** Maximum time (seconds) to wait for OnStartedTalking / OnFinishedTalking before
	 *  giving up and firing a wait-gated first action anyway. Per-character setting,
	 *  shared across all actions on this chatbot. Intentionally not a UPROPERTY —
	 *  hidden from the Details panel and Blueprint to keep the action API surface
	 *  focused on the per-action toggle (FConvaiAction::bWaitForBotSpeech). */
	float ActionWaitForBotSpeechTimeoutSec = 2.0f;

	UPROPERTY(Replicated)
	FConvaiEmotionState EmotionState;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai|Emotion", Replicated)
	bool LockEmotionState = false;

	/**
	 *    Used to track memory of a previous conversation, set to -1 means no previous conversation,
	 *	  this property will change as you talk to the character, you can save the session ID for a
	 *    conversation and then set it back later on to resume a conversation
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Convai", Replicated)
	FString SessionID = "-1";

	/**
	 *    Action affordance contract for this character. Designers populate Actions, Objects,
	 *    and Characters at edit time in the Details panel. At runtime, BP can only mutate
	 *    the environment through the granular Add/Remove/Clear methods below — direct
	 *    BP read/write access is intentionally hidden so all changes flow through the
	 *    debounced WebRTC sync pipeline.
	 *
	 *    The struct's own `bEnableActions` toggle (at the top of the Details panel) gates
	 *    whether the contract is sent at /connect time. Leave off for purely conversational
	 *    characters that don't perform physical actions.
	 */
	UPROPERTY(EditAnywhere, Category = "Convai|Actions", Replicated, meta = (DisplayName = "Environment"))
	FConvaiEnvironmentData EnvironmentData;

	/**
	 *    The character this bot is currently conversing with (the player or another NPC).
	 *    Mutate via SetConversationPartner. Auto-added to Environment.Characters if missing.
	 *    The server treats this as a normal scene character; the "conversation partner" concept
	 *    is client-side — it's metadata for animation and UX, not a distinct server lane.
	 */
	UPROPERTY(EditAnywhere, Replicated, Category = "Convai|Actions")
	FConvaiObjectEntry ConversationPartner;

	/**
	 *    When true, before /connect the chatbot populates ConversationPartner from the first
	 *    registered Convai Player Component (using its PlayerName). Falls back to player pawn 0
	 *    with name "User" if no player component exists. Skip this if you set
	 *    ConversationPartner explicitly elsewhere.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Session")
	bool bAutoFillConversationPartnerFromPlayer = true;

	/**
	 *    Actor the character should visually look at. Read by AnimBP for gaze / IK; has no
	 *    effect on the conversation or RTVI pipeline. Can point at the conversation partner,
	 *    an attention object, an interesting prop, or anything else. Replicated.
	 */
	UPROPERTY(BlueprintReadWrite, Replicated, Category = "Convai|Animation")
	TObjectPtr<AActor> LookAtTarget;

	/**
	 *    Actor the character should point at. Read by AnimBP for arm IK / gesture (when the
	 *    rig supports it); has no effect on the conversation or RTVI pipeline. Independent
	 *    of LookAtTarget. Replicated.
	 */
	UPROPERTY(BlueprintReadWrite, Replicated, Category = "Convai|Animation")
	TObjectPtr<AActor> PointAtTarget;

	/**
	 *    Time in seconds, for the character's voice audio to gradually degrade until it is completely turned off when interrupted.
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
	float InterruptVoiceFadeOutDuration;

	/**
	 *    Value between -1 and 1, a value greater than zero over extends the emotions strength and a value less than zero dimishes it.
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
	float EmotionOffset = 0;

	/**
	 *    Contains key value pairs used for Narrative Design.
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai|NarrativeDesign", BlueprintSetter = UpdateNarrativeTemplateKeys)
	TMap<FString, FString> NarrativeTemplateKeys;

	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void UpdateNarrativeTemplateKeys(TMap<FString, FString> InNarrativeTemplateKeys);
	
	/**
	 *   Extra information that can be passed to the character, can contain any important data that the chracter needs to know about without the 
	 *   need of player interaction or narrative triggers, e.g. Inventory items, Player health, key information to solve a puzzle, time of day, etc...
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai", BlueprintSetter = UpdateDynamicEnvironmentInfo)
	FString DynamicEnvironmentInfo;

	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void UpdateDynamicEnvironmentInfo(FString InDynamicEnvironmentInfo);

	/**
	 * Sends a context update to dynamically modify the bot's temporary runtime context.
	 * @param Text           The context text to apply (optional when Mode is Reset)
	 * @param Mode           How the context should be applied: Append, Replace, or Reset
	 * @param ShouldRespond         Whether to trigger an LLM response: Auto, True, or False
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai")
	void UpdateContext(const FString& Text, EC_ContextUpdateMode Mode = EC_ContextUpdateMode::Append, EC_RunLLMOption ShouldRespond = EC_RunLLMOption::Auto);

	/** Internal: same as UpdateContext but folds an attention object into the same RTVI message. */
	void UpdateContextWithAttention(const FString& Text, EC_ContextUpdateMode Mode, EC_RunLLMOption ShouldRespond,
		const FConvaiObjectEntry* OptionalAttention);

	// ── Dynamic Context ──────────────────────────────────────────────

	/**
	 * How long (in seconds) to wait after the most-recent staged context update
	 * before flushing the batch to the server. Each new update during the window
	 * resets this timer so rapid bursts coalesce into a single send.
	 *
	 * Advanced: bounded above by ContextMaxDebounceWindow so an unbroken stream
	 * of updates can't postpone the flush forever.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|DynamicContext",
		AdvancedDisplay,
		meta = (DisplayName = "Context Debounce Window (s)", ClampMin = "0.1", UIMin = "0.1"))
	float ContextDebounceWindow = 0.5f;

	/**
	 * Upper bound (in seconds) on how long the FIRST update in a debounce burst
	 * can be delayed before a forced flush. Acts as a safety cap when context
	 * updates arrive faster than ContextDebounceWindow, so designers can rely
	 * on the bot reacting within a predictable worst-case latency.
	 *
	 * Set this >= ContextDebounceWindow; smaller values are clamped at flush
	 * time.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|DynamicContext",
		AdvancedDisplay,
		meta = (DisplayName = "Max Debounce Window (s)", ClampMin = "0.1", UIMin = "0.1"))
	float ContextMaxDebounceWindow = 3.0f;

	/**
	 * Sets a single state property in the dynamic context (e.g. "Health", "80").
	 * If the property already exists, its value is replaced.
	 *
	 * With ShouldRespond = Never: rebuilds and replaces the full canonical context.
	 * With ShouldRespond = Auto/Always: also appends a change message to trigger the LLM.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|DynamicContext",
		meta = (DisplayName = "Set Context State", AdvancedDisplay = "bFlushImmediately",
				ToolTip = "Sets a state property in the dynamic context.\n\nbFlushImmediately: send right away instead of batching. Use sparingly — high-frequency calls will spam WebRTC."))
	void SetContextState(const FString& Name, const FString& Value,
		EC_RunLLMOption ShouldRespond = EC_RunLLMOption::Never,
		bool bFlushImmediately = false);

	/**
	 * Sets multiple state properties at once. When ShouldRespond is Auto/Always, the
	 * change messages are combined into a single append so only one LLM response
	 * is triggered.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|DynamicContext",
		meta = (DisplayName = "Set Context States", AdvancedDisplay = "bFlushImmediately"))
	void SetContextStates(const TMap<FString, FString>& States,
		EC_RunLLMOption ShouldRespond = EC_RunLLMOption::Never,
		bool bFlushImmediately = false);

	/**
	 * Adds a chronological event to the dynamic context (e.g. "Player approached the merchant").
	 * The event is appended directly using the provided ShouldRespond option.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|DynamicContext",
		meta = (DisplayName = "Add Context Event", AdvancedDisplay = "bFlushImmediately"))
	void AddContextEvent(const FString& Text,
		EC_RunLLMOption ShouldRespond = EC_RunLLMOption::Auto,
		bool bFlushImmediately = false);

	/**
	 * Removes a state property from the dynamic context and rebuilds the canonical context.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|DynamicContext",
		meta = (DisplayName = "Remove Context State", AdvancedDisplay = "bFlushImmediately"))
	void RemoveContextState(const FString& Name, bool bFlushImmediately = false);

	/**
	 * Clears all tracked state properties and events and resets the remote context.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|DynamicContext", meta = (DisplayName = "Reset Dynamic Context"))
	void ResetDynamicContext();

	/**
	 * Returns the current value of a tracked state property.
	 * @return True if the property exists.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|DynamicContext", meta = (DisplayName = "Get Context State Value", ReturnDisplayName = "Found"))
	bool GetContextStateValue(const FString& Name, FString& OutValue) const;

	/**
	 *   End User ID used for long term memory (LTM)
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
	FString EndUserID;

	/**
	 *   End User Metadata as a JSON string for long term memory (LTM)
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
	FString EndUserMetadata;

	/**
	 *    Reset the conversation with the character and remove previous memory, this is the same as setting the session ID property to -1.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai")
	void ResetConversation();	
	
	/**
	 *    Loads a new character using its ID
	 */
	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void LoadCharacter(FString NewCharacterID);

	// ── Granular environment mutation ──────────────────────────────────
	// Each method writes through to Environment first, then schedules an
	// update-scene-metadata flush in the next debounce window. Same-name
	// entries replace in place. The optional bFlushImmediately bypasses
	// debouncing for time-critical updates.

	/**
	 * Adds an object to the Environment and schedules an update-scene-metadata send.
	 *
	 * If the object was already in the Environment at the time of /connect, the server's
	 * action_config still holds the original description until the next reconnect — only
	 * brand-new objects are pushed mid-session via update-scene-metadata.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions",
		meta = (AdvancedDisplay = "bFlushImmediately",
				ToolTip = "Add an object to the Environment and notify the bot.\n\nbFlushImmediately: send right away instead of batching with other updates. Use sparingly — high-frequency calls will spam WebRTC."))
	void AddObject(FConvaiObjectEntry Object, bool bFlushImmediately = false);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions", meta = (AdvancedDisplay = "bFlushImmediately"))
	void AddObjects(TArray<FConvaiObjectEntry> Objects, bool bFlushImmediately = false);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions", meta = (AdvancedDisplay = "bFlushImmediately"))
	void RemoveObject(FString ObjectName, bool bFlushImmediately = false);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions", meta = (AdvancedDisplay = "bFlushImmediately"))
	void RemoveObjects(TArray<FString> ObjectNames, bool bFlushImmediately = false);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions", meta = (AdvancedDisplay = "bFlushImmediately"))
	void ClearObjects(bool bFlushImmediately = false);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions", meta = (AdvancedDisplay = "bFlushImmediately"))
	void AddCharacter(FConvaiObjectEntry Character, bool bFlushImmediately = false);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions", meta = (AdvancedDisplay = "bFlushImmediately"))
	void AddCharacters(TArray<FConvaiObjectEntry> Characters, bool bFlushImmediately = false);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions", meta = (AdvancedDisplay = "bFlushImmediately"))
	void RemoveCharacter(FString InCharacterName, bool bFlushImmediately = false);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions", meta = (AdvancedDisplay = "bFlushImmediately"))
	void RemoveCharacters(TArray<FString> InCharacterNames, bool bFlushImmediately = false);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions", meta = (AdvancedDisplay = "bFlushImmediately"))
	void ClearCharacters(bool bFlushImmediately = false);

	/**
	 * Adds an FConvaiAction template to the local Environment.
	 *
	 * The server's action set is fixed at /connect time — runtime mutations only take
	 * effect on the next reconnect (StopSession + StartSession). Use this to prepare
	 * the next session, not to mutate the live one.
	 *
	 * Same-name entries replace in place.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions")
	void AddAction(FConvaiAction Action);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions")
	void AddActions(TArray<FConvaiAction> Actions);

	/** Convenience: adds a no-description / no-parameter action by name only. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions",
		meta = (DisplayName = "Add Action By Name"))
	void AddActionByName(FString Name);

	/** Removes the action whose Name matches. Case-sensitive. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions")
	void RemoveAction(FString Name);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions")
	void RemoveActions(TArray<FString> Names);

	UFUNCTION(BlueprintCallable, Category = "Convai|Actions")
	void ClearActions();

	/**
	 * Sets the bot's current attention object and (optionally) appends an event to the
	 * dynamic context. Mirrors AddContextEvent's signature with an extra attention payload.
	 *
	 * If the entry isn't already in Environment.Objects, it is auto-added so it becomes
	 * resolvable on the next reconnect. Server resolves attention only against
	 * action_config.objects; characters-only entries are auto-promoted.
	 *
	 * Has no effect when EnvironmentData.bEnableActions is false on this chatbot —
	 * the server only resolves attention when action_config was sent at /connect.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions",
		meta = (AdvancedDisplay = "bFlushImmediately",
				ToolTip = "Set attention object + optional event text. Has no effect when Environment > Enable Actions is off.\n\nbFlushImmediately: send right away instead of batching. Use sparingly."))
	void SetObjectInAttention(FConvaiObjectEntry AttentionObject,
		FString Text = TEXT(""),
		EC_RunLLMOption ShouldRespond = EC_RunLLMOption::Auto,
		bool bFlushImmediately = false);

	/**
	 * Who last set CurrentAttentionObject. Gaze-driven sets only succeed when this is
	 * None or Gaze; an Explicit (BP/C++) set locks the slot until cleared with an empty
	 * SetObjectInAttention call. Runtime-only — not persisted, not replicated.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Convai|Actions")
	EConvaiAttentionSource AttentionSource = EConvaiAttentionSource::None;

	/**
	 * Gaze-gated wrapper around SetObjectInAttention. Returns true when the chatbot
	 * accepted the new attention (AttentionSource was None or Gaze), false when the
	 * slot is currently owned by an Explicit Blueprint/C++ set.
	 *
	 * On accept this stamps AttentionSource = Gaze so subsequent gaze sets/clears on the
	 * same chatbot can take and release the slot freely.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions",
		meta = (AdvancedDisplay = "bFlushImmediately",
				ToolTip = "Set attention via gaze. Rejected (returns false) if BP/C++ explicitly set the slot — gaze can only own None or already-Gaze slots."))
	bool TrySetObjectInAttentionFromGaze(FConvaiObjectEntry AttentionObject,
		FString Text = TEXT(""),
		EC_RunLLMOption ShouldRespond = EC_RunLLMOption::Auto,
		bool bFlushImmediately = false);

	/**
	 * Companion to TrySetObjectInAttentionFromGaze. Clears the attention slot only when
	 * (a) the chatbot still considers gaze the owner, and (b) the currently-attended
	 * object matches the one we expect to be clearing — so a late "gaze lost" call from
	 * one player can't wipe out a newer gaze target set by another path.
	 * Returns true when the clear was applied.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions",
		meta = (ToolTip = "Clear the gaze-owned attention slot if it still matches ExpectedObject. No-op when ownership changed underneath us."))
	bool TryClearObjectInAttentionFromGaze(FConvaiObjectEntry ExpectedObject);

	/**
	 * Sets the character this bot is currently conversing with.
	 *
	 * If the partner isn't already in Environment.Characters, it's auto-added so the bot
	 * sees it in scene metadata. Server-side this maps to a normal character entry — the
	 * "conversation partner" concept is purely a client-side affordance for animation, UI,
	 * and clean "who am I talking to" semantics.
	 *
	 * Pass an empty entry (default-constructed FConvaiObjectEntry with empty Name) to
	 * clear the partner without removing anyone from the character list.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions",
		meta = (AdvancedDisplay = "bFlushImmediately",
				ToolTip = "Set the character the bot is currently conversing with. Auto-adds to Environment.Characters if missing.\n\nbFlushImmediately: send right away instead of batching. Use sparingly."))
	void SetConversationPartner(FConvaiObjectEntry Partner, bool bFlushImmediately = false);

	/**
	 * Walk EnvironmentData.Objects and, for each entry whose Ref actor doesn't already have a
	 * UConvaiObjectComponent, spawn one bound exclusively to this chatbot. Auto-spawned
	 * components inherit the entry's ObjectEntry (including ComponentName for component-level
	 * scoping) and route gaze notifications only to this chatbot — they don't register with
	 * the subsystem-wide pool and aren't visible to other chatbots.
	 *
	 * Idempotent: running it again is a no-op for entries that already have a component, so
	 * it's safe to wire from multiple hooks (BeginPlay, AddObject, AddObjects, etc.).
	 *
	 * Returns the number of new components spawned.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions",
		meta = (ToolTip = "Auto-spawn UConvaiObjectComponent on each Environment.Objects actor that doesn't already have one. New components are exclusively paired with this chatbot."))
	int32 EnsureObjectComponentsForEnvironmentObjects();

public:
	/**
	 * Auto-spawned UConvaiObjectComponents this chatbot keeps alive — either ones it spawned
	 * itself via EnsureObjectComponentsForEnvironmentObjects, or ones it adopted because another
	 * chatbot had already spawned a matching component for an actor it also references.
	 * UPROPERTY-tracked so GC keeps them alive as long as the chatbot is alive; raw pointers
	 * (not weak) on purpose because these entries keep auto-spawned components registered with
	 * the actor. EndPlay destroys them only once no other chatbot still lists them here.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Convai|Actions")
	TArray<UConvaiObjectComponent*> OwnedAutoSpawnComponents;

protected:

	/**
	 * Returns an existing UConvaiObjectComponent on Actor that resolves to the same target
	 * component as Entry (or both are whole-actor scope). When null,
	 * EnsureObjectComponentsForEnvironmentObjects spawns a new component so this entry's scope
	 * is represented on the actor. Every registered ObjComp fans out to every chatbot via the
	 * subsystem pool, so any matching-scope component is automatically reachable.
	 */
	UConvaiObjectComponent* FindCoveringObjectComponentOnActor(
		const FConvaiObjectEntry& Entry,
		AActor* Actor) const;

public:

	/**
	 * DEPRECATED — legacy BP-visible Environment pointer. Lazily allocated on first access
	 * to a UConvaiEnvironment shim that delegates back to this chatbot, so old BP graphs
	 * that did `Chatbot->Environment->XxxMethod(...)` keep compiling with deprecation
	 * warnings pointing at the new API. New code should use the granular methods on the
	 * chatbot directly (AddObject, AddCharacter, SetObjectInAttention, SetConversationPartner)
	 * and LookAtTarget for animation gaze.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, BlueprintGetter = GetEnvironment,
		Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedProperty,
				DeprecationMessage = "Use the chatbot's granular methods directly. LookAtTarget replaces MainCharacter for animation gaze."))
	class UConvaiEnvironment* Environment;

	UFUNCTION(BlueprintGetter)
	class UConvaiEnvironment* GetEnvironment();

	/**
	 * Override in Blueprint to append extra actions, objects, or characters to the
	 * Environment right before /connect. Called once at the start of StartSession().
	 * Use this to populate based on dynamic world state (nearby NPCs, current quest
	 * inventory, etc.) instead of relying on edit-time defaults.
	 *
	 * The defaults from the Environment property in the Details panel are still
	 * sent; this hook only APPENDS — it does not replace.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Convai|Session",
		meta = (DisplayName = "Gather Environment Extras"))
	void GatherEnvironmentExtras(TArray<FConvaiAction>& OutExtraActions,
		TArray<FConvaiObjectEntry>& OutExtraObjects,
		TArray<FConvaiObjectEntry>& OutExtraCharacters);
	virtual void GatherEnvironmentExtras_Implementation(TArray<FConvaiAction>& OutExtraActions,
		TArray<FConvaiObjectEntry>& OutExtraObjects,
		TArray<FConvaiObjectEntry>& OutExtraCharacters) {}

	/**
	 * Adds (or replaces by name) an entry in EnvironmentData.Objects sourced from a
	 * UConvaiObjectComponent. Called by the object component at its BeginPlay so
	 * components spawned mid-session can register with already-connected chatbots,
	 * and also from this chatbot's own StartSession via GatherObjectComponentsFromSubsystem.
	 */
	void AddOrUpdateObjectFromComponent(class UConvaiObjectComponent* ObjectComponent);

	/**
	 * Appends a TArray of FConvaiResultAction items to the existing ActionsQueue.
	 * If ActionsQueue is not empty, it takes the first element, appends the new array to it,
	 * and then reassigns it back to ActionsQueue. If ActionsQueue is empty, it simply sets ActionsQueue to the new array.
	 *
	 * @param NewActions Array of FConvaiResultAction items to be appended.
	 *
	 * @category Convai
	 */
	 void AppendActionsToQueue(TArray<FConvaiResultAction> NewActions);

	/**
	 * Reports an action's outcome to the chatbot. Call this from your action
	 * handler the moment it finishes (whether it succeeded or gave up).
	 *
	 * What it does:
	 *   1. Updates the action queue — success dequeues the head and advances to
	 *      the next action; failure clears the rest of the queue.
	 *   2. (When bAutoReport is true, the default) Sends a dynamic-context event
	 *      to the LLM with a sensible default message describing the outcome.
	 *      Any AdditionalNote you supplied is appended as ", note: <text>".
	 *
	 * Default messages used when bAutoReport is true and AdditionalNote is empty:
	 *   • Success: "you were able to <action> successfully"
	 *   • Failure: "failed to <action>, try something else or consult with
	 *     <player name>"
	 *
	 * Example of bAutoReport + AdditionalNote combined:
	 *   HandleActionCompletion(IsSuccessful=true, bAutoReport=true,
	 *                          AdditionalNote="picked the red one")
	 *   →  sends: "you were able to <action> successfully, note: picked the red one"
	 *
	 * When bAutoReport is false, only AdditionalNote is sent (or nothing, if empty).
	 *
	 * @param IsSuccessful    true  → dequeue and start the next action.
	 *                        false → clear the remaining queue.
	 * @param bAutoReport     true (default) → send a default outcome message to
	 *                        the LLM (with AdditionalNote appended as a note, if
	 *                        any). false → skip the default message; send
	 *                        AdditionalNote on its own, or nothing if empty.
	 * @param ShouldRespond   How the bot should react to the event:
	 *                        Never (default) — silent context update only; the
	 *                        outcome lands in the LLM's view on the next user
	 *                        turn instead of interrupting the bot to comment
	 *                        on its own action.
	 *                        Auto — let the LLM decide whether to react.
	 *                        Always — force a spoken reply.
	 * @param AdditionalNote  Optional free-form text to attach to the message.
	 *                        Reads as a ", note: ..." appendix on the
	 *                        auto-generated default. With bAutoReport=false and
	 *                        a non-empty value, it's sent on its own. Default
	 *                        is empty — most callers won't need this.
	 * @param Delay           Seconds to wait before the next LLM-touching step.
	 *                        On success, defers starting the next action.
	 *                        On failure, defers sending the outgoing event
	 *                        (useful when you want an animation or sfx to play
	 *                        before the bot reacts). 0 (default) = no delay.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions",
		meta = (AdvancedDisplay = "AdditionalNote,Delay"))
	void HandleActionCompletion(bool IsSuccessful = true, bool bAutoReport = true,
		EC_RunLLMOption ShouldRespond = EC_RunLLMOption::Never,
		FString AdditionalNote = TEXT(""),
		float Delay = 0.0f);

	/**
	 * Aborts the current action sequence. Clears the queue without retrying or
	 * advancing — use when a handler hits an unrecoverable problem (target gone,
	 * preconditions failed) and wants the LLM to plan a fresh sequence instead of
	 * retrying the broken one.
	 *
	 * The optional EventText + ShouldRespond pair routes through AddContextEvent so
	 * the bot is told what went wrong in the same call. Pass an empty EventText for
	 * a silent abort (the queue is still cleared).
	 *
	 * Common pattern when you want a new sequence: pass an EventText like
	 * "Couldn't reach the cube — it's blocked" with ShouldRespond=Always so the LLM
	 * acknowledges and generates a new action plan.
	 *
	 * @param EventText      Optional message describing what failed. Empty = silent abort.
	 * @param ShouldRespond  Auto / Always / Never. Ignored when EventText is empty.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions",
		meta = (AdvancedDisplay = "EventText,ShouldRespond"))
	void AbortActionSequence(FString EventText = TEXT(""),
		EC_RunLLMOption ShouldRespond = EC_RunLLMOption::Auto);

	/**
	 * Checks if the ActionsQueue managed by the Convai chatbot component is empty.
	 *
	 * @return Returns true if the ActionsQueue is empty; otherwise, returns false.
	 *
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Actions")
	bool IsActionsQueueEmpty();

	/**
	 * Clears the ActionsQueue managed by the Convai chatbot component.
	 *
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions")
	void ClearActionQueue();

	/**
	 * Fetches the first action from the ActionsQueue managed by the Convai chatbot component.
	 *
	 * @param ConvaiResultAction Reference to a struct that will be populated with the details of the first action in the queue.
	 *
	 * @return Returns true if there is at least one action in the ActionsQueue and the struct has been successfully populated; otherwise, returns false.
	 *
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Actions")
	bool FetchFirstAction(FConvaiResultAction& ConvaiResultAction);

	/**
	 * Removes the first action from the ActionsQueue managed by the Convai chatbot component.
	 *
	 * @return Returns true if an action was successfully removed; otherwise, returns false.
	 *
	 */
	bool DequeueAction();

	/**
	 * Starts executing the first action in the ActionsQueue by calling TriggerNamedBlueprintAction.
	 *
	 * @return Returns true if the first action was successfully started; otherwise, returns false.
	 *
	 */
	UFUNCTION()
	bool StartFirstAction();
	
	/**
	 * Triggers a specified Blueprint event or function on the owning actor based on the given action name and parameters.
	 *
	 * @param ActionName The name of the Blueprint event or function to trigger. This event or function should exist in the Blueprint that owns this component.
	 * @param ConvaiActionStruct A struct containing additional data or parameters to pass to the Blueprint event or function.
	 *
	 * @note The function attempts to dynamically find and call a Blueprint event or function in the owning actor's class. If the Blueprint event or function does not exist or if the signature doesn't match, the function will log a warning.
	 *
	 */
	bool TriggerNamedBlueprintAction(const FString& ActionName, FConvaiResultAction ConvaiActionStruct);
	bool TryCallFunction(UObject* Object, const FString& FunctionName, FConvaiResultAction& ConvaiResultAction) const;
	
	UFUNCTION(BlueprintCallable, Category = "Convai|Emotion")
	void ForceSetEmotion(EBasicEmotions BasicEmotion, EEmotionIntensity Intensity, bool ResetOtherEmotions = false);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Emotion")
	float GetEmotionScore(EBasicEmotions Emotion);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Emotion")
	TMap<FName, float> GetEmotionBlendshapes();

	UFUNCTION(BlueprintCallable, Category = "Convai|Emotion")
	void ResetEmotionState();

	/**
	 * Get the current chatbot connection state
	 * @return The current chatbot connection state
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Connection")
	EC_ConnectionState GetChatbotConnectionState() const;

public:
	/** Called when a new action is received from the API */
	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Actions Received"))
	FOnActionReceivedSignature_V2 OnActionReceivedEvent_V2;
	
	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Emotion State Changed"))
	FOnEmotionReceivedSignature OnEmotionStateChangedEvent;


	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Character Data Loaded"))
	FOnCharacterDataLoadSignature_V2 OnCharacterDataLoadEvent_V2;

	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Narrative Section Received"))
	FOnNarrativeSectionReceivedSignature OnNarrativeSectionReceivedEvent;

	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Interaction ID Received"))
	FOnInteractionIDReceivedSignature OnInteractionIDReceivedEvent;

	/** Called when the character is interrupted */
	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Interrupted"))
	FOnInterruptedSignature OnInterruptedEvent;

	/** Called when there is an error */
	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Failure"))
	FOnFailureSignature OnFailureEvent;	

public:
	UFUNCTION(BlueprintCallable, Category = "Convai", meta = (DisplayName = "Invoke Speech"))
	void ExecuteNarrativeTrigger(FString TriggerMessage, bool InGenerateActions, bool InReplicateOnNetwork);

	UFUNCTION(BlueprintCallable, Category = "Convai", meta = (DisplayName = "Invoke Narrative Design Trigger"))
	void InvokeNarrativeDesignTrigger(FString TriggerName, bool InGenerateActions, bool InReplicateOnNetwork);

	void InvokeTrigger_Internal(const FString& TriggerName, const FString& TriggerMessage, bool InGenerateActions, bool InReplicateOnNetwork);

	// Interrupts the current speech with a provided fade-out duration. 
	// The fade-out duration is controlled by the parameter 'InVoiceFadeOutDuration'.
	UFUNCTION(BlueprintCallable, Category = "Convai")
	void InterruptSpeech(float InVoiceFadeOutDuration);

	// Broadcasts an interruption of the current speech across a network, with a provided fade-out duration.
	// This function ensures that the interruption is communicated reliably to all connected clients.
	// The fade-out duration is controlled by the parameter 'InVoiceFadeOutDuration'.
	UFUNCTION(NetMulticast, Reliable, Category = "VoiceNetworking")
	void Broadcast_InterruptSpeech(float InVoiceFadeOutDuration);

public:
	// AActorComponent interface
	virtual void BeginPlay() override;
	//virtual void OnRegister() override;
	//virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	// End AActorComponent interface

	//~ Begin UObject Interface.
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
	virtual void PostInitProperties() override;
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End UObject Interface.

private:
	/** Repopulate the Transient RenderedString preview on every FConvaiAction in
	 *  EnvironmentData. Called after construction, after deserialization, and after
	 *  edits — RenderedString isn't serialized, so it must be regenerated each time
	 *  the chatbot loads (otherwise the preview field shows blank in the editor). */
	void RefreshActionRenderedStrings();

	//~ Begin UConvaiAudioStreamer Interface.
	virtual bool CanUseLipSync() override;
	virtual bool CanUseVision() override;
	virtual void onAudioFinished() override;
	//~ End UConvaiAudioStreamer Interface.

	/**
	 * Force play any buffered audio immediately.
	 * Used when no more audio is expected but we have buffered data.
	 */
	void ForcePlayBufferedAudio();

private:
	UConvaiChatBotGetDetailsProxy* ConvaiGetDetails();

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
	FScriptDelegate ConvaiChatBotGetDetailsDelegate;
#else
	TScriptDelegate<FWeakObjectPtr> ConvaiChatBotGetDetailsDelegate;
#endif

	UFUNCTION()
	void OnConvaiGetDetailsCompleted(FString ReceivedCharacterName, FString ReceivedVoiceType, FString ReceivedBackstory, FString ReceivedLanguageCode, bool HasReadyPlayerMeLink, FString ReceivedReadyPlayerMeLink, FString ReceivedAvatarImageLink);

private:
	UPROPERTY()
	UConvaiChatBotGetDetailsProxy* ConvaiChatBotGetDetailsProxy;

	/** Client-side source of truth for dynamic state properties and events. */
	FConvaiDynamicContextTracker DynamicContextTracker;

	/** Updates staged within the current debounce window, flushed as one batch. */
	FConvaiPendingContextBatch PendingContextBatch;

	/** Environment mutations staged within the current debounce window — flushed as one
	 *  update-scene-metadata send by the same tick driver as the dynamic-context batch. */
	FConvaiPendingEnvironmentBatch PendingEnvironmentBatch;


	/** Snapshot of Object/Character names that were sent in action_config at the moment of
	 *  the last successful /connect. The env-batch flush subtracts this set so connect-time
	 *  entries aren't re-sent via update-scene-metadata. Cleared on disconnect. */
	TSet<FString> EnvironmentSnapshotAtConnect;

	/** Helper: either run FlushDynamicContext now or schedule it via debounce. */
	void ScheduleOrFlushDynamicContext(bool bFlushImmediately);

	/** Trigger messages / narrative design triggers queued while disconnected. */
	struct FPendingTrigger
	{
		FString TriggerName;
		bool bGenerateActions = false;
		bool bReplicateOnNetwork = false;
	};
	TArray<FPendingTrigger> PendingTriggers;

	/** Absolute time (FPlatformTime::Seconds) at which the next flush should occur. -1 = idle. */
	double NextContextFlushTime = -1.0;

	/** Absolute time at which the current debounce window opened (i.e., the first
	 *  staged update of the current burst). -1 when idle. Used to enforce the
	 *  ContextMaxDebounceWindow safety cap so unbounded update streams can't
	 *  postpone the flush forever. */
	double DebounceWindowStartTime = -1.0;

	/** Bumps the flush timer forward to now + ContextDebounceWindow, but never
	 *  past DebounceWindowStartTime + ContextMaxDebounceWindow. Starts a fresh
	 *  burst when the window is idle. */
	void ScheduleContextFlush();

	/** Called from TickComponent to drive the flush loop. */
	void TickDynamicContext();

	/** Apply staged batch + queued triggers to the session (called when connected and timer elapsed). */
	void FlushDynamicContext();

	/** True when the underlying session is in the Connected state. */
	bool IsChatbotConnected() const;

	TMap<FName, float> EmotionBlendshapes;
	TArray<uint8> RecordedAudio;
	uint32 RecordedAudioSampleRate;
	bool IsRecordingAudio;

	/** The session proxy instance */
	UPROPERTY()
	UConvaiConnectionSessionProxy* SessionProxyInstance;

public:
	/** Whether to automatically initialize the session in BeginPlay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Session")
	bool bAutoInitializeSession;

	/**
	 * Initializes the session for this chatbot component
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Session")
	void StartSession();

	/**
	 * Shuts down the session for this chatbot component
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Session")
	void StopSession();

private:
	// IConvaiConnectionInterface implementation
	virtual FString GetActionConfigJson() override
	{
		return EnvironmentData.bEnableActions ? EnvironmentData.ToActionConfigJson() : FString();
	}
	virtual FString GetEndUserID() override { return EndUserID; }
	virtual FString GetEndUserMetadata() override { return EndUserMetadata; }
	virtual bool IsVisionSupported() override { return SupportsVision(); }
	virtual EC_LipSyncMode GetLipSyncMode() override;
	virtual bool RequiresPrecomputedFaceData() override;
	virtual void OnConnectedToServer() override;
	virtual void OnDisconnectedFromServer() override;
	virtual void OnAttendeeConnecting() override;
	virtual void OnAttendeeConnected(FString AttendeeId) override;
	virtual void OnAttendeeDisconnected(FString AttendeeId) override;
	virtual void OnTranscriptionReceived(FString Transcription, bool IsTranscriptionReady, bool IsFinal) override;
	virtual void OnAudioDataReceived(const int16_t* AudioData, size_t NumFrames, uint32_t SampleRate, uint32_t BitsPerSample, uint32_t NumChannels) override;
	virtual void OnStartedTalking() override;
	virtual void OnFinishedTalking() override;
	virtual void OnLLMStarted() override;
	virtual void OnLLMStopped() override;
	virtual void OnInterrupt() override;
	virtual void OnInterruptEnd() override;
	virtual void OnFaceDataReceived(FAnimationSequence FaceDataAnimation) override;
	virtual void OnSessionIDReceived(FString ReceivedSessionID) override;
	virtual void OnInteractionIDReceived(FString ReceivedInteractionID) override;
	virtual void OnActionSequenceReceived(const TArray<FConvaiResultAction>& ReceivedSequenceOfActions) override;
	virtual void OnEmotionReceived(FString ReceivedEmotionResponse, FAnimationFrame EmotionBlendshapesFrame, bool MultipleEmotions) override;
	virtual void OnNarrativeSectionReceived(FString BT_Code, FString BT_Constants, FString ReceivedNarrativeSectionID) override;
	virtual void OnFailure(FString Message) override;

	FThreadSafeBool IsConnectionTalking = false;

	/** Flag indicating the character is interrupted by user speaking */
	FThreadSafeBool bIsInterrupted = false;

	void SendImage(const float& DeltaTime);

	// Helper function to broadcast connection state changes on game thread
	void BroadcastConnectionStateChanged(const FString& AttendeeId, EC_ConnectionState State);

private:
	/** Timestamp of the most recent OnFinishedTalking transition. -1 means the
	 *  connection never entered a talking state. Used to measure the grace
	 *  window in OnAudioDataReceived. */
	double FinishedTalkingTimestamp = -1.0;

	/** Protects FinishedTalkingTimestamp which is read/written from both the
	 *  WebRTC transport thread (OnAudioDataReceived) and the Game Thread
	 *  (OnFinishedTalking, OnStartedTalking, OnLLMStarted). */
	mutable FCriticalSection FinishedTalkingLock;

	/** Maximum gap of *real* silence (analyser-classified) we'll tolerate in
	 *  the tail before flushing. Measured from the last real-audio frame, or
	 *  from OnFinishedTalking when no real audio has been heard yet. Tuned to
	 *  the same 0.5 s as the original stutter window so behaviour is the same
	 *  in the "no real-audio detected" case. */
	static constexpr double StutterGraceWindow = 0.5;

	/** Hard ceiling on tail extension, in seconds. Once this many seconds have
	 *  elapsed since OnFinishedTalking we flush no matter what the analyser
	 *  thinks — protects against pathological inputs (constant tone, looping
	 *  garbage) that would otherwise keep the debounce timer alive forever.
	 *
	 *  -1.0 (default) disables the cap: tail extension is unbounded. Flip to
	 *  e.g. 2.0 once we've validated the analyser-driven debounce alone is
	 *  reliable enough in the field — useful as a belt-and-braces safety net,
	 *  not as primary control. */
	static constexpr double MaxTailExtension = -1.0;

	/** Timestamp of the most recently analyser-classified "real audio" tail
	 *  frame. -1 = no real audio seen since OnFinishedTalking (silence is
	 *  then measured from FinishedTalkingTimestamp instead). Written under
	 *  FinishedTalkingLock by both the WebRTC transport thread (real-audio
	 *  detection in OnAudioDataReceived) and the Game thread (resets in
	 *  OnStartedTalking / OnFinishedTalking / OnLLMStarted). */
	double LastRealAudioTimestamp = -1.0;

	/** Rolling per-frame audio analysis stats from the previous OnAudioDataReceived
	 *  tail-frame. Only read/written on the WebRTC transport thread inside that
	 *  handler, so it does not need a lock. Not explicitly reset across utterances
	 *  — the analyser runs on every tail-frame, so the first tail-frame of a new
	 *  utterance overwrites whatever the previous utterance left behind within
	 *  16 ms. The smoothing rule's "RMS collapsed AND variance below threshold"
	 *  double-gate keeps a stale high-RMS PrevStats from mis-suppressing a
	 *  genuine quiet syllable.
	 *
	 *  Threaded through UConvaiUtils::ContainsActualAudio so its cross-frame
	 *  smoothing can see deltas — single noisy frames can't re-extend the
	 *  grace window on their own, only sustained signal does. */
	FConvaiAudioFrameStats LastAudioFrameStats;

	// Accumulates DeltaTime so we can throttle to ConvaiVision's FPS.
	float TimeSinceLastVideoSend = 0.f;

	// Cached target interval between frames (in seconds). Recomputed if FPS changes.
	float TargetFrameInterval = 1.f / 15.f; // sensible default 15 FPS

	// Cache the last seen FPS to avoid recomputing every tick
	int32 CachedVisionFPS = 15;

	bool bWasPublishingVideo = false;

	/** Wait-for-bot-speech state for the first action of a freshly-arrived sequence.
	 *  Flipped on in OnActionSequenceReceived when the first action carries
	 *  bWaitForBotSpeech and the queue was empty. The next OnStartedTalking,
	 *  OnFinishedTalking, llm-no-response, or ActionWaitTimerHandle expiry will
	 *  fire FlushPendingActionWait — whichever comes first. Subsequent triggers
	 *  are no-ops because the flag is cleared on first fire. */
	bool bWaitingForBotSpeechTrigger = false;

	/** Per-action delay captured from the gating action's DelayAfterBotSpeechSec.
	 *  Applied AFTER the wait condition resolves, before StartFirstAction runs. */
	float PendingActionPostSpeechDelay = 0.0f;

	/** Timer that times out the wait-for-bot-speech window
	 *  (ActionWaitForBotSpeechTimeoutSec). Cleared when any speech event fires. */
	FTimerHandle ActionWaitTimerHandle;

	/** Timer for the optional post-speech delay before StartFirstAction. */
	FTimerHandle PostSpeechDelayTimerHandle;

	/** Common entry point for "the wait is over": cancels the timeout, applies the
	 *  post-speech delay if any, then calls StartFirstAction. Idempotent — only the
	 *  first invocation has effect; subsequent ones return early because
	 *  bWaitingForBotSpeechTrigger is cleared on first fire. */
	void FlushPendingActionWait();
};
