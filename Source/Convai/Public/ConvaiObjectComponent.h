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

};
