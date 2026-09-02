// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ConvaiDefinitions.h"
#include "Utility/Log/ConvaiLogger.h"
#include "ConvaiObjectComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiObjectComponentLog, Log, All);

class UConvaiChatbotComponent;
class UConvaiPlayerComponent;
class UConvaiObjectComponent;
class ULineBatchComponent;

// Gaze event payload — emitted by UConvaiObjectComponent on highlight and attention transitions.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FConvaiObjectGazeEvent,
	UConvaiObjectComponent*, ObjectComponent,
	UConvaiPlayerComponent*, PlayerComponent);

/**
 * Drop this component on any Actor (door, switch, lever, room, item, vehicle, ...)
 * to make it visible to all Convai chatbots in the level — without having to
 * hand-author it on each chatbot one by one. Plays the same role for "things"
 * that Convai Chatbot Component plays for NPCs and Convai Player Component
 * plays for the player.
 *
 * What this component gives you:
 *
 *   1. Identity. Fill in the Object Details (name + description, plus the
 *      navigation targeting fields if you want AI actions to walk to this
 *      object). Every chatbot automatically pulls this in at session start
 *      and adds it to its known-objects list. The "Ref" field is auto-bound
 *      to the owning Actor — you don't set it.
 *
 *   2. Live state. Add entries to Tracked Properties to pick UPROPERTYs on
 *      this Actor that the AI should know about (a "Door is locked" bool, a
 *      "Health" int, etc.). Each tracked property is sent to chatbots at
 *      session start and again every time its value changes, so the AI is
 *      always in sync. Sampling happens on a shared clock so all objects
 *      tick at the same instant — efficient even with many of them.
 */
UCLASS(ClassGroup = (Convai), meta = (BlueprintSpawnableComponent), HideCategories = (ComponentTick))
class CONVAI_API UConvaiObjectComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UConvaiObjectComponent();

#if WITH_EDITOR
	/** One-time data heal for maps touched by the pre-visualizer Movement Point
	 *  system: its editor-only arrow markers could get baked into saved actors
	 *  by reconstruction (the instance-data cache drops the transient flag).
	 *  Strips any that match the old markers' fingerprint. */
	virtual void OnRegister() override;

	/** The editor-time transform relative Movement Points anchor to: the
	 *  resolved sub-component's transform when the entry is component-scoped
	 *  (ObjectEntry.Ref only auto-binds at BeginPlay, so the filter is resolved
	 *  directly against the owner), else the owner actor's transform — the same
	 *  anchoring FConvaiMovementPoint::ResolveWorldLocation applies at runtime.
	 *  Used by FConvaiMovementPointVisualizer, which handles all viewport
	 *  editing of the points (nothing is spawned into the actor). */
	FTransform GetMovementPointAnchorTransform() const;
