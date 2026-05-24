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
	 * member, etc.), describe what it means, optionally describe what each
	 * value means, and choose how the chatbot should react to changes.
	 *
	 * The current value is sent to all chatbots at session start, then again
	 * every time it changes. The AI sees them as keys like
	 * "<this object's name>.<property name>".
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object")
	TArray<FConvaiTrackedProperty> TrackedProperties;

	/**
	 * When ON (default), the component synthesises a per-chatbot "Proximity"
	 * state that describes where this object is relative to each chatbot in
	 * plain language — "close by, in front and to the right", "far away,
	 * behind me", "out of reach, to the left and above", etc. The chatbot
	 * sees it as the state key "<this object's name>.Proximity" and treats
	 * it the same as any other tracked property.
	 *
	 * The shared poll evaluates per-chatbot stability every tick, defers
	 * broadcasts while the chatbot/object is moving, and reuses the last
	 * reachability result until either endpoint moves more than ~100 uu — so
	 * idle scenes pay almost nothing. Reachability uses UE's navigation system
	 * (partial paths allowed); a path that ends within ~300 uu of this object's
	 * bounding box on the horizontal plane and within the chatbot owner actor's
	 * height vertically counts as reachable. ShouldRespond is always Never —
	 * the LLM is silently informed and won't speak about it on its own.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object")
	bool bAutoGenerateProximityState = true;

	/**
	 * Returns this object's targeted scene component if ObjectEntry has a component
	 * filter (resolves lazily via FConvaiObjectEntry::ResolveComponent and caches the
	 * result), or nullptr when the object is whole-actor scope. The player component
	 * uses this to decide whether the gaze is hitting the right primitive.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object")
	USceneComponent* GetResolvedComponent(bool bForceRefresh = false);

	// ── Blueprint mutators ───────────────────────────────────────────

	/**
	 * Add a new tracked property at runtime — useful for properties that only
	 * become relevant once the player has done something (picked up an item,
	 * entered a room). The property's current value is pushed to all chatbots
	 * immediately. Returns false if the property path doesn't exist on this
	 * Actor, can't be tracked (unsupported type), or is already in the list.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object")
	bool AddTrackedProperty(const FConvaiTrackedProperty& InProperty);

	/**
	 * Stop tracking a property at runtime. Returns true if it was removed.
	 * Already-broadcast values stay in the chatbot's context (it has no way to
	 * "forget" the last value seen) — they simply stop updating.
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
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Object|Gaze")
	void NotifyGazeAttentionBegin(UConvaiPlayerComponent* Player, const FString& Text, EC_RunLLMOption ShouldRespond);

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
	 * Debug helper. When ON, every proximity re-evaluation flushes the
	 * component's persistent debug-line buffer and redraws the nav path from
	 * each subscribed chatbot to this object — green when the chatbot can
	 * actually reach this object, red when not. Lines persist until the next
	 * re-evaluation. Each object component owns its own ULineBatchComponent
	 * so multiple components with this toggle on don't clobber each other.
	 *
	 * Editor / debug only — leave OFF in shipping. Has no effect when
	 * bAutoGenerateProximityState is also OFF.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object|Debug")
	bool bDebugDrawProximityPaths = false;

	// ── UActorComponent overrides ────────────────────────────────────

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	virtual void PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedChainEvent) override;
#endif

private:
	struct FCachedEntry
	{
		FName PropertyPath;
		FString LastFormattedValue;
		bool bResolved = false;
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
	FString BuildStateKey(const FName& PropertyPath) const;

	/** Pushes the current value for one tracked property onto every chatbot. Used for the seed and the change broadcasts. */
	void BroadcastToAllChatbots(const FConvaiTrackedProperty& Prop, const FString& StateKey, const FString& Value, EC_RunLLMOption RunLLMOption);

	// ── Proximity (per-chatbot synthesised state) ────────────────────
	//
	// Distinct from TrackedProperties because the value depends on each
	// individual chatbot's location relative to this object — TrackedProperties
	// reads from the object's own UPROPERTYs and broadcasts the same value to
	// every chatbot. The Proximity computation runs on the shared poll tick,
	// defers broadcasts while motion is unstable, and gates expensive path
	// recomputation on a ~100 uu position-delta check so idle scenes pay almost
	// nothing.

	struct FProximityCache
	{
		// ── Per-pathfind cache (100 uu skip gate) ────────────────
		// Last chatbot + object locations at which reachability + distance-band
		// were recomputed. The 100 uu position-delta gate uses these as the
		// reference; while they haven't moved >100 uu since the last pathfind,
		// LastBand + bReachable are reused. Direction is NOT cached — it
		// depends on the chatbot's rotation and is cheap to rebuild.
		FVector LastChatbotLocation = FVector::ZeroVector;
		FVector LastObjectLocation  = FVector::ZeroVector;
		uint8   LastBand     = 0; // EProximityBand cast to uint8 (struct can't see the enum scoped to the cpp)
		bool    bReachable   = false;
		bool    bAlreadyThere = false; // Snapshot of ResolveGoalLocation's arrival check; drives debug-draw colour.

		// ── Per-tick stability sample (motion gate) ──────────────
		// Captured at every subsystem poll tick (typically 0.25 s) regardless
		// of whether the eval proceeded. The stability gate compares the
		// current poll sample against these to decide "did anything move /
		// rotate since last tick"; if yes the proximity update is deferred,
		// up to ProximityMaxDeferrals consecutive ticks, then forced through
		// so a chatbot in sustained motion still hears about its surroundings
		// eventually.
		FVector TickChatbotLoc = FVector::ZeroVector;
		FVector TickObjectLoc  = FVector::ZeroVector;
		FQuat   TickChatbotRot = FQuat::Identity;
		int32   DeferralCount  = 0;
		bool    bHasTickSample = false;

		FString LastPublishedValue;
		bool    bInitialized = false;

		/** World-space nav path from this chatbot to the object, populated on
		 *  every recompute. Retained between recomputes so RefreshProximityDebugDraw
		 *  can redraw it after a flush even on ticks where this chatbot was
		 *  pathfind-skip-gated. */
		TArray<FVector> LastPathPoints;
	};

	/** Per-chatbot proximity state. Entries with stale weak pointers are
	 *  pruned at the top of EvaluateProximityForAllChatbots(). */
	TMap<TWeakObjectPtr<UConvaiChatbotComponent>, FProximityCache> ProximityCaches;

	/** Per-component persistent-debug-line batcher used by RefreshProximityDebugDraw.
	 *  Lazily created on first draw; each component owns its own so toggling
	 *  bDebugDrawProximityPaths on multiple components doesn't cross-flush. */
	UPROPERTY(Transient)
	TObjectPtr<ULineBatchComponent> ProximityDebugBatcher;

	/** Runs the per-chatbot proximity computation when enabled. Fires on every
	 *  subsystem poll tick (typically 0.25 s); each chatbot's eval is then
	 *  gated by (a) a stability check vs the previous tick sample and (b) the
	 *  100 uu pathfind-skip gate. */
	void EvaluateProximityForAllChatbots();

	/** Updates the cache and pushes "<ObjectName>.Proximity" onto a single
	 *  chatbot when its value would change. ObjectLoc is hoisted by the caller
	 *  because it doesn't vary between chatbots within one pass. The bounds /
	 *  reachability are recomputed inside ResolveGoalLocation per-chatbot.
	 *  Returns true when this chatbot's nav path was recomputed on this call —
	 *  used by the caller to decide whether to refresh the debug draw. */
	bool EvaluateProximityForChatbot(UConvaiChatbotComponent* Chatbot,
		const FVector& ObjectLoc);

	/** Flushes the component's line batcher and redraws cached nav paths for
	 *  every still-valid chatbot in ProximityCaches. Called by
	 *  EvaluateProximityForAllChatbots after the per-chatbot loop, only when
	 *  bDebugDrawProximityPaths is on AND at least one chatbot recomputed
	 *  this pass (otherwise the prior persistent lines remain). */
	void RefreshProximityDebugDraw();
};
