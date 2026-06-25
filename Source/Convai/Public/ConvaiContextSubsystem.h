// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "ConvaiContextSubsystem.generated.h"

class UConvaiSubsystem;
class UConvaiChatbotComponent;

/**
 * Game-instance subsystem that owns the shared poll clock driving Convai's
 * dynamic-context generation — and, in later phases, the spatial-awareness pass
 * that composes the per-chatbot Context Facts (proximity, line-of-sight,
 * entity relations).
 *
 * The poll clock was lifted out of UConvaiSubsystem to keep that class focused
 * on connection/session work. The component REGISTRIES still live on
 * UConvaiSubsystem; this subsystem owns only the *timer* and asks UConvaiSubsystem
 * to do the per-tick work via UConvaiSubsystem::PollObjectComponents(). The clock
 * starts lazily when the first object component registers and stops itself once
 * the last one is gone, so a level with no Convai objects pays nothing.
 */
UCLASS()
class CONVAI_API UConvaiContextSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Begin USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End USubsystem

	/**
	 * (Re)start the shared object-poll ticker at the current effective interval.
	 * Idempotent. Called by UConvaiSubsystem when an object component registers.
	 * No-op when there are no registered object components yet.
	 */
	void RestartObjectPollClock();

	/**
	 * Stop the shared object-poll ticker. Called by UConvaiSubsystem when the
	 * last object component unregisters (and from Deinitialize).
	 */
	void StopObjectPollClock();

private:
	/** Shared poll-clock handle (core ticker). */
	FTSTicker::FDelegateHandle ObjectPollTickerHandle;

	/** Pass-through to UConvaiUtils::GetObjectPollIntervalSeconds(). Kept as a
	 *  single seam in case future cadence policy needs to layer on top. */
	float ComputeEffectiveObjectPollInterval() const;

	/** Ticker callback: polls the object components (tracked properties) and runs
	 *  the spatial-awareness pass, then keeps ticking while any objects OR
	 *  chatbots remain (chatbots are observers, so the clock must run for them
	 *  even with no objects in the level). */
	bool TickObjectPollClock(float DeltaTime);

	/** Resolve the sibling UConvaiSubsystem, which holds the component registries. */
	UConvaiSubsystem* GetConvaiSubsystem() const;

	// ── Spatial awareness ────────────────────────────────────────────
	// Per-observer record of the Context Facts we last published, keyed by
	// stable per-subject key. Used to change-gate (only re-emit when a fact's
	// sentence changes) and to remove facts for subjects that drop out of range/scene.
	struct FObserverSpatialCache
	{
		TMap<FString, FString> Facts;

		// Per-subject nav-reachability cache (objects only). Reused until the
		// observer or the object's centroid moves more than ~100 uu, OR the merged
		// set's member count changes, so the expensive nav pathfind doesn't run every
		// poll — mirrors the old per-object proximity skip gate. (For a single-member
		// object this is exactly the old per-object behaviour.)
		struct FNavCacheEntry
		{
			FVector LastObserverLoc = FVector::ZeroVector;
			FVector LastObjectLoc   = FVector::ZeroVector;
			int32   LastMemberCount = 0;
			bool    bReachable      = true;
			bool    bValid          = false;
		};
		TMap<FString, FNavCacheEntry> NavCache;
	};
	TMap<TWeakObjectPtr<UConvaiChatbotComponent>, FObserverSpatialCache> SpatialCaches;

	/** Compose, for every chatbot, one Context Fact per nearby subject (objects,
	 *  other characters, players) describing where it is relative to the chatbot
	 *  and how it relates to neighbours, honouring per-chatbot preferences and
	 *  the project settings. Driven each poll tick. */
	void EvaluateSpatialAwareness();
};