#endif

	// ── Master switch ────────────────────────────────────────────────

	/**
	 * Master switch for this object's presence in Convai. When OFF the object
	 * is invisible to the whole system: not in any chatbot's environment, no
	 * tracked-property broadcasts, no spatial awareness, no gaze — exactly as
	 * if the component weren't there. Toggle at runtime via Set Convai Object
	 * Enabled; turning it off mid-session cleans the object out of every
	 * chatbot (states, environment entry, attention slot).
	 */
	UPROPERTY(EditAnywhere, Category = "Convai|Object",
		meta = (DisplayName = "Enabled"), BlueprintSetter = SetConvaiObjectEnabled)
	bool bConvaiEnabled = true;

	/** Runtime toggle for Enabled — registers/unregisters through the same
	 *  paths BeginPlay/EndPlay use, so on/off is always a clean full cycle. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object")
	void SetConvaiObjectEnabled(bool bEnabled);

	// ── Identity ─────────────────────────────────────────────────────

	/**
	 * How this object identifies itself to chatbots.
	 *
	 *   - Name:        what every chatbot will call this object (e.g. "FrontDoor",
	 *                  "PowerLever"). Must be unique across the level; the
	 *                  subsystem will rename duplicates automatically.
	 *   - Description: what this object IS, in plain language for the AI.
	 *   - The rest of the fields control how AI movement actions navigate to
	 *     this object — "walk to the actor", "walk to a specific component on
	 *     it", with optional socket / step-onto-bounds / offset refinements.
	 *
	 * Each chatbot pulls this in at session start so the AI knows the object
	 * exists and can reference it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object", meta = (ShowOnlyInnerProperties))
	FConvaiObjectEntry ObjectEntry;

	/**
	 * Properties on this Actor that the AI should be kept aware of in real time.
	 *
	 * For each entry, pick a property (bool, number, string, enum, struct
	 * member, etc. — paths may hop through actor / component references into
	 * sub-objects), describe what it means, optionally describe what each
	 * value means, and choose how the chatbot should react to changes.
	 *
	 * The current value is sent to all chatbots at session start, then again
	 * every time it changes. The AI sees them as keys like
	 * "<this object's name>.<alias, or the property path>" (whitespace-stripped).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object")
	TArray<FConvaiTrackedProperty> TrackedProperties;

	/** Controls transform-motion detection for this object. Movement awareness
	 *  feeds concise direction into spatial context; optional settings add the
	 *  watchable `<ObjectName>.Movement` state and movement-edge responses. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object|Movement Awareness",
		meta = (ShowOnlyInnerProperties))
	FConvaiObjectMovementSettings MovementAwareness;

	/**
	 * When ON (default), this object is included in the Convai spatial-awareness
	 * system: every chatbot is quietly told where it is relative to them ("the
	 * crate is close by, in front of you"), whether it can be walked to, and how
	 * it relates to nearby things ("…and on top of the pressure plate") — composed
	 * centrally by UConvaiContextSubsystem and delivered as a Context Fact.
	 *
	 * Turn it OFF for objects the AI shouldn't be spatially aware of (background
	 * props, scenery, trigger volumes). This does NOT affect Tracked Properties —
	 * those are still reported regardless.
	 *
	 * Has no effect unless spatial awareness is enabled in Project Settings ▸
	 * Plugins ▸ Convai ▸ Spatial Awareness.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object",
		meta = (DisplayName = "Include in Spatial Awareness"))
	bool bIncludeInSpatialAwareness = true;

	/**
	 * When ON, this object is MERGED with every other Convai object that shares its
	 * Name (case-insensitive) AND the same Merge Group Index (below) into a SINGLE
	 * logical object the AI perceives as one thing:
	 *   • its position is the average of all members (e.g. the middle of a pile);
	 *   • gazing at any one member highlights them ALL together;
	 *   • the AI gets one description for the whole set — the first non-empty member
	 *     description (so you only fill it in on one of them);
	 *   • members of the set are NOT suffixed apart from one another — they share one
	 *     name on purpose. (If a SEPARATE merged set, or a non-merged object, already
	 *     uses that name, this whole set still gets a single shared suffix to stay
	 *     addressable — see Merge Group Index.)
	 *
	 * Leave OFF (default) to keep this object distinct: same-named objects are then
	 * told apart by a name suffix instead (style set in Project Settings ▸ Plugins ▸
	 * Convai ▸ Objects ▸ Duplicate Name Suffix Style). Turn it ON for piles/sets of
	 * identical props — a stack of crates, a row of pressure plates — the AI should
	 * treat as one thing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object|Grouping",
		meta = (DisplayName = "Merge With Same-Named Objects"))
	bool bMergeWithSameNamedObjects = false;

	/**
	 * Which merge set this object belongs to, used only when "Merge With Same-Named
	 * Objects" is ON. Objects are merged together only when they share the same Name
	 * AND the same index.
	 *
	 * Leave at 0 for the common case (a single pile of identically-named objects).
	 * Use different indices when the level has TWO separate sets of same-named objects
	 * that should each merge on their own — e.g. two stacks both named "Crate": give
	 * one stack index 0 and the other index 1 so they become two separate logical
	 * objects instead of one averaged blob spanning both stacks. They are then told
	 * apart by a name suffix (one keeps "Crate", the other becomes "Crate 2"); which
	 * set keeps the bare name depends on which registers first, not on the index value.
	 *
	 * Has no effect while merging is OFF.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object|Grouping",
		meta = (DisplayName = "Merge Group Index", EditCondition = "bMergeWithSameNamedObjects",
		        ClampMin = "0", UIMin = "0"))
	int32 MergeGroupIndex = 0;

	/**
	 * Internal: the object's authored Name as seen at first registration, BEFORE any
	 * duplicate-suffix is applied. Set by UConvaiSubsystem::RegisterObjectComponent and
	 * used to match later merge siblings (whose ObjectEntry.Name may already be suffixed).
	 * Not serialized; not intended for general use.
	 */
	FString RegisteredBaseName;

	/**
	 * Returns this object's targeted scene component if ObjectEntry has a component
	 * filter (resolves lazily via FConvaiObjectEntry::ResolveComponent and caches the
	 * result), or nullptr when the object is whole-actor scope. The player component
	 * uses this to decide whether the gaze is hitting the right primitive.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object")
	USceneComponent* GetResolvedComponent();

	// ── Blueprint mutators ───────────────────────────────────────────

	/**
	 * Add a new tracked property at runtime — useful for properties that only
	 * become relevant once the player has done something (picked up an item,
	 * entered a room). The property's current value is pushed to all chatbots
	 * immediately. Returns false if the path is empty, already in the list, or
	 * resolves to an unsupported type. A path that simply doesn't resolve YET
	 * (e.g. it hops through a sub-actor reference that is still null) is
	 * accepted and starts broadcasting as soon as it resolves on the poll.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object")
	bool AddTrackedProperty(const FConvaiTrackedProperty& InProperty);

	/**
	 * Stop tracking a property at runtime. Returns true if it was removed.
	 * The property's state key is also removed from every chatbot's dynamic
	 * context, so the AI stops seeing the stale last value.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object")
	bool RemoveTrackedProperty(FName PropertyPath);

	/**
	 * Change the metadata on an already-tracked property (its Description,
	 * StateValueDescriptions, or ShouldRespond mode). The property path itself
	 * is the key — you cannot change it via this method; remove and re-add
	 * if you need to point at a different property.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object")
	bool UpdateTrackedProperty(FName PropertyPath, const FConvaiTrackedProperty& NewSettings);

	/** Returns a copy of the tracked-property list, for inspection from Blueprint. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object")
	void GetTrackedProperties(TArray<FConvaiTrackedProperty>& OutProperties) const;

	/** Appends the sanitized context key of every tracked property on this
	 *  component ("FrontDoor.DoorState") — exactly the keys chatbots see in
	 *  their context states. Used by the "Watch Property" built-in action to
	 *  resolve an LLM-provided name to a real key. */
	void AppendTrackedPropertyContextKeys(TArray<FString>& OutKeys) const;

	/** Effective synthetic movement key: `<ObjectName>.Movement`. */
	FString BuildMovementStateKey() const;

	/** True when an authored tracked property already owns the synthetic
	 *  movement key. In that case movement-state exposure is suppressed. */
	bool HasMovementStateKeyCollision() const;

	// ── Gather support ───────────────────────────────────────────────

	/** Composes the description string a chatbot uses when it pulls this object into its environment. */
	FString ComposeDescriptionForLLM() const;

	/**
	 * Subsystem-driven shared-clock callback. Reads each tracked property's
	 * current formatted value, compares with the cached snapshot, and on change
	 * pushes SetContextState onto every chatbot returned by UConvaiSubsystem.
	 */
	void EvaluateTrackedProperties();

	/** Used by chatbots that came up *after* this component, so they can backfill state from the cache. */
	void SeedInitialStateOntoChatbot(UConvaiChatbotComponent* Chatbot) const;

	/** Returns true when ObjectEntry.Name is non-empty (we refuse to register empty-named objects). */
	bool HasValidObjectName() const { return !ObjectEntry.Name.IsEmpty(); }

	/**
	 * Fills Out with the chatbots that should receive this object's state-tracking + gaze
	 * notifications: every chatbot the subsystem knows about right now. Single source of
	 * truth — both BroadcastToAllChatbots and the gaze NotifyGazeAttention* methods route
	 * through this.
	 */
	void GatherEligibleChatbots(TArray<UConvaiChatbotComponent*>& Out) const;

	// ── Gaze events ──────────────────────────────────────────────────

	/**
	 * Opt-out for gaze. When false, UConvaiPlayerComponent's gaze pipeline (line trace
	 * matching AND dot-product fallback) skips this object entirely — no highlight, no
	 * OnGazedIn/Out events, no attention promotion. Use it to mark Convai objects that
	 * should exist in the chatbot's environment (for action_config / state tracking) but
	 * shouldn't be reachable via player gaze (background props, scenery, etc.).
	 *
	 * Default true — preserves the "every Convai object is gazeable" behavior.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object|Gaze")
	bool bGazeable = true;

	/**
	 * Purely cosmetic, and only relevant when this object is part of a merged set
	 * (see "Merge With Same-Named Objects"). By default, gazing at any member of a
	 * merged set highlights the WHOLE set together. Turn this ON to instead highlight
	 * only the single object actually being looked at.
	 *
	 * This changes the visual highlight ONLY — the entire merged set still enters the
	 * AI's attention and is still treated as one logical object. Has no effect on
	 * non-merged objects (there is nothing else to highlight).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object|Gaze",
		meta = (DisplayName = "Highlight Only This Object When Gazed",
		        EditCondition = "bMergeWithSameNamedObjects"))
	bool bHighlightOnlyThisWhenGazed = false;

	/** Fired the instant a player's gaze enters this object (before any attention threshold). */
	UPROPERTY(BlueprintAssignable, Category = "Convai|Object|Gaze")
	FConvaiObjectGazeEvent OnGazedIn;

	/** Fired the instant a player's gaze leaves this object. */
	UPROPERTY(BlueprintAssignable, Category = "Convai|Object|Gaze")
	FConvaiObjectGazeEvent OnGazedOut;

	/** Fired when this object becomes a chatbot's in-attention target via the gaze threshold. */
	UPROPERTY(BlueprintAssignable, Category = "Convai|Object|Gaze")
	FConvaiObjectGazeEvent OnAttentionGained;

	/** Fired when this object is released from the in-attention slot. */
	UPROPERTY(BlueprintAssignable, Category = "Convai|Object|Gaze")
	FConvaiObjectGazeEvent OnAttentionLost;

	// ── Gaze attention ───────────────────────────────────────────────

	/** Called by UConvaiPlayerComponent when its gaze enters this object's bounds (highlight on). */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object|Gaze")
	void NotifyGazeBegin(UConvaiPlayerComponent* Player);

	/** Companion to NotifyGazeBegin — called when the player's gaze leaves this object. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object|Gaze")
	void NotifyGazeEnd(UConvaiPlayerComponent* Player);

	/**
	 * Called by UConvaiPlayerComponent when the player's gaze has dwelled on this object
	 * long enough to promote it to "object in attention". Fans out to every chatbot the
	 * subsystem currently knows about, asking each to take the slot via its gaze-gated
	 * setter. Each chatbot independently decides whether to accept based on its own
	 * AttentionSource (gaze can't trample a slot owned by an Explicit BP/C++ set).
	 *
	 * Also BlueprintCallable so non-gaze flows (cinematic cameras, custom focus systems)
	 * can drive the same path.
	 *
	 * @param Player        The player whose gaze promoted this object (may be null
	 *                      for custom focus systems).
	 * @param Text          Optional extra sentence appended to the attention cue.
	 * @param ShouldRespond Auto lets the server decide whether to react; Always
	 *                      requests a reaction; Never updates silently.
	 * @param Delivery      Wait Until Conversation Is Idle defers the attention cue
	 *                      until the conversation pauses (Auto/Always only);
	 *                      looking away first cancels the held cue.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object|Gaze")
	void NotifyGazeAttentionBegin(UConvaiPlayerComponent* Player, const FString& Text, EC_RunLLMOption ShouldRespond,
		EConvaiContextDelivery Delivery = EConvaiContextDelivery::SendNormally);

	/**
	 * Companion to NotifyGazeAttentionBegin — called when a player has stopped looking
	 * at this object long enough to release the slot. Fans out the gaze-gated clear to
	 * every chatbot. Safe to call when no chatbot currently holds gaze-attention on us.
	 * Player may be null in destroyed-target paths; OnAttentionLost receivers must null-check.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object|Gaze")
	void NotifyGazeAttentionEnd(UConvaiPlayerComponent* Player = nullptr);

	// ── Debug ────────────────────────────────────────────────────────

	/**
	 * Debug helper. When ON, each subsystem poll (typically 0.25 s) recomputes
	 * the nav path from every registered chatbot to this object and redraws
	 * it — green when the chatbot can actually reach this object, red when
	 * not, cyan when it is already there. Lines persist until the next
	 * recompute. Each object component owns its own ULineBatchComponent so
	 * multiple components with this toggle on don't clobber each other.
	 *
	 * Editor / debug only — leave OFF in shipping.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object|Debug")
	bool bDebugDrawProximityPaths = false;

	// ── UActorComponent overrides ────────────────────────────────────

	virtual void PostLoad() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	virtual void PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedChainEvent) override;
#endif

private:
	/** BeginPlay's registration body: registry + push/seed onto live chatbots.
	 *  Shared with SetConvaiObjectEnabled(true). */
	void RegisterWithConvai();

	/** EndPlay's teardown body: registry removal + per-chatbot cleanup (state
	 *  keys, environment entry / merged-set sibling rebuild, attention slot).
	 *  Shared with SetConvaiObjectEnabled(false); idempotent. */
	void UnregisterFromConvai();

	struct FCachedEntry
	{
		FName PropertyPath;
		FString LastFormattedValue;
		/** False while the path doesn't currently resolve — NOT terminal: the
		 *  poll keeps retrying, since object-reference hops legitimately go
		 *  null/valid over an object's lifetime (spawn order, destruction). */
		bool bResolved = false;
		/** True once the path resolved at least once (rebuild or poll). Lets the
		 *  late-resolve seed fire even when the first value formats to "" —
		 *  LastFormattedValue starts empty, so a value-diff check alone can't
		 *  tell "never seeded" from "empty value". */
		bool bEverResolved = false;
	};

	/** Parallel to TrackedProperties; rebuilt whenever the array shape changes. */
	TArray<FCachedEntry> Cache;

	/** World time (seconds) when BeginPlay seeded the cache. Tracked-property
	 *  changes detected within InitialGracePeriodSec of this time are
	 *  broadcast as EC_RunLLMOption::Never regardless of the prop's own
	 *  ShouldRespond — so game-startup logic mutating values during the
	 *  BeginPlay cascade doesn't make every chatbot react. Sentinel -1
	 *  means BeginPlay hasn't run yet (no grace gate applied). */
	double BeginPlayTimeSec = -1.0;

	/** Length of the post-BeginPlay grace window where change broadcasts
	 *  are forced to Never. Tweak here if your game's startup cascade
	 *  takes longer than ~1 s to settle. */
	static constexpr float InitialGracePeriodSec = 1.0f;

	void RebuildCache();

	/** Effective state key: "<ObjectName>.<Alias-or-PropertyPath>". The chatbot
	 *  sanitizes it (whitespace Pascal-cased away) on receipt, so removal and
	 *  broadcast agree as long as both go through this. */
	FString BuildStateKey(const FConvaiTrackedProperty& Prop) const;

