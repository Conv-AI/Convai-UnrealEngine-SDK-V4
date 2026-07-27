// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "ConvaiContextSubsystem.generated.h"

class UConvaiSubsystem;
class UConvaiChatbotComponent;
class UConvaiObjectComponent;

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

	/** Forget one observer's published spatial baseline. The next poll rebuilds
	 *  it silently; used for a fresh session and Reset Dynamic Context. */
	void ResetObserverSpatialContext(UConvaiChatbotComponent* Observer);

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

	// One group-level detector per logical object name. Member transforms are
	// sampled on the same shared clock as tracked properties; no object ticks.
	struct FObjectMotionCache
	{
		struct FMemberPose
		{
			struct FTransformSample
			{
				FVector Location = FVector::ZeroVector;
				FQuat Rotation = FQuat::Identity;
				double TimeSeconds = -1.0;
			};

			/** Resolved scene component, or owning actor for whole-actor scope. */
			TWeakObjectPtr<UObject> Target;
			FVector LastLocation = FVector::ZeroVector;
			FQuat LastRotation = FQuat::Identity;
			double LastSampleTime = -1.0;
			FVector DirectionEpisodeAnchorLocation = FVector::ZeroVector;
			FQuat DirectionEpisodeAnchorRotation = FQuat::Identity;
			FVector BoundedSettlingAnchorLocation = FVector::ZeroVector;
			FQuat BoundedSettlingAnchorRotation = FQuat::Identity;
			/** Last detector profile. A runtime sensitivity edit establishes a
			 *  fresh baseline instead of turning old samples into a gameplay edge. */
			uint8 Sensitivity = 0;
			bool bHasSensitivity = false;
			/** Short transform history used to cancel small back-and-forth physics
			 *  jitter without hiding slow movement that keeps making progress. */
			TArray<FTransformSample> TrendSamples;
		};

		TMap<TWeakObjectPtr<UConvaiObjectComponent>, FMemberPose> Members;
		bool bMoving = false;
		double CandidateSince = -1.0;
		double InitializedAt = -1.0;

		// Stable semantic velocity used for low-churn observer-relative wording.
		FVector SemanticVelocity = FVector::ZeroVector;
		bool bRotationOnly = false;
		double LastSemanticUpdate = -1.0;
		// True only when the object's active world-space motion direction/mode
		// meaningfully changed this poll, not when the observer merely turned.
		bool bSemanticChangedThisPoll = false;
		// Prospective semantic relabel. Only raw+rolling-coherent aggregate motion
		// contributes progress; quick bounded rebounds may settle without replacing
		// the last committed direction.
		bool bDirectionCandidateActive = false;
		FVector DirectionCandidateVelocity = FVector::ZeroVector;
		bool bDirectionCandidateRotationOnly = false;
		double DirectionCandidateEpisodeSince = -1.0;
		double DirectionCandidateCoherentSince = -1.0;
		double LastDirectionEvaluationTime = -1.0;
		float DirectionCandidateProgress = 0.0f;
		float DirectionCandidateRequiredProgress = 0.0f;
		bool bDirectionCandidateHadCoherenceLoss = false;
		int32 DirectionCandidateSwitchCount = 0;
		bool bDirectionCandidateEscaped = false;
		FVector DirectionEpisodeOriginalVelocity = FVector::ZeroVector;
		bool bDirectionEpisodeOriginalRotationOnly = false;
		bool bBoundedSettlingLatched = false;
		bool bBoundedSettlingEscapePending = false;

		void ResetDirectionCandidate()
		{
			bDirectionCandidateActive = false;
			DirectionCandidateVelocity = FVector::ZeroVector;
			bDirectionCandidateRotationOnly = false;
			DirectionCandidateEpisodeSince = -1.0;
			DirectionCandidateCoherentSince = -1.0;
			LastDirectionEvaluationTime = -1.0;
			DirectionCandidateProgress = 0.0f;
			DirectionCandidateRequiredProgress = 0.0f;
			bDirectionCandidateHadCoherenceLoss = false;
			DirectionCandidateSwitchCount = 0;
			bDirectionCandidateEscaped = false;
			DirectionEpisodeOriginalVelocity = FVector::ZeroVector;
			bDirectionEpisodeOriginalRotationOnly = false;
		}

		void CaptureDirectionEpisodeAnchors()
		{
			for (TPair<TWeakObjectPtr<UConvaiObjectComponent>, FMemberPose>& Pair
				: Members)
			{
				// Include the sample that activated/rebased the candidate. The
				// evaluator has already advanced LastLocation by this point; using
				// it alone would let an arbitrarily large first step disappear
				// outside the bounded-settling envelope.
				const int32 PreviousSampleIndex =
					Pair.Value.TrendSamples.Num() - 2;
				if (PreviousSampleIndex >= 0)
				{
					Pair.Value.DirectionEpisodeAnchorLocation =
						Pair.Value.TrendSamples[PreviousSampleIndex].Location;
					Pair.Value.DirectionEpisodeAnchorRotation =
						Pair.Value.TrendSamples[PreviousSampleIndex].Rotation;
				}
				else
				{
					Pair.Value.DirectionEpisodeAnchorLocation =
						Pair.Value.LastLocation;
					Pair.Value.DirectionEpisodeAnchorRotation =
						Pair.Value.LastRotation;
				}
			}
		}

		void CaptureBoundedSettlingLatch()
		{
			bBoundedSettlingLatched = true;
			bBoundedSettlingEscapePending = false;
			for (TPair<TWeakObjectPtr<UConvaiObjectComponent>, FMemberPose>& Pair
				: Members)
			{
				Pair.Value.BoundedSettlingAnchorLocation =
					Pair.Value.LastLocation;
				Pair.Value.BoundedSettlingAnchorRotation =
					Pair.Value.LastRotation;
			}
		}

		void ClearBoundedSettlingLatch()
		{
			bBoundedSettlingLatched = false;
			bBoundedSettlingEscapePending = false;
		}
		// Structural rebaselining while already moving remains a silent baseline
		// until real post-reinit motion is observed. This suppresses both the first
		// recovered direction watch and a synthetic stop on a stationary new target.
		bool bAwaitingPostReinitMotion = false;
		FVector LastCentroid = FVector::ZeroVector;
		uint8 TransitionThisPoll = 0; // ConvaiSpatial::EMotionTransition
		bool bReinitializedThisPoll = false;

		// Whether the synthetic `<Object>.Movement` state existed last poll.
		bool bStateExposed = false;
		FString StateKey;
	};
	TMap<FString, FObjectMotionCache> ObjectMotionCaches;

	/** Sample logical object transforms, confirm start/stop edges, maintain the
	 *  optional watchable Movement state, and cache motion for spatial wording. */
	void EvaluateObjectMovement();

	/** Publish Movement states after spatial facts have been staged, so Flush
	 *  Immediately sends the stable state and observer-relative detail together. */
	void PublishObjectMovementStates();

	// ── Spatial awareness ────────────────────────────────────────────
	// Per-observer record of the Context Facts we last published, keyed by
	// stable per-subject key. Used to change-gate (only re-emit when a fact's
	// sentence changes) and to remove facts for subjects that drop out of range/scene.
	struct FObserverSpatialCache
	{
		// The first complete spatial/movement view is baseline knowledge, not a
		// world event. It stays silent until its queued keys have actually left
		// the debounce batch; pre-connect motion must not upgrade startup context.
		// The gate is observer/session scoped even when no spatial subject is
		// currently visible, because synthetic Movement can still change.
		bool bInitialBaselineQueued = false;
		bool bInitialBaselineDelivered = false;

		// Lets a runtime off -> on transition get a fresh silent spatial snapshot
		// without repeatedly resetting the session-wide Movement baseline while
		// spatial awareness remains disabled.
		bool bSpatialAwarenessWasActive = false;

		// Sentence as published, plus the subject's display name (kept so the
		// debug overlay can label rows without parsing sentences).
		struct FFact
		{
			FString Name;
			FString Sentence;
		};
		TMap<FString, FFact> Facts;

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
			bool    bAlreadyThere   = false;
			bool    bValid          = false;

			// Previous POLL positions (unlike Last*Loc above, which are the last
			// PATHFIND positions). A drift smaller than the 100 uu recompute gate
			// can flip reachability and then settle, leaving bReachable stale
			// indefinitely — so the poll loop tracks per-poll motion and forces
			// one recompute on the poll where motion stops.
			FVector PrevPollObserverLoc = FVector::ZeroVector;
			FVector PrevPollObjectLoc   = FVector::ZeroVector;
			bool    bWasMoving          = false;
		};
		TMap<FString, FNavCacheEntry> NavCache;
	};
	TMap<TWeakObjectPtr<UConvaiChatbotComponent>, FObserverSpatialCache> SpatialCaches;

	/** Compose, for every chatbot, one Context Fact per nearby subject (objects,
	 *  other characters, players) describing where it is relative to the chatbot
	 *  and how it relates to neighbours, honouring per-chatbot preferences and
	 *  the project settings. Driven each poll tick. */
	void EvaluateSpatialAwareness();

	// ── Nav-update-driven reachability invalidation ──────────────────
	// A navmesh rebuild can flip reachability with nothing moving (a door
	// opens), which the motion-gated NavCache would never notice. Each
	// completed rebuild stamps a time; once nav has been QUIET for a short
	// settle window, the poll re-checks every cached verdict once. Bursts of
	// tile updates keep pushing the window, collapsing to one sweep — and a
	// perpetually-rebuilding scene (a patrolling nav obstacle) defers to the
	// motion gates, which already track everything near the mover.

	/** Fires when the world's navmesh generator drains its task queue —
	 *  including runtime dynamic tile rebuilds. Just stamps the time. */
	UFUNCTION()
	void OnNavGenerationFinished(class ANavigationData* NavData);

	/** The nav system currently bound — a weak ptr, not a bool: map travel
	 *  replaces the nav system, and the poll must rebind to the new one. */
	TWeakObjectPtr<class UNavigationSystemV1> BoundNavSys;

	/** FPlatformTime of the last completed rebuild; < 0 = nothing pending. */
	double NavChangedAtSeconds = -1.0;

	/** Backup nav-change detector: the generation-finished event only fires
	 *  when the WHOLE build queue drains, which can lag seconds behind in a
	 *  busy scene. A registered object whose geometry can affect navigation
	 *  MOVING is ground truth that nav is changing — the union of its
	 *  nav-relevant primitives' bounds is compared each poll (bounds catch a
	 *  door leaf swinging while the actor origin stays put) and any change
	 *  stamps the same settle clock. */
	struct FNavRelevantPose
	{
		bool bEvaluated = false;
		/** The owner's nav-relevant primitives, cached at first sight — the
		 *  signature unions THESE bounds, not GetActorBounds, which triggers
		 *  and effect volumes would pollute. Rescanned if they all die. */
		TArray<TWeakObjectPtr<class UPrimitiveComponent>> NavRelevantPrims;
		FVector LastOrigin = FVector::ZeroVector;
		FVector LastExtent = FVector::ZeroVector;
		// Sum of prim locations — catches movement INSIDE a stable union box
		// (a leaf rotating within a static frame's bounds).
		FVector LastLocSum = FVector::ZeroVector;
	};
	TMap<TWeakObjectPtr<class UConvaiObjectComponent>, FNavRelevantPose> NavRelevantPoses;

public:
	/** One row of GetDebugSpatialFacts: a subject's published fact, verbatim. */
	struct FConvaiDebugSpatialFact
	{
		FString Key;       // stable cache key (Object:/Chatbot:/Player:)
		FString Name;      // subject display name
		FString Sentence;  // exactly what the AI was told
		bool bReachable = true;     // cached nav verdict (true when none yet)
		bool bReached = false;      // observer is already at the subject
		bool bPendingFlush = false; // staged in the debounce batch, not sent yet
	};

	/** Debug read: every spatial fact currently published for Observer —
	 *  objects, other characters, and players — exactly as the AI sees them.
	 *  Subjects the pass told the AI nothing about are simply absent. */
	void GetDebugSpatialFacts(const class UConvaiChatbotComponent* Observer,
		TArray<FConvaiDebugSpatialFact>& Out) const;
};