#if WITH_EDITOR
	/** Best-effort re-latch of tracked-property paths whose Blueprint variable
	 *  (or user-defined-struct member) was renamed, via the per-segment GUIDs
	 *  captured at bind time (FConvaiTrackedProperty::PropertyPathGuids). Runs
	 *  at the top of RebuildCache and on PostEditChangeChainProperty; also
	 *  backfills GUIDs for resolvable segments that don't have one yet. */
	void HealTrackedPropertyPaths();
#endif

	/** Pushes the current value for one tracked property onto every chatbot. Used for the seed and the change broadcasts. */
	void BroadcastToAllChatbots(const FConvaiTrackedProperty& Prop, const FString& StateKey,
		const FString& Value, EC_RunLLMOption RunLLMOption, bool bFlushImmediately = false);

	// ── Proximity debug draw (bDebugDrawProximityPaths) ──────────────

	struct FProximityDebugCache
	{
		/** Chatbot + object locations at the last pathfind. The 100 uu
		 *  position-delta gate uses these as the reference; while neither end
		 *  has moved >100 uu since the last pathfind, the cached path is
		 *  reused and stays on screen. */
		FVector LastChatbotLocation = FVector::ZeroVector;
		FVector LastObjectLocation  = FVector::ZeroVector;
		bool    bReachable    = false;
		bool    bAlreadyThere = false; // Snapshot of ResolveGoalLocation's arrival check; drives line colour.
		bool    bInitialized  = false;

		/** World-space nav path from this chatbot to the object, populated on
		 *  every recompute. Retained between recomputes so RefreshProximityDebugDraw
		 *  can redraw it after a flush even on ticks where this chatbot was
		 *  pathfind-skip-gated. */
		TArray<FVector> LastPathPoints;
	};

	/** Per-chatbot debug-draw state. Entries with stale weak pointers are
	 *  pruned at the top of UpdateProximityDebugPaths(). */
	TMap<TWeakObjectPtr<UConvaiChatbotComponent>, FProximityDebugCache> ProximityDebugCaches;

	/** Per-component persistent-debug-line batcher used by RefreshProximityDebugDraw.
	 *  Lazily created on first draw; each component owns its own so toggling
	 *  bDebugDrawProximityPaths on multiple components doesn't cross-flush. */
	UPROPERTY(Transient)
	TObjectPtr<ULineBatchComponent> ProximityDebugBatcher;

	/** Recomputes each chatbot's nav path to this object and redraws when
	 *  anything changed. Runs from EvaluateTrackedProperties on the subsystem
	 *  poll tick (typically 0.25 s), only while bDebugDrawProximityPaths is
	 *  on; each chatbot's pathfind is gated by the 100 uu position-delta
	 *  check so idle scenes pay almost nothing. */
	void UpdateProximityDebugPaths();

	/** Flushes the component's line batcher and redraws cached nav paths for
	 *  every still-valid chatbot in ProximityDebugCaches. Called by
	 *  UpdateProximityDebugPaths after the per-chatbot loop, only when at
	 *  least one chatbot recomputed this pass (otherwise the prior persistent
	 *  lines remain). */
	void RefreshProximityDebugDraw();
};
