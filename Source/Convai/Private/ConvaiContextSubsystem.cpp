// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiContextSubsystem.h"

#include "ConvaiSubsystem.h"
#include "ConvaiUtils.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiObjectComponent.h"
#include "ConvaiConversationComponent.h"
#include "../Convai.h"
#include "Utility/ConvaiContextFormat.h"
#include "Utility/ConvaiSpatial.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "AI/Navigation/NavAgentInterface.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"

namespace ConvaiContextPrivate
{
	float ComputeNearestDistance(
		const FVector& ObserverLocation,
		const TConstArrayView<FVector> CandidateLocations,
		const FVector& FallbackLocation)
	{
		double BestDistanceSquared = TNumericLimits<double>::Max();
		for (const FVector& CandidateLocation : CandidateLocations)
		{
			if (!CandidateLocation.ContainsNaN())
			{
				BestDistanceSquared = FMath::Min(
					BestDistanceSquared,
					FVector::DistSquared(ObserverLocation, CandidateLocation));
			}
		}

		if (BestDistanceSquared == TNumericLimits<double>::Max())
		{
			return static_cast<float>(FVector::Dist(ObserverLocation, FallbackLocation));
		}
		return static_cast<float>(FMath::Sqrt(BestDistanceSquared));
	}
}

namespace
{
	int32 MovementRunLLMRank(EC_RunLLMOption Option)
	{
		switch (Option)
		{
			case EC_RunLLMOption::Always: return 2;
			case EC_RunLLMOption::Auto:   return 1;
			default:                      return 0;
		}
	}

	struct FResolvedMovementSettings
	{
		bool bAwarenessEnabled = false;
		bool bStateEnabled = false;
		bool bStateKeyCollision = false;
		EC_RunLLMOption StartedResponse = EC_RunLLMOption::Never;
		EC_RunLLMOption StoppedResponse = EC_RunLLMOption::Never;
		EConvaiContextDelivery Delivery = EConvaiContextDelivery::SendNormally;
		bool bFlushImmediately = false;
	};

	// Same-named objects can form one logical group. Aggregate every enabled
	// member so behavior is deterministic even if their authored settings differ:
	// strongest response, safest delivery, and any urgent flush win.
	FResolvedMovementSettings ResolveMovementSettings(
		const UConvaiSubsystem::FConvaiObjectGroup& Group)
	{
		FResolvedMovementSettings Result;
		for (UConvaiObjectComponent* Member : Group.Members)
		{
			if (!IsValid(Member))
			{
				continue;
			}
			// Track ownership even while the synthetic state is disabled. If an
			// authored property has claimed the same key, retiring an older movement
			// state must not delete that property's current value.
			Result.bStateKeyCollision |= Member->HasMovementStateKeyCollision();
			const FConvaiObjectMovementSettings& Settings = Member->MovementAwareness;
			if (!Settings.bEnableMovementAwareness)
			{
				continue;
			}

			Result.bAwarenessEnabled = true;
			if (!Settings.bExposeMovementState)
			{
				continue;
			}

			Result.bStateEnabled = true;
			if (MovementRunLLMRank(Settings.StartedMovingResponse)
				> MovementRunLLMRank(Result.StartedResponse))
			{
				Result.StartedResponse = Settings.StartedMovingResponse;
			}
			if (MovementRunLLMRank(Settings.StoppedMovingResponse)
				> MovementRunLLMRank(Result.StoppedResponse))
			{
				Result.StoppedResponse = Settings.StoppedMovingResponse;
			}
			if (Settings.GetEffectiveDelivery()
				== EConvaiContextDelivery::WaitUntilConversationIsIdle)
			{
				Result.Delivery = EConvaiContextDelivery::WaitUntilConversationIsIdle;
			}
			Result.bFlushImmediately |= Settings.GetEffectiveFlushImmediately();
		}
		return Result;
	}
}

void UConvaiContextSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// The clock starts lazily on the first object-component registration
	// (UConvaiSubsystem::RegisterObjectComponent -> RestartObjectPollClock).
	// The nav generation-finished delegate binds lazily in the poll tick,
	// since the nav system may not exist yet.
}

void UConvaiContextSubsystem::Deinitialize()
{
	StopObjectPollClock();
	ObjectMotionCaches.Empty();
	if (UNavigationSystemV1* Bound = BoundNavSys.Get())
	{
		Bound->OnNavigationGenerationFinishedDelegate.RemoveDynamic(
			this, &UConvaiContextSubsystem::OnNavGenerationFinished);
	}
	BoundNavSys.Reset();
	Super::Deinitialize();
}

void UConvaiContextSubsystem::OnNavGenerationFinished(ANavigationData* NavData)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!NavData || !World || NavData->GetWorld() != World)
	{
		return;
	}
	// Just stamp the time — the poll sweeps once nav has settled. (This fires
	// for runtime dynamic tile rebuilds too: recast broadcasts whenever its
	// pending task queue drains.)
	NavChangedAtSeconds = FPlatformTime::Seconds();
}

float UConvaiContextSubsystem::ComputeEffectiveObjectPollInterval() const
{
	// Plain user-configured tick rate. The chatbot's own debounce window already
	// coalesces same-tick context updates, so the poll clock doesn't enforce a
	// floor relative to it — designers get exactly the cadence they ask for.
	return UConvaiUtils::GetObjectPollIntervalSeconds();
}

void UConvaiContextSubsystem::RestartObjectPollClock()
{
	StopObjectPollClock();

	UConvaiSubsystem* Subsystem = GetConvaiSubsystem();
	if (!Subsystem ||
		(Subsystem->GetAllObjectComponents().Num() == 0 &&
		 Subsystem->GetAllChatbotComponents().Num() == 0))
	{
		return; // nothing to evaluate yet (no objects and no chatbot observers)
	}

	const float Interval = ComputeEffectiveObjectPollInterval();
	ObjectPollTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UConvaiContextSubsystem::TickObjectPollClock),
		Interval);
}

void UConvaiContextSubsystem::StopObjectPollClock()
{
	if (ObjectPollTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ObjectPollTickerHandle);
		ObjectPollTickerHandle.Reset();
	}
	if (UConvaiSubsystem* Subsystem = GetConvaiSubsystem())
	{
		if (Subsystem->GetAllObjectComponents().Num() == 0
			&& Subsystem->GetAllChatbotComponents().Num() == 0)
		{
			ObjectMotionCaches.Empty();
		}
	}
}

bool UConvaiContextSubsystem::TickObjectPollClock(float /*DeltaTime*/)
{
	UConvaiSubsystem* Subsystem = GetConvaiSubsystem();
	if (!Subsystem)
	{
		ObjectPollTickerHandle.Reset();
		return false;
	}

	// (Re)bind the rebuild-completed delegate to the CURRENT world's nav
	// system — it usually spawns after this subsystem initializes, and map
	// travel replaces it entirely.
	{
		UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
		UNavigationSystemV1* NavSys = World
			? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
		if (NavSys != BoundNavSys.Get())
		{
			if (UNavigationSystemV1* Old = BoundNavSys.Get())
			{
				Old->OnNavigationGenerationFinishedDelegate.RemoveDynamic(
					this, &UConvaiContextSubsystem::OnNavGenerationFinished);
			}
			if (NavSys)
			{
				NavSys->OnNavigationGenerationFinishedDelegate.AddDynamic(
					this, &UConvaiContextSubsystem::OnNavGenerationFinished);
				// A NEW nav system means a new mesh — verdicts cached against
				// the old one may look valid (same names/positions after
				// seamless travel) but were never checked here. Sweep once.
				NavChangedAtSeconds = FPlatformTime::Seconds();
			}
			BoundNavSys = NavSys;
		}
	}

	// Backup nav-change detector: a nav-affecting registered object MOVING is
	// ground truth that nav is changing — no need to wait for the engine's
	// generation-finished event, which only fires when the whole build queue
	// drains (seconds late in busy scenes). Nav-relevant prim-union bounds,
	// not the actor transform: a door leaf swings while its origin stays put.
	for (auto It = NavRelevantPoses.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid()) { It.RemoveCurrent(); }
	}
	for (UConvaiObjectComponent* Obj : Subsystem->GetAllObjectComponents())
	{
		AActor* ObjOwner = IsValid(Obj) ? Obj->GetOwner() : nullptr;
		if (!IsValid(ObjOwner))
		{
			continue;
		}
		FNavRelevantPose& Pose = NavRelevantPoses.FindOrAdd(Obj);

		bool bAnyPrimAlive = false;
		for (const TWeakObjectPtr<UPrimitiveComponent>& Prim : Pose.NavRelevantPrims)
		{
			bAnyPrimAlive |= Prim.IsValid();
		}
		if (!Pose.bEvaluated || (Pose.NavRelevantPrims.Num() > 0 && !bAnyPrimAlive))
		{
			// First sight (or every cached primitive died): (re)scan and take a
			// baseline — a baseline is not a change.
			Pose.bEvaluated = true;
			Pose.NavRelevantPrims.Reset();
			FBox Box(ForceInit);
			FVector LocSum = FVector::ZeroVector;
			ObjOwner->ForEachComponent<UPrimitiveComponent>(/*bIncludeFromChildActors*/ false,
				[&Pose, &Box, &LocSum](UPrimitiveComponent* Prim)
				{
					if (Prim->IsRegistered() && Prim->IsNavigationRelevant())
					{
						Pose.NavRelevantPrims.Add(Prim);
						Box += Prim->Bounds.GetBox();
						LocSum += Prim->GetComponentLocation();
					}
				});
			if (Box.IsValid)
			{
				Pose.LastOrigin = Box.GetCenter();
				Pose.LastExtent = Box.GetExtent();
				Pose.LastLocSum = LocSum;
			}
			continue;
		}
		if (Pose.NavRelevantPrims.Num() == 0)
		{
			continue; // owner never affects nav
		}
		FBox Box(ForceInit);
		FVector LocSum = FVector::ZeroVector;
		for (const TWeakObjectPtr<UPrimitiveComponent>& PrimPtr : Pose.NavRelevantPrims)
		{
			if (UPrimitiveComponent* Prim = PrimPtr.Get())
			{
				Box += Prim->Bounds.GetBox();
				LocSum += Prim->GetComponentLocation();
			}
		}
		if (!Box.IsValid)
		{
			continue;
		}
		const FVector Origin = Box.GetCenter();
		const FVector Extent = Box.GetExtent();
		if (!Origin.Equals(Pose.LastOrigin, 2.0f) || !Extent.Equals(Pose.LastExtent, 2.0f)
			|| !LocSum.Equals(Pose.LastLocSum, 2.0f))
		{
			Pose.LastOrigin = Origin;
			Pose.LastExtent = Extent;
			Pose.LastLocSum = LocSum;
			NavChangedAtSeconds = FPlatformTime::Seconds();
		}
	}

	// Nav settled? Re-check every cached reachability verdict once. The
	// settle window batches tile-update bursts into one sweep — it can be
	// SHORT, because every completion re-stamps the clock (a long door swing
	// keeps pushing the window; the sweep fires ~this long after the LAST
	// rebuild, not the first).
	constexpr double NavSettleSeconds = 0.3;
	if (NavChangedAtSeconds >= 0.0
		&& FPlatformTime::Seconds() - NavChangedAtSeconds >= NavSettleSeconds)
	{
		NavChangedAtSeconds = -1.0;
		for (TPair<TWeakObjectPtr<UConvaiChatbotComponent>, FObserverSpatialCache>& CachePair : SpatialCaches)
		{
			for (TPair<FString, FObserverSpatialCache::FNavCacheEntry>& NavPair : CachePair.Value.NavCache)
			{
				NavPair.Value.bValid = false;
			}
		}
	}

	// Object tracked-property poll (also prunes destroyed object components).
	Subsystem->PollObjectComponents();

	// Transform-sampled movement: passive spatial wording for awareness-enabled
	// objects, plus the optional durable state for authored reactions/watches.
	EvaluateObjectMovement();

	// Spatial-awareness pass (objects + characters + players, per chatbot).
	EvaluateSpatialAwareness();

	// Publish/flush the Movement state last. Any matching spatial motion fact was
	// staged first, so an urgent transition reaches the LLM as one coherent batch.
	PublishObjectMovementStates();

	// Keep ticking while there's anything to drive: object components (tracked
	// properties) OR chatbots (spatial-awareness observers). Returning false
	// removes this ticker; clear our stored handle to match.
	const bool bHasWork =
		Subsystem->GetAllObjectComponents().Num() > 0 ||
		Subsystem->GetAllChatbotComponents().Num() > 0;
	if (!bHasWork)
	{
		ObjectPollTickerHandle.Reset();
		return false;
	}
	return true; // keep ticking
}

UConvaiSubsystem* UConvaiContextSubsystem::GetConvaiSubsystem() const
{
	if (UGameInstance* GI = GetGameInstance())
	{
		return GI->GetSubsystem<UConvaiSubsystem>();
	}
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Spatial awareness
// ─────────────────────────────────────────────────────────────────────────────

void UConvaiContextSubsystem::EvaluateObjectMovement()
{
	UConvaiSubsystem* Subsystem = GetConvaiSubsystem();
	if (!Subsystem)
	{
		return;
	}

	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	const double NowSeconds = World ? static_cast<double>(World->GetTimeSeconds())
		: FPlatformTime::Seconds();

	TArray<UConvaiSubsystem::FConvaiObjectGroup> Groups;
	Subsystem->BuildObjectGroups(Groups);

	for (const UConvaiSubsystem::FConvaiObjectGroup& Group : Groups)
	{
		if (Group.Name.IsEmpty() || Group.Members.Num() == 0)
		{
			continue;
		}

		TSet<UConvaiObjectComponent*> CurrentMembers;
		for (UConvaiObjectComponent* Member : Group.Members)
		{
			if (!IsValid(Member) || !IsValid(Member->GetOwner()))
			{
				continue;
			}
			if (!Member->MovementAwareness.bEnableMovementAwareness)
			{
				continue;
			}
			CurrentMembers.Add(Member);
		}
		// Leave an old cache for PublishObjectMovementStates to retire cleanly.
		// Do not create a new one when every member has movement awareness off.
		if (CurrentMembers.Num() == 0)
		{
			continue;
		}

		const FString CacheKey = FString::Printf(TEXT("Object:%s"), *Group.Name.ToLower());
		FObjectMotionCache& Cache = ObjectMotionCaches.FindOrAdd(CacheKey);
		Cache.TransitionThisPoll = static_cast<uint8>(ConvaiSpatial::EMotionTransition::None);
		Cache.bReinitializedThisPoll = false;
		Cache.bSemanticChangedThisPoll = false;
		Cache.LastCentroid = Group.Centroid;
		if (Cache.InitializedAt < 0.0 || NowSeconds < Cache.InitializedAt)
		{
			Cache.InitializedAt = NowSeconds;
			Cache.ClearBoundedSettlingLatch();
			for (TPair<TWeakObjectPtr<UConvaiObjectComponent>, FObjectMotionCache::FMemberPose>& Pair : Cache.Members)
			{
				Pair.Value.LastSampleTime = -1.0;
				Pair.Value.TrendSamples.Reset();
			}
		}

		bool bDetectorConfigurationChanged =
			Cache.Members.Num() != CurrentMembers.Num();
		if (!bDetectorConfigurationChanged)
		{
			for (const TPair<TWeakObjectPtr<UConvaiObjectComponent>,
				FObjectMotionCache::FMemberPose>& Pair : Cache.Members)
			{
				if (!Pair.Key.IsValid()
					|| !CurrentMembers.Contains(Pair.Key.Get()))
				{
					bDetectorConfigurationChanged = true;
					break;
				}
			}
		}
		if (bDetectorConfigurationChanged)
		{
			Cache.ClearBoundedSettlingLatch();
			// A merged group's membership is detector configuration, not physical
			// motion. Rebaseline every surviving member so removing the only moving
			// member cannot fabricate a watched/responding Stopped edge.
			for (TPair<TWeakObjectPtr<UConvaiObjectComponent>,
				FObjectMotionCache::FMemberPose>& Pair : Cache.Members)
			{
				if (Pair.Key.IsValid()
					&& CurrentMembers.Contains(Pair.Key.Get()))
				{
					Pair.Value.LastSampleTime = -1.0;
					Pair.Value.TrendSamples.Reset();
				}
			}
		}

		for (auto It = Cache.Members.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid() || !CurrentMembers.Contains(It.Key().Get()))
			{
				It.RemoveCurrent();
			}
		}

		bool bAboveStart = false;
		bool bBelowStop = true;
		bool bAngularTrendQuiet = true;
		bool bAnyAngularMotion = false;
		bool bAnyLinearExcursion = false;
		bool bAnyClearAngularDirectionEvidence = false;
		bool bAnyContinuationAngularDirectionEvidence = false;
		int32 ComparableSamples = 0;
		int32 ValidSamples = 0;
		int32 RebaselinedSamples = 0;
		FVector VelocitySum = FVector::ZeroVector;
		float LinearSpeedSum = 0.0f;
		int32 LinearVelocityCount = 0;
		FVector ClearVelocitySum = FVector::ZeroVector;
		float ClearLinearSpeedSum = 0.0f;
		int32 ClearLinearVelocityCount = 0;
		FVector ContinuationVelocitySum = FVector::ZeroVector;
		float ContinuationLinearSpeedSum = 0.0f;
		int32 ContinuationLinearVelocityCount = 0;
		float ClearLinearExcursionFloor = 0.0f;
		float ClearAngularExcursionFloor = 0.0f;
		float ClearAngularSpeed = 0.0f;
		float ContinuationAngularSpeed = 0.0f;
		bool bDirectionEpisodeInsideEnvelope =
			Cache.bDirectionCandidateActive;
		int32 DirectionEpisodeSamples = 0;
		bool bBoundedLatchInsideReleaseEnvelope =
			Cache.bBoundedSettlingLatched;
		int32 BoundedLatchSamples = 0;

		for (UConvaiObjectComponent* Member : CurrentMembers)
		{
			const uint8 Sensitivity = static_cast<uint8>(
				Member->MovementAwareness.MovementSensitivity);
			const ConvaiSpatial::FMotionThresholds Thresholds =
				ConvaiSpatial::GetMotionThresholds(
					Member->MovementAwareness.MovementSensitivity);
			AActor* Owner = Member->GetOwner();
			USceneComponent* ResolvedComponent = Member->GetResolvedComponent();
			UObject* Target = ResolvedComponent
				? static_cast<UObject*>(ResolvedComponent) : static_cast<UObject*>(Owner);
			const FTransform Transform = ResolvedComponent
				? ResolvedComponent->GetComponentTransform() : Owner->GetActorTransform();
			++ValidSamples;

			FObjectMotionCache::FMemberPose& Pose = Cache.Members.FindOrAdd(Member);
			const bool bTargetChanged = Pose.LastSampleTime >= 0.0
				&& Pose.Target.Get() != Target;
			const bool bSensitivityChanged = Pose.bHasSensitivity
				&& Pose.Sensitivity != Sensitivity;
			bDetectorConfigurationChanged |= bTargetChanged || bSensitivityChanged;
			if (Cache.bDirectionCandidateActive)
			{
				++DirectionEpisodeSamples;
				bDirectionEpisodeInsideEnvelope &=
					!bTargetChanged
					&& !bSensitivityChanged
					&& ConvaiSpatial::IsInsideMotionSettlingEnvelope(
						Pose.DirectionEpisodeAnchorLocation,
						Pose.DirectionEpisodeAnchorRotation,
						Transform.GetLocation(),
						Transform.GetRotation().GetNormalized(),
						Thresholds);
			}
			if (Cache.bBoundedSettlingLatched)
			{
				++BoundedLatchSamples;
				bBoundedLatchInsideReleaseEnvelope &=
					!bTargetChanged
					&& !bSensitivityChanged
					&& ConvaiSpatial::IsInsideMotionSettlingEnvelope(
						Pose.BoundedSettlingAnchorLocation,
						Pose.BoundedSettlingAnchorRotation,
						Transform.GetLocation(),
						Transform.GetRotation().GetNormalized(),
						Thresholds,
						ConvaiSpatial::MotionSettlingReleaseMultiplier);
			}
			if (Pose.Target.Get() != Target || Pose.LastSampleTime < 0.0
				|| bSensitivityChanged)
			{
				// New member, resolved-component swap, or reattachment: establish a
				// baseline. Detector configuration is not gameplay movement either.
				Pose.Target = Target;
				Pose.Sensitivity = Sensitivity;
				Pose.bHasSensitivity = true;
				Pose.LastLocation = Transform.GetLocation();
				Pose.LastRotation = Transform.GetRotation().GetNormalized();
				Pose.LastSampleTime = NowSeconds;
				Pose.TrendSamples.Reset();
				Pose.TrendSamples.Add({Pose.LastLocation, Pose.LastRotation, NowSeconds});
				++RebaselinedSamples;
				continue;
			}

			const double DeltaSeconds = NowSeconds - Pose.LastSampleTime;
			if (DeltaSeconds <= SMALL_NUMBER)
			{
				Pose.LastLocation = Transform.GetLocation();
				Pose.LastRotation = Transform.GetRotation().GetNormalized();
				Pose.LastSampleTime = NowSeconds;
				Pose.TrendSamples.Reset();
				Pose.TrendSamples.Add({Pose.LastLocation, Pose.LastRotation, NowSeconds});
				++RebaselinedSamples;
				continue;
			}

			const FVector CurrentLocation = Transform.GetLocation();
			const FQuat CurrentRotation = Transform.GetRotation().GetNormalized();
			const FVector Velocity = (CurrentLocation - Pose.LastLocation) / DeltaSeconds;
			const float LinearSpeed = Velocity.Size();
			const double RotationDot = FMath::Clamp(
				FMath::Abs(static_cast<double>(Pose.LastRotation | CurrentRotation)), 0.0, 1.0);
			const float AngularSpeed = static_cast<float>(
				FMath::RadiansToDegrees(2.0 * FMath::Acos(RotationDot)) / DeltaSeconds);

			Pose.TrendSamples.Add({CurrentLocation, CurrentRotation, NowSeconds});
			const ConvaiSpatial::FMotionTrend Trend =
				ConvaiSpatial::ComputeRollingMotionTrend(Pose.TrendSamples);
			// Even before the full window matures, its net vector is more stable than
			// the last raw delta and agrees with the evidence used to confirm a start.
			const FVector FilteredVelocity = Trend.LinearVelocity;
			const float FilteredLinearSpeed = FilteredVelocity.Size();
			const float FilteredAngularSpeed = Trend.AngularSpeed;

			Pose.LastLocation = CurrentLocation;
			Pose.LastRotation = CurrentRotation;
			Pose.LastSampleTime = NowSeconds;
			++ComparableSamples;

			const bool bTrendQuiet = ConvaiSpatial::IsMotionTrendQuiet(
				Trend, Thresholds);
			bAngularTrendQuiet &= Trend.bReady
				&& FilteredAngularSpeed <= Thresholds.StopAngularSpeed
				&& AngularSpeed <= Thresholds.StopAngularSpeed
				&& Trend.MaxAngularExcursion
					<= Thresholds.AngularExcursionFloor;
			const bool bRawAboveStart =
				LinearSpeed >= Thresholds.StartLinearSpeed
					|| AngularSpeed >= Thresholds.StartAngularSpeed;
			bAboveStart |= ConvaiSpatial::IsMotionStartEvidence(
				bRawAboveStart, Trend, Thresholds);
			bBelowStop &= bTrendQuiet;
			// Raw fallback gives a newly starting object an immediate direction, but
			// cannot relabel an established moving group after a member swap.
			const bool bCanContributeSemantic = Trend.bReady || !Cache.bMoving;
			bAnyLinearExcursion |= bCanContributeSemantic && Trend.bReady
				&& Trend.MaxLinearExcursion > Thresholds.LinearExcursionFloor;
			bAnyAngularMotion |= bCanContributeSemantic
				&& (FilteredAngularSpeed > Thresholds.StopAngularSpeed
					|| (Trend.bReady && Trend.MaxAngularExcursion
						> Thresholds.AngularExcursionFloor));
			const bool bHasClearLinearDirectionEvidence = bCanContributeSemantic
				&& ConvaiSpatial::HasClearMotionDirectionEvidence(
					Velocity, AngularSpeed,
					FilteredVelocity, FilteredAngularSpeed,
					/*bRotationOnly*/ false, Thresholds);
			const bool bHasClearAngularDirectionEvidence = bCanContributeSemantic
				&& ConvaiSpatial::HasClearMotionDirectionEvidence(
					Velocity, AngularSpeed,
					FilteredVelocity, FilteredAngularSpeed,
					/*bRotationOnly*/ true, Thresholds);
			// Starting a prospective direction still requires the profile's Start
			// threshold. Once that same candidate exists, however, preserve coherent
			// evidence throughout the Start/Stop hysteresis band so sustained slow
			// travel cannot be mistaken for a bounded endpoint rebound.
			ConvaiSpatial::FMotionThresholds ContinuationThresholds = Thresholds;
			ContinuationThresholds.StartLinearSpeed =
				Thresholds.StopLinearSpeed;
			ContinuationThresholds.StartAngularSpeed =
				Thresholds.StopAngularSpeed;
			const bool bHasContinuationLinearDirectionEvidence =
				bCanContributeSemantic
				&& ConvaiSpatial::HasClearMotionDirectionEvidence(
					Velocity, AngularSpeed,
					FilteredVelocity, FilteredAngularSpeed,
					/*bRotationOnly*/ false, ContinuationThresholds);
			const bool bHasContinuationAngularDirectionEvidence =
				bCanContributeSemantic
				&& ConvaiSpatial::HasClearMotionDirectionEvidence(
					Velocity, AngularSpeed,
					FilteredVelocity, FilteredAngularSpeed,
					/*bRotationOnly*/ true, ContinuationThresholds);
			bAnyClearAngularDirectionEvidence |=
				bHasClearAngularDirectionEvidence;
			bAnyContinuationAngularDirectionEvidence |=
				bHasContinuationAngularDirectionEvidence;
			if (bHasContinuationAngularDirectionEvidence)
			{
				ContinuationAngularSpeed = FMath::Max(
					ContinuationAngularSpeed, FilteredAngularSpeed);
			}
			if (bHasClearAngularDirectionEvidence)
			{
				ClearAngularExcursionFloor = FMath::Max(
					ClearAngularExcursionFloor,
					Thresholds.AngularExcursionFloor);
				ClearAngularSpeed = FMath::Max(
					ClearAngularSpeed, FilteredAngularSpeed);
			}
			if (bCanContributeSemantic
				&& FilteredLinearSpeed > Thresholds.StopLinearSpeed)
			{
				VelocitySum += FilteredVelocity;
				LinearSpeedSum += FilteredLinearSpeed;
				++LinearVelocityCount;
				if (bHasClearLinearDirectionEvidence)
				{
					ClearVelocitySum += FilteredVelocity;
					ClearLinearSpeedSum += FilteredLinearSpeed;
					++ClearLinearVelocityCount;
					ClearLinearExcursionFloor = FMath::Max(
						ClearLinearExcursionFloor,
						Thresholds.LinearExcursionFloor);
				}
				if (bHasContinuationLinearDirectionEvidence)
				{
					ContinuationVelocitySum += FilteredVelocity;
					ContinuationLinearSpeedSum += FilteredLinearSpeed;
					++ContinuationLinearVelocityCount;
				}
			}
		}
		// A trend-backed stop is already fully confirmed, so it may publish in this
		// poll. Require evidence from every valid member first; a newly attached or
		// swapped member must not make a partly sampled group stop immediately.
		bBelowStop &= ComparableSamples == ValidSamples;
		bAngularTrendQuiet &= ComparableSamples == ValidSamples;
		if (Cache.bDirectionCandidateActive
			&& !bDetectorConfigurationChanged
			&& DirectionEpisodeSamples == ValidSamples
			&& !bDirectionEpisodeInsideEnvelope)
		{
			Cache.bDirectionCandidateEscaped = true;
		}
		if (Cache.bBoundedSettlingLatched)
		{
			if (bDetectorConfigurationChanged
				|| BoundedLatchSamples != ValidSamples)
			{
				// Structural changes silently rebaseline; they are not gameplay
				// movement and must not consume a movement watch.
				Cache.ClearBoundedSettlingLatch();
			}
			else if (bBoundedLatchInsideReleaseEnvelope)
			{
				bAboveStart = false;
			}
			else
			{
				// Physical escape is allowed to restart from the lower Stop
				// threshold, so slow travel cannot remain latched forever.
				Cache.bBoundedSettlingLatched = false;
				Cache.bBoundedSettlingEscapePending = true;
			}
		}
		if (Cache.bBoundedSettlingEscapePending)
		{
			if (bBelowStop)
			{
				Cache.bBoundedSettlingEscapePending = false;
			}
			else
			{
				bAboveStart = true;
			}
		}

		// Average only coherent group motion. Opposing members in a merged set
		// still make the coarse state Moving, but get no invented direction.
		FVector SemanticCandidate = FVector::ZeroVector;
		float AverageMemberLinearSpeed = 0.0f;
		if (LinearVelocityCount > 0)
		{
			const FVector AverageVelocity = VelocitySum / static_cast<float>(LinearVelocityCount);
			AverageMemberLinearSpeed =
				LinearSpeedSum / static_cast<float>(LinearVelocityCount);
			if (LinearVelocityCount == 1
				|| AverageVelocity.Size() >= AverageMemberLinearSpeed * 0.60f)
			{
				SemanticCandidate = AverageVelocity;
			}
		}

		const bool bHasLinearMotion =
			LinearVelocityCount > 0 || bAnyLinearExcursion;
		const bool bHasSemanticMotion =
			bHasLinearMotion || bAnyAngularMotion;
		const bool bCandidateRotationOnly =
			!bHasLinearMotion && bAnyAngularMotion;
		const bool bMeaningfulDirectionChange =
			bHasSemanticMotion
				&& ConvaiSpatial::HasMeaningfulMotionDirectionChange(
					Cache.SemanticVelocity, Cache.bRotationOnly,
					SemanticCandidate, bCandidateRotationOnly);
		const bool bHasClearDirectionEvidence = bCandidateRotationOnly
			? bAnyClearAngularDirectionEvidence
			: ConvaiSpatial::HasCoherentClearLinearDirectionEvidence(
				SemanticCandidate, ClearVelocitySum,
				ClearLinearVelocityCount, LinearVelocityCount);
		const bool bHasContinuationDirectionEvidence =
			bCandidateRotationOnly
			? bAnyContinuationAngularDirectionEvidence
			: ConvaiSpatial::HasCoherentClearLinearDirectionEvidence(
				SemanticCandidate, ContinuationVelocitySum,
				ContinuationLinearVelocityCount, LinearVelocityCount);
		const double DirectionDeltaSeconds =
			Cache.LastDirectionEvaluationTime >= 0.0
				&& NowSeconds > Cache.LastDirectionEvaluationTime
			? NowSeconds - Cache.LastDirectionEvaluationTime
			: 0.0;
		Cache.LastDirectionEvaluationTime = NowSeconds;

		bool bDirectionCandidateConfirmed = false;
		bool bCurrentCandidateCoherent = false;
		bool bBoundedSettlingThisPoll = false;
		if (Cache.bMoving && !bDetectorConfigurationChanged)
		{
			const bool bDirectionlessScalarContinuation =
				Cache.bDirectionCandidateActive
				&& !Cache.bDirectionCandidateRotationOnly
				&& !bCandidateRotationOnly
				&& Cache.DirectionCandidateVelocity.Size()
					<= ConvaiSpatial::MotionMinimumLinearSpeed
				&& LinearVelocityCount > 0
				&& ContinuationLinearVelocityCount
					== LinearVelocityCount;
			bool bMatchesDirectionCandidate =
				Cache.bDirectionCandidateActive
				&& Cache.bDirectionCandidateRotationOnly
					== bCandidateRotationOnly
				&& (bDirectionlessScalarContinuation
					|| !ConvaiSpatial::HasMeaningfulMotionDirectionChange(
						Cache.DirectionCandidateVelocity,
						Cache.bDirectionCandidateRotationOnly,
						SemanticCandidate, bCandidateRotationOnly));
			// Once an endpoint-settling episode exists, clear motion back in the
			// committed direction is also a candidate segment. It must prove real
			// progress before cancelling settling; a short return leg is just the
			// other half of the same bounded bounce. A matching active segment may
			// continue inside the Start/Stop hysteresis band, but sub-Start evidence
			// cannot create a candidate or switch it to a different direction.
			const bool bHasStartLevelCandidate = bHasSemanticMotion
				&& bHasClearDirectionEvidence
				&& (bMeaningfulDirectionChange
					|| Cache.bDirectionCandidateActive);
			const bool bHasRelaxedCandidateContinuation =
				bHasSemanticMotion
				&& bMatchesDirectionCandidate
				&& bHasContinuationDirectionEvidence;
			bCurrentCandidateCoherent = bHasStartLevelCandidate
				|| bHasRelaxedCandidateContinuation;
			if (bCurrentCandidateCoherent)
			{
				const bool bResumedAfterCoherenceLoss =
					Cache.bDirectionCandidateHadCoherenceLoss
						&& Cache.DirectionCandidateCoherentSince < 0.0;
				if (!Cache.bDirectionCandidateActive)
				{
					Cache.bDirectionCandidateActive = true;
					Cache.DirectionCandidateEpisodeSince = NowSeconds;
					Cache.DirectionEpisodeOriginalVelocity =
						Cache.SemanticVelocity;
					Cache.bDirectionEpisodeOriginalRotationOnly =
						Cache.bRotationOnly;
					Cache.CaptureDirectionEpisodeAnchors();
				}
				const bool bRebasedEscapedDirectionEpisode =
					ConvaiSpatial::ShouldRebaseEscapedMotionEpisode(
						Cache.bDirectionCandidateActive,
						Cache.bDirectionCandidateEscaped,
						bCurrentCandidateCoherent,
						bMatchesDirectionCandidate,
						bResumedAfterCoherenceLoss);
				if (bRebasedEscapedDirectionEpisode)
				{
					// The old candidate already proved meaningful travel. A newly
					// coherent direction, including one returning after an evidence
					// gap, is a local segment. Judge endpoint settling from here
					// instead of inheriting the prior trip's escape.
					Cache.DirectionCandidateEpisodeSince = NowSeconds;
					Cache.bDirectionCandidateHadCoherenceLoss = false;
					Cache.DirectionCandidateSwitchCount = 0;
					Cache.bDirectionCandidateEscaped = false;
					// This is a genuinely new local episode, even when the same
					// candidate direction resumed after a gap. Do not inherit the
					// trip's accumulated progress or stable-duration clock; either
					// would let endpoint jitter relabel immediately after rebase.
					Cache.DirectionCandidateVelocity = SemanticCandidate;
					Cache.bDirectionCandidateRotationOnly =
						bCandidateRotationOnly;
					Cache.DirectionCandidateCoherentSince = NowSeconds;
					Cache.DirectionCandidateProgress = 0.0f;
					if (!bMatchesDirectionCandidate)
					{
						// A different coherent segment has start-level evidence,
						// so use only its contributing members' sensitivity floor.
						// A matching relaxed resume keeps the candidate's existing
						// floor; stationary siblings must not make it stricter.
						Cache.DirectionCandidateRequiredProgress =
							ConvaiSpatial::MotionDirectionProgressMultiplier
								* (bCandidateRotationOnly
									? ClearAngularExcursionFloor
									: ClearLinearExcursionFloor);
					}
					Cache.DirectionEpisodeOriginalVelocity =
						Cache.SemanticVelocity;
					Cache.bDirectionEpisodeOriginalRotationOnly =
						Cache.bRotationOnly;
					Cache.CaptureDirectionEpisodeAnchors();
					bMatchesDirectionCandidate = true;
				}
				else if (bResumedAfterCoherenceLoss)
				{
					// The gap happened before this episode escaped. Consume it on
					// the first coherent resume so a later continuous escape cannot
					// be mistaken for a post-trip endpoint segment.
					Cache.bDirectionCandidateHadCoherenceLoss = false;
				}
				if (!bMatchesDirectionCandidate)
				{
					if (!bRebasedEscapedDirectionEpisode
						&& Cache.DirectionCandidateCoherentSince >= 0.0)
					{
						++Cache.DirectionCandidateSwitchCount;
					}
					Cache.DirectionCandidateVelocity = SemanticCandidate;
					Cache.bDirectionCandidateRotationOnly =
						bCandidateRotationOnly;
					Cache.DirectionCandidateCoherentSince = NowSeconds;
					Cache.DirectionCandidateProgress = 0.0f;
					Cache.DirectionCandidateRequiredProgress =
						ConvaiSpatial::MotionDirectionProgressMultiplier
						* (bCandidateRotationOnly
							? ClearAngularExcursionFloor
							: ClearLinearExcursionFloor);
				}
				else if (Cache.DirectionCandidateCoherentSince < 0.0)
				{
					// A later matching burst starts a new continuous-evidence
					// stretch but remains in the same bounded-settling episode.
					Cache.DirectionCandidateCoherentSince = NowSeconds;
					Cache.DirectionCandidateProgress = 0.0f;
				}

				const bool bUseStartLevelProgress =
					bHasStartLevelCandidate;
				const float ProgressSpeed =
					ConvaiSpatial::MotionDirectionCandidateProgressSpeed(
						Cache.DirectionCandidateVelocity,
						bCandidateRotationOnly,
						bUseStartLevelProgress
							? ClearVelocitySum
							: ContinuationVelocitySum,
						bUseStartLevelProgress
							? ClearLinearSpeedSum
							: ContinuationLinearSpeedSum,
						bUseStartLevelProgress
							? ClearLinearVelocityCount
							: ContinuationLinearVelocityCount,
						bUseStartLevelProgress
							? ClearAngularSpeed
							: ContinuationAngularSpeed);
				Cache.DirectionCandidateProgress += ProgressSpeed
					* static_cast<float>(DirectionDeltaSeconds);
				bDirectionCandidateConfirmed =
					ConvaiSpatial::IsMotionDirectionCandidateConfirmed(
						/*currently coherent*/ true,
						NowSeconds
							- Cache.DirectionCandidateCoherentSince,
						Cache.DirectionCandidateProgress,
						Cache.DirectionCandidateRequiredProgress);
			}
			else if (Cache.bDirectionCandidateActive
				&& Cache.DirectionCandidateCoherentSince >= 0.0)
			{
				Cache.bDirectionCandidateHadCoherenceLoss = true;
				Cache.DirectionCandidateCoherentSince = -1.0;
				Cache.DirectionCandidateProgress = 0.0f;
			}

			if (Cache.bDirectionCandidateActive)
			{
				const bool bCurrentDirectionStable =
					bCurrentCandidateCoherent
					&& Cache.DirectionCandidateCoherentSince >= 0.0
					&& NowSeconds
						- Cache.DirectionCandidateCoherentSince
							>= ConvaiSpatial::MotionStartConfirmationSeconds;
				const bool bHasSettlingEvidence =
					ConvaiSpatial::HasMotionSettlingEvidence(
						Cache.bDirectionCandidateHadCoherenceLoss,
						Cache.DirectionCandidateSwitchCount,
						bCurrentCandidateCoherent,
						bCurrentDirectionStable);
				bBoundedSettlingThisPoll =
					ConvaiSpatial::CanUseBoundedMotionSettling(
						bHasSettlingEvidence,
						Cache.bDirectionCandidateEscaped,
						bDirectionCandidateConfirmed,
						bCurrentDirectionStable,
						NowSeconds - Cache.DirectionCandidateEpisodeSince,
						bAngularTrendQuiet);
				bBelowStop |= bBoundedSettlingThisPoll;
			}
		}

		if (bBoundedSettlingThisPoll)
		{
			// A bounded physics episode may briefly acquire lateral labels. The
			// stop edge describes the completed trip that preceded the episode.
			Cache.SemanticVelocity =
				Cache.DirectionEpisodeOriginalVelocity;
			Cache.bRotationOnly =
				Cache.bDirectionEpisodeOriginalRotationOnly;
		}

		bool bReinitialized = false;
		ConvaiSpatial::EMotionTransition Transition = ConvaiSpatial::EMotionTransition::None;
		if (bDetectorConfigurationChanged
			|| (ValidSamples > 0 && ComparableSamples == 0
				&& RebaselinedSamples == ValidSamples))
		{
			// Target, membership, and sensitivity changes are detector configuration.
			// Preserve the last confirmed coarse state and silently establish a fresh
			// baseline; a synthetic edge here could consume an armed watch.
			bReinitialized = true;
			Cache.CandidateSince = -1.0;
			Cache.InitializedAt = NowSeconds;
		}
		else if (ComparableSamples > 0)
		{
			Transition = ConvaiSpatial::UpdateMotionState(
				bAboveStart, bBelowStop, NowSeconds, Cache.bMoving, Cache.CandidateSince,
				ConvaiSpatial::MotionStartConfirmationSeconds,
				/*StopConfirmationSeconds*/ 0.0);
		}

		Cache.TransitionThisPoll = static_cast<uint8>(Transition);
		Cache.bReinitializedThisPoll = bReinitialized;
		if (bReinitialized)
		{
			Cache.bAwaitingPostReinitMotion = Cache.bMoving;
			Cache.ResetDirectionCandidate();
			Cache.ClearBoundedSettlingLatch();
		}
		else if (Transition == ConvaiSpatial::EMotionTransition::Started)
		{
			Cache.ClearBoundedSettlingLatch();
			Cache.ResetDirectionCandidate();
		}
		else if (Transition == ConvaiSpatial::EMotionTransition::Stopped)
		{
			if (bBoundedSettlingThisPoll)
			{
				Cache.CaptureBoundedSettlingLatch();
			}
			else
			{
				Cache.ClearBoundedSettlingLatch();
			}
			Cache.ResetDirectionCandidate();
		}

		if (bReinitialized)
		{
			Cache.SemanticVelocity = FVector::ZeroVector;
			Cache.bRotationOnly = false;
			Cache.LastSemanticUpdate = -1.0;
		}
		else if (Cache.bMoving)
		{
			const bool bDirectionChanged =
				bDirectionCandidateConfirmed
				&& Cache.bDirectionCandidateEscaped
				&& bMeaningfulDirectionChange;
			const bool bUncommittedDirectionEpisode =
				bMeaningfulDirectionChange
				&& !bDirectionChanged;
			const bool bRefreshSemantic =
				Transition == ConvaiSpatial::EMotionTransition::Started
				|| Cache.LastSemanticUpdate < 0.0
				|| bDirectionChanged
				|| (!bUncommittedDirectionEpisode
					&& NowSeconds - Cache.LastSemanticUpdate >= 1.0);
			// During endpoint settling, retain the last useful direction. A rebound
			// may briefly clear the start threshold, but it cannot relabel the trip
			// until coherent progress exits the sensitivity-scaled envelope.
			// Active but clearly opposing merged members are different: clear the
			// old vector so the fact truthfully falls back to generic "moving".
			if (bRefreshSemantic && bHasSemanticMotion)
			{
				const bool bSpeedOnlyRefresh = !bDirectionChanged
					&& Transition != ConvaiSpatial::EMotionTransition::Started
					&& !Cache.bRotationOnly && !bCandidateRotationOnly
					&& Cache.SemanticVelocity.Size() > ConvaiSpatial::MotionMinimumLinearSpeed
					&& SemanticCandidate.Size() > ConvaiSpatial::MotionMinimumLinearSpeed;
				if (bSpeedOnlyRefresh)
				{
					// Refresh magnitude for sparse slow/quick wording without moving the
					// direction anchor. Small turns then accumulate until they become a
					// meaningful state change instead of being forgotten every second.
					Cache.SemanticVelocity = Cache.SemanticVelocity.GetSafeNormal()
						* SemanticCandidate.Size();
				}
				else
				{
					Cache.SemanticVelocity = SemanticCandidate;
					Cache.bRotationOnly = bCandidateRotationOnly;
				}
				Cache.bSemanticChangedThisPoll = bDirectionChanged;
				Cache.LastSemanticUpdate = NowSeconds;
			}
			if (bDirectionCandidateConfirmed
				&& Cache.bDirectionCandidateEscaped)
			{
				Cache.ResetDirectionCandidate();
			}
		}

	}
}

void UConvaiContextSubsystem::PublishObjectMovementStates()
{
	UConvaiSubsystem* Subsystem = GetConvaiSubsystem();
	if (!Subsystem)
	{
		return;
	}

	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	const double NowSeconds = World ? static_cast<double>(World->GetTimeSeconds())
		: FPlatformTime::Seconds();
	TArray<UConvaiSubsystem::FConvaiObjectGroup> Groups;
	Subsystem->BuildObjectGroups(Groups);
	TSet<FString> CurrentKeys;

	auto RemoveStateFromAll = [Subsystem](const FString& StateKey)
	{
		if (StateKey.IsEmpty())
		{
			return;
		}
		for (UConvaiChatbotComponent* Chatbot : Subsystem->GetAllChatbotComponents())
		{
			if (IsValid(Chatbot))
			{
				Chatbot->RemoveContextState(StateKey);
			}
		}
	};

	for (const UConvaiSubsystem::FConvaiObjectGroup& Group : Groups)
	{
		const FString CacheKey = FString::Printf(TEXT("Object:%s"), *Group.Name.ToLower());
		FObjectMotionCache* Cache = ObjectMotionCaches.Find(CacheKey);
		if (!Cache)
		{
			continue;
		}
		const FResolvedMovementSettings StateSettings = ResolveMovementSettings(Group);
		const bool bHasStateSettings =
			StateSettings.bStateEnabled && !StateSettings.bStateKeyCollision;

		const FString DesiredStateKey = ConvaiContextFormat::SanitizeKey(
			FString::Printf(TEXT("%s.Movement"), *Group.Name));
		if (Cache->bStateExposed && (!bHasStateSettings || Cache->StateKey != DesiredStateKey))
		{
			// If an authored property has claimed the same key, ownership passes
			// to that producer; deleting it here would erase the authored state.
			if (!StateSettings.bStateKeyCollision)
			{
				RemoveStateFromAll(Cache->StateKey);
			}
			Cache->bStateExposed = false;
			Cache->StateKey.Reset();
		}
		if (!StateSettings.bAwarenessEnabled)
		{
			// The cleanup pass below removes this now-disabled detector cache.
			continue;
		}
		CurrentKeys.Add(CacheKey);

		if (!bHasStateSettings)
		{
			if (!Cache->bMoving || (Cache->bAwaitingPostReinitMotion
				&& Cache->LastSemanticUpdate >= 0.0))
			{
				Cache->bAwaitingPostReinitMotion = false;
			}
			continue;
		}

		const ConvaiSpatial::EMotionTransition Transition =
			static_cast<ConvaiSpatial::EMotionTransition>(Cache->TransitionThisPoll);
		const bool bPastStartupGrace = NowSeconds - Cache->InitializedAt >= 1.0;
		const bool bConfirmedTransition = ConvaiSpatial::IsGameplayMotionTransition(
			Transition, Cache->bReinitializedThisPoll,
			Cache->bAwaitingPostReinitMotion);

		for (UConvaiChatbotComponent* Chatbot : Subsystem->GetAllChatbotComponents())
		{
			AActor* ObserverOwner = IsValid(Chatbot) ? Chatbot->GetOwner() : nullptr;
			if (!IsValid(ObserverOwner))
			{
				continue;
			}
			const FTransform ObserverTransform = ObserverOwner->GetActorTransform();
			const FString DesiredValue = ConvaiSpatial::MotionStateValue(
				Cache->bMoving, Cache->SemanticVelocity, Cache->bRotationOnly,
				ObserverTransform, Group.Centroid);
			// A disconnected Reset Dynamic Context is delivered last on reconnect.
			// Do not seed a movement state ahead of that reset and then lose it.
			if (Chatbot->PendingContextBatch.bPendingReset)
			{
				continue;
			}
			if (Cache->bReinitializedThisPoll)
			{
				// A component swap/reattachment is a new observation baseline, not a
				// continuation of a queued gameplay edge. Restore the pre-batch tracker
				// value before dropping staged metadata, then seed the current baseline
				// silently so no stale response rank or watch promotion survives.
				if (Chatbot->PendingContextBatch.StagedStateOrder.Contains(DesiredStateKey))
				{
					if (const FString* Initial =
						Chatbot->PendingContextBatch.InitialValues.Find(DesiredStateKey))
					{
						Chatbot->DynamicContextTracker.SetState(DesiredStateKey, *Initial);
					}
					else
					{
						Chatbot->DynamicContextTracker.RemoveState(DesiredStateKey);
					}
				}
				Chatbot->HeldContext.DropStateKey(DesiredStateKey);
				Chatbot->PendingContextBatch.DropStateKey(DesiredStateKey);
				Chatbot->SetContextStateInternal(DesiredStateKey, DesiredValue,
					EC_RunLLMOption::Never, EConvaiContextDelivery::SendNormally,
					/*bFlushImmediately*/ false, /*bOmitPreviousValue*/ true,
					/*bEvaluateWatches*/ false);
				continue;
			}
			FString ExistingValue;
			const bool bHasState = Chatbot->GetContextStateValue(DesiredStateKey, ExistingValue);
			const FObserverSpatialCache* SpatialCache = SpatialCaches.Find(Chatbot);
			const bool bInitialBaselinePending = !SpatialCache
				|| !SpatialCache->bInitialBaselineDelivered;
			if (!bHasState)
			{
				// A seed is never a transition and must not consume a one-shot watch.
				Chatbot->SetContextStateInternal(DesiredStateKey, DesiredValue,
					EC_RunLLMOption::Never, EConvaiContextDelivery::SendNormally,
					/*bFlushImmediately*/ false, /*bOmitPreviousValue*/ true,
					/*bEvaluateWatches*/ false);
				continue;
			}
			const FString* HeldValue =
				Chatbot->HeldContext.StateValues.Find(DesiredStateKey);
			const FString& EffectiveValue = HeldValue ? *HeldValue : ExistingValue;
			const bool bValueChanged = EffectiveValue != DesiredValue;
			// Keep observer-relative labels truthful when the character turns or
			// moves, but do not let that perspective-only relabel consume a watch.
			// Watches represent physical object motion: start/stop or a meaningful
			// change in the sampled velocity/direction.
			const bool bPhysicalDirectionValueChanged = Cache->bMoving
				&& Cache->bSemanticChangedThisPoll && bValueChanged;
			if (!bConfirmedTransition && !bValueChanged)
			{
				continue;
			}
			FString PreviousValue = TEXT("Stopped");
			if (Transition == ConvaiSpatial::EMotionTransition::Stopped)
			{
				// Match the state this observer actually received (including an idle-held
				// value). Recomputing from its current pose could contradict that value
				// if the observer turned just before the object stopped.
				PreviousValue = EffectiveValue != TEXT("Stopped")
					? EffectiveValue
					: ConvaiSpatial::MotionStateValue(
						/*bIsMoving*/ true, Cache->SemanticVelocity,
						Cache->bRotationOnly, ObserverTransform, Group.Centroid);
			}

			// Start/stop policies apply only to confirmed detector edges. A direction
			// shift silently keeps the durable state truthful unless a one-shot
			// property watch explicitly promotes that value change.
			EC_RunLLMOption Response = EC_RunLLMOption::Never;
			if (bConfirmedTransition && bPastStartupGrace && !bInitialBaselinePending)
			{
				Response = Transition == ConvaiSpatial::EMotionTransition::Started
					? StateSettings.StartedResponse
					: StateSettings.StoppedResponse;
			}
			const bool bEvaluateWatches = !bInitialBaselinePending
				&& !Cache->bReinitializedThisPoll
				&& (bConfirmedTransition || (bPhysicalDirectionValueChanged
					&& !Cache->bAwaitingPostReinitMotion));
			bool bWatchedTransition = bEvaluateWatches
				&& Chatbot->WouldWatchContextStateChange(
					DesiredStateKey, DesiredValue,
					bValueChanged ? &EffectiveValue : nullptr);
			FString PreviousOverrideStorage;
			const FString* PreviousOverride =
				bConfirmedTransition ? &PreviousValue : nullptr;
			if (!bConfirmedTransition && Cache->bMoving && bValueChanged)
			{
				if (bWatchedTransition)
				{
					// A promoted direction watch describes the physical value it
					// actually followed, including a value still held until idle.
					PreviousOverrideStorage = EffectiveValue;
					PreviousOverride = &PreviousOverrideStorage;
				}
				else
				{
					// Generic batching deliberately advances preserved history on every
					// later write (needed by Watch Property round trips). Movement is the
					// narrow exception: a silent direction/perspective relabel must not
					// erase a queued confirmed start's `was Stopped` origin.
					const FString* PreservedOrigin = nullptr;
					if (Chatbot->HeldContext.StatePreserveTransitions.Contains(DesiredStateKey))
					{
						PreservedOrigin = Chatbot->HeldContext.StatePreviousValueOverrides.Find(
							DesiredStateKey);
					}
					if (!PreservedOrigin
						&& Chatbot->PendingContextBatch.PreserveTransitionKeys.Contains(
							DesiredStateKey))
					{
						PreservedOrigin = Chatbot->PendingContextBatch.OldValues.Find(
							DesiredStateKey);
					}
					if (PreservedOrigin && *PreservedOrigin == TEXT("Stopped"))
					{
						PreviousOverrideStorage = *PreservedOrigin;
						PreviousOverride = &PreviousOverrideStorage;
					}
				}
			}
			bool bInheritedWatchPromotion = false;
			if (bConfirmedTransition)
			{
				// A confirmed detector edge is authoritative. Do not let an older
				// same-key movement value flush first or lend its stronger response
				// rank to this edge. Direction-only relabels intentionally skip this
				// withdrawal and may continue coalescing with the active edge.
				Chatbot->HeldContext.DropStateKey(
					DesiredStateKey, &bInheritedWatchPromotion);
				bool bPendingWatchPromotion = false;
				Chatbot->PendingContextBatch.WithdrawStateForHold(
					Chatbot->DynamicContextTracker, DesiredStateKey,
					&bPendingWatchPromotion);
				bInheritedWatchPromotion |= bPendingWatchPromotion;
				if (bInheritedWatchPromotion)
				{
					// The watch was consumed by the older queued edge. Its explicit
					// Always promise follows the fresher authoritative edge.
					Response = EC_RunLLMOption::Always;
					bWatchedTransition = true;
				}
			}
			Chatbot->SetContextStateInternal(DesiredStateKey, DesiredValue, Response,
				StateSettings.Delivery,
				StateSettings.bFlushImmediately
					&& (Response != EC_RunLLMOption::Never || bWatchedTransition),
				/*bOmitPreviousValue*/ false,
				bEvaluateWatches,
				PreviousOverride,
				/*bPreserveTransition*/ bConfirmedTransition);
			if (bConfirmedTransition && bInheritedWatchPromotion)
			{
				// A fresh watch is tagged by SetContextStateInternal. This consumed
				// inherited watch needs the same explicit ownership on whichever
				// lane accepted the replacement, so a third edge cannot lose it.
				if (Chatbot->PendingContextBatch.StagedStateOrder.Contains(
					DesiredStateKey))
				{
					Chatbot->PendingContextBatch.WatchPromotedStateKeys.Add(
						DesiredStateKey);
				}
				if (Chatbot->HeldContext.StateValues.Contains(DesiredStateKey))
				{
					Chatbot->HeldContext.WatchPromotedStateKeys.Add(DesiredStateKey);
				}
			}
		}
		if (!Cache->bMoving || (Cache->bAwaitingPostReinitMotion
			&& Cache->LastSemanticUpdate >= 0.0))
		{
			Cache->bAwaitingPostReinitMotion = false;
		}
		Cache->bStateExposed = true;
		Cache->StateKey = DesiredStateKey;
	}

	for (auto It = ObjectMotionCaches.CreateIterator(); It; ++It)
	{
		if (!CurrentKeys.Contains(It.Key()))
		{
			if (It.Value().bStateExposed)
			{
				RemoveStateFromAll(It.Value().StateKey);
			}
			It.RemoveCurrent();
		}
	}
}

void UConvaiContextSubsystem::ResetObserverSpatialContext(
	UConvaiChatbotComponent* Observer)
{
	if (!IsValid(Observer))
	{
		return;
	}
	FObserverSpatialCache& Cache = SpatialCaches.FindOrAdd(Observer);
	for (const TPair<FString, FObserverSpatialCache::FFact>& Pair : Cache.Facts)
	{
		Observer->RemoveContextFact(Pair.Key);
	}
	Cache = FObserverSpatialCache();
	AActor* ObserverOwner = Observer->GetOwner();
	if (!IsValid(ObserverOwner))
	{
		return;
	}
	const FTransform ObserverTransform = ObserverOwner->GetActorTransform();

	// Synthetic Movement is spatial-derived state. An edge may have staged a
	// responsive update while an explicit session was stopped; a fresh session
	// must see the current value as silent baseline knowledge, not inherit that
	// old wake-up rank (StageState normally only upgrades within a batch).
	for (const TPair<FString, FObjectMotionCache>& Pair : ObjectMotionCaches)
	{
		const FObjectMotionCache& Motion = Pair.Value;
		if (!Motion.bStateExposed || Motion.StateKey.IsEmpty())
		{
			continue;
		}
		Observer->HeldContext.DropStateKey(Motion.StateKey);
		Observer->PendingContextBatch.DropStateKey(Motion.StateKey);
		Observer->SetContextStateInternal(Motion.StateKey,
			ConvaiSpatial::MotionStateValue(Motion.bMoving,
				Motion.SemanticVelocity, Motion.bRotationOnly,
				ObserverTransform, Motion.LastCentroid),
			EC_RunLLMOption::Never, EConvaiContextDelivery::SendNormally,
			/*bFlushImmediately*/ false, /*bOmitPreviousValue*/ true,
			/*bEvaluateWatches*/ false);
	}
}

namespace
{
	// A single thing a chatbot can perceive — an object, another character, or
	// the player. Built fresh each pass; not persisted.
	struct FSpatialEntity
	{
		FString  Name;
		FString  CacheKey;
		FVector  Location  = FVector::ZeroVector;
		FVector  Forward   = FVector::ForwardVector; // meaningful only when bHasFacing
		AActor*  Actor     = nullptr;                // for line-of-sight + frame
		// World-space AABB for support-relation classification (zero extent =
		// unknown, falls back to pivot heuristics). For merged sets this is the
		// union of member bounds so it stays consistent with the centroid Location.
		FVector  BoundsOrigin = FVector::ZeroVector;
		FVector  BoundsExtent = FVector::ZeroVector;
		uint8    Priority  = 0;                       // player=2 > character=1 > object=0
		bool     bHasFacing = false;                  // characters/players have a forward vector
		// Player view (camera) frame, yaw only — drives the player-perspective
		// clause. Body forward stays in Forward for facing phrases: players AIM
		// with the camera, so "ahead" must follow the view, not pawn yaw.
		FQuat    ViewRotation = FQuat::Identity;
		bool     bHasViewRotation = false;
		bool     bIsObject  = false;
		bool     bIsMoving  = false;
		bool     bReportsMovementState = false;       // valid explicit directional Movement opt-in
		bool     bRotationOnly = false;
		FVector  MotionVelocity = FVector::ZeroVector;
		bool     bHasMotionTransition = false;
		bool     bMotionReinitialized = false;
		bool     bMotionBaselineCorrection = false;
		FString  MotionStateKey;
		EC_RunLLMOption MotionTransitionResponse = EC_RunLLMOption::Never;
		EConvaiContextDelivery MotionDelivery = EConvaiContextDelivery::SendNormally;
		UConvaiChatbotComponent* AsChatbot = nullptr; // set when this subject IS a chatbot (skip self)
		// Set when this subject IS an object: every component making up this logical
		// object (one for a normal object, several for a merged same-named set). Used
		// for nav reachability — the group is reachable if ANY member can be pathed to.
		TArray<UConvaiObjectComponent*> ObjectMembers;
		// Scalar proximity uses the nearest concrete member (or named destination),
		// while Location remains the aggregate centroid for direction, relations,
		// line of sight, and change gating.
		TArray<FVector> ProximityLocations;

		// Named movement points: a virtual sub-object ("Door Other Side") sets
		// PointNameFilter (normalized) — its nav/position resolve ONLY the members'
		// points with that name. The base subject of an expanded set instead
		// resolves only its unnamed points. LosLocation is where the visibility
		// gate traces to: sub-objects anchor it to the BASE object — a far-side
		// point is usually occluded by the very thing it belongs to, but its
		// existence is authored knowledge, known whenever the object is seen.
		FString  PointNameFilter;
		bool     bUnnamedPointsOnly = false;
		FVector  LosLocation = FVector::ZeroVector; // ZeroVector = use Location
	};

	// World AABB from pawn-BLOCKING primitives only — the geometry characters
	// actually stand on or bump into. GetActorBounds(bOnlyCollidingComponents)
	// still counts overlap-only volumes, so a pressure plate's fat trigger box
	// (or an audio/effect sphere) would otherwise define the "faces" the
	// support relations classify against. Falls back to the colliding bounds
	// when nothing blocks (pure trigger objects, overlap-only pawns).
	void GetPawnBlockingBounds(const AActor* Actor, FVector& OutOrigin, FVector& OutExtent)
	{
		FBox Box(ForceInit);
		Actor->ForEachComponent<UPrimitiveComponent>(/*bIncludeFromChildActors*/ false,
			[&Box](const UPrimitiveComponent* Prim)
			{
				if (Prim->IsRegistered() && Prim->IsCollisionEnabled()
					&& Prim->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block)
				{
					Box += Prim->Bounds.GetBox();
				}
			});
		if (Box.IsValid)
		{
			OutOrigin = Box.GetCenter();
			OutExtent = Box.GetExtent();
		}
		else
		{
			Actor->GetActorBounds(/*bOnlyCollidingComponents*/ true, OutOrigin, OutExtent);
		}
	}

	// Objects read as "the <name>"; characters/players use their name verbatim.
	// Virtual movement-point sub-objects also stay verbatim ("Door Other Side
	// is behind you") — the model must be able to say the exact target back.
	FString MakeDisplayName(const FSpatialEntity& E)
	{
		return (E.bIsObject && E.PointNameFilter.IsEmpty())
			? FString::Printf(TEXT("the %s"), *E.Name) : E.Name;
	}

	int32 RunLLMRank(EC_RunLLMOption Option)
	{
		switch (Option)
		{
			case EC_RunLLMOption::Always: return 2;
			case EC_RunLLMOption::Auto:   return 1;
			default:                      return 0; // Never
		}
	}
	EC_RunLLMOption MaxResp(EC_RunLLMOption A, EC_RunLLMOption B)
	{
		return RunLLMRank(A) >= RunLLMRank(B) ? A : B;
	}

	FString JoinClauses(const TArray<FString, TInlineAllocator<2>>& Parts)
	{
		if (Parts.Num() == 0) { return FString(); }
		if (Parts.Num() == 1) { return Parts[0]; }
		// Separate egocentric/nav wording from a third-party relation. Without
		// the comma, "reachable by walking and behind Player" misgroups easily.
		return FString::Printf(TEXT("%s, and %s"), *Parts[0], *Parts[1]);
	}

	// Point-based stacking/adjacency test: returns "on top of" / "underneath" /
	// "next to", or empty when the two aren't tightly coupled. Coarse (uses the
	// pivots, not full bounds) — the fallback when an entity has no bounds.
	FString SupportRelation(const FVector& S, const FVector& T)
	{
		const float HorizDist = FVector(S.X - T.X, S.Y - T.Y, 0.0f).Size();
		const float Vert = S.Z - T.Z;
		constexpr float HorizThresholdUU = 100.0f;
		constexpr float VertThresholdUU  = 20.0f;
		if (HorizDist <= HorizThresholdUU)
		{
			if (Vert >  VertThresholdUU) { return TEXT("on top of"); }
			if (Vert < -VertThresholdUU) { return TEXT("underneath"); }
			return TEXT("next to");
		}
		return FString();
	}

	// Bounds-aware stacking/adjacency: classifies from the actual box faces, so
	// a crate resting anywhere on a wide pressure plate reads "on top of" even
	// when the pivots are far apart. Falls back to the pivot test when either
	// box is unknown.
	FString SupportRelation(const FSpatialEntity& S, const FSpatialEntity& T)
	{
		if (S.BoundsExtent.IsNearlyZero() || T.BoundsExtent.IsNearlyZero())
		{
			return SupportRelation(S.Location, T.Location);
		}

		constexpr float HorizSlackUU = 25.0f; // footprint coupling slack
		constexpr float VertTolUU    = 50.0f; // resting-face tolerance — wide enough
		                                      // for capsule/mesh offsets and sunk plates
		constexpr float GapMaxUU     = 50.0f; // max side gap for "next to"

		const float DX = FMath::Abs(S.BoundsOrigin.X - T.BoundsOrigin.X);
		const float DY = FMath::Abs(S.BoundsOrigin.Y - T.BoundsOrigin.Y);
		const bool bFootprintsCouple =
			DX <= S.BoundsExtent.X + T.BoundsExtent.X + HorizSlackUU &&
			DY <= S.BoundsExtent.Y + T.BoundsExtent.Y + HorizSlackUU;

		const float SBottom = S.BoundsOrigin.Z - S.BoundsExtent.Z;
		const float STop    = S.BoundsOrigin.Z + S.BoundsExtent.Z;
		const float TBottom = T.BoundsOrigin.Z - T.BoundsExtent.Z;
		const float TTop    = T.BoundsOrigin.Z + T.BoundsExtent.Z;

		if (bFootprintsCouple)
		{
			if (FMath::Abs(SBottom - TTop) <= VertTolUU) { return TEXT("on top of"); }
			if (FMath::Abs(STop - TBottom) <= VertTolUU) { return TEXT("underneath"); }
		}

		// "next to": genuinely side-by-side — vertical spans overlap and the
		// boxes touch or nearly touch in XY without being deeply nested.
		// Conservative on purpose: two large objects across the room must not
		// read adjacent just because their world-AABBs are big.
		const float GapX = DX - (S.BoundsExtent.X + T.BoundsExtent.X);
		const float GapY = DY - (S.BoundsExtent.Y + T.BoundsExtent.Y);
		const bool bVerticalOverlap = (SBottom <= TTop) && (TBottom <= STop);
		if (bVerticalOverlap
			&& GapX <= GapMaxUU && GapY <= GapMaxUU
			&& FMath::Max(GapX, GapY) > -GapMaxUU)
		{
			return TEXT("next to");
		}
		return FString();
	}

	// How subject S reads relative to its anchor T. Support relations win; else a
	// direction, taken from the anchor's own frame when it has facing
	// (character/player), or from the observing chatbot's frame for object-object.
	FString ComposeRelationClause(const FSpatialEntity& S, const FSpatialEntity& Anchor, const FTransform& ObserverXform)
	{
		const FString Support = SupportRelation(S, Anchor);
		if (!Support.IsEmpty())
		{
			return FString::Printf(TEXT("%s %s"), *Support, *MakeDisplayName(Anchor));
		}

		const FTransform Frame = (Anchor.bHasFacing && Anchor.Actor)
			? Anchor.Actor->GetActorTransform()
			: ObserverXform;
		const FVector LocalDelta = Frame.InverseTransformVectorNoScale(S.Location - Anchor.Location);

		FVector EyeUnused; float HeadAbove, FootBelow;
		ConvaiSpatial::ComputeEyeLine(Anchor.bHasFacing ? Anchor.Actor : nullptr, EyeUnused, HeadAbove, FootBelow);

		const ConvaiSpatial::FDirection Dir = ConvaiSpatial::ComputeDirection(LocalDelta, HeadAbove, FootBelow);
		if (Dir.IsEmpty() && !Dir.bColocated)
		{
			return FString();
		}
		return ConvaiSpatial::DirectionToRef(Dir, MakeDisplayName(Anchor));
	}

	// The anchor for subject S: the closest OTHER entity within ClusterDist that
	// outranks S — higher priority, or same priority with an earlier name. The
	// key tiebreak makes each pair described exactly once (dedup) and stable
	// across passes. Returns null when nothing nearby outranks S.
	const FSpatialEntity* FindAnchor(const FSpatialEntity& S, int32 SelfIndex,
		const TArray<FSpatialEntity>& All, float ClusterDist, bool bSkipPlayers)
	{
		const FSpatialEntity* Best = nullptr;
		const float ClusterDistSq = ClusterDist * ClusterDist;
		float BestDistSq = TNumericLimits<float>::Max();
		for (int32 i = 0; i < All.Num(); ++i)
		{
			if (i == SelfIndex) { continue; }
			const FSpatialEntity& T = All[i];
			// With the dedicated player-perspective clause on, the player must not
			// also serve as a directional anchor — the same frame would be stated
			// twice in different diction ("in front of Eshmawy" + "From Eshmawy's
			// position, ahead").
			if (bSkipPlayers && T.Priority == 2) { continue; }
			const bool bOutranks = (T.Priority > S.Priority)
				|| (T.Priority == S.Priority && T.CacheKey < S.CacheKey);
			if (!bOutranks) { continue; }
			const float DistSq = FVector::DistSquared(S.Location, T.Location);
			if (DistSq <= ClusterDistSq && DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Best = &T;
			}
		}
		return Best;
	}
} // namespace

void UConvaiContextSubsystem::GetDebugSpatialFacts(const UConvaiChatbotComponent* Observer,
	TArray<FConvaiDebugSpatialFact>& Out) const
{
	Out.Reset();
	if (!IsValid(Observer))
	{
		return;
	}
	const FObserverSpatialCache* Cache = SpatialCaches.Find(
		TWeakObjectPtr<UConvaiChatbotComponent>(const_cast<UConvaiChatbotComponent*>(Observer)));
	if (!Cache)
	{
		return;
	}
	for (const auto& Pair : Cache->Facts)
	{
		FConvaiDebugSpatialFact F;
		F.Key = Pair.Key;
		F.Name = Pair.Value.Name;
		F.Sentence = Pair.Value.Sentence;
		const FObserverSpatialCache::FNavCacheEntry* Nav = Cache->NavCache.Find(Pair.Key);
		F.bReached = Nav && Nav->bValid && Nav->bAlreadyThere;
		F.bReachable = !Nav || !Nav->bValid
			|| ConvaiSpatial::HasUsableDestination(Nav->bReachable, F.bReached);
		F.bPendingFlush = Observer->IsContextKeyPendingFlush(Pair.Key);
		Out.Add(MoveTemp(F));
	}
}

bool UConvaiContextSubsystem::HasPlayerPerspectiveFacts(const UConvaiChatbotComponent* Observer) const
{
	if (!IsValid(Observer))
	{
		return false;
	}
	const FObserverSpatialCache* Cache = SpatialCaches.Find(
		TWeakObjectPtr<UConvaiChatbotComponent>(const_cast<UConvaiChatbotComponent*>(Observer)));
	return Cache && Cache->Facts.Num() > 0 && Cache->bHasPlayerPerspectiveFacts;
}

void UConvaiContextSubsystem::EvaluateSpatialAwareness()
{
	UConvaiSubsystem* Subsystem = GetConvaiSubsystem();
	if (!Subsystem)
	{
		return;
	}

	// A startup baseline is complete only after the observer is connected and
	// every spatial fact / synthetic Movement state queued by an earlier poll has
	// left its delivery lane. This gate deliberately exists even when there are
	// no visible facts: otherwise a moving object could be the first responsive
	// update staged while the connection is still coming up.
	auto AdvanceInitialBaseline = [this](UConvaiChatbotComponent* Observer,
		FObserverSpatialCache& Cache)
	{
		if (!Cache.bInitialBaselineQueued || Cache.bInitialBaselineDelivered
			|| !Observer->IsChatbotConnected())
		{
			return;
		}

		for (const TPair<FString, FObserverSpatialCache::FFact>& Pair : Cache.Facts)
		{
			if (Observer->IsContextKeyPendingFlush(Pair.Key))
			{
				return;
			}
		}
		for (const TPair<FString, FObjectMotionCache>& Pair : ObjectMotionCaches)
		{
			const FObjectMotionCache& Motion = Pair.Value;
			if (Motion.bStateExposed && !Motion.StateKey.IsEmpty()
				&& Observer->IsContextKeyPendingFlush(Motion.StateKey))
			{
				return;
			}
		}
		Cache.bInitialBaselineDelivered = true;
	};

	// Read the REGISTERED settings instance — the one the Project Settings page edits
	// (Convai::StartupModule registers Convai::Get().GetConvaiSettings(), and the rest of
	// the plugin reads it). NOT GetDefault<>()/the CDO: the CDO is a separate object the
	// live editor toggle never updates, so reading it ignored Project-Settings changes to
	// the spatial toggles until an editor restart reloaded the CDO from config.
	const UConvaiSettings* Settings = Convai::Get().GetConvaiSettings();
	if (!Settings || !Settings->bEnableSpatialAwareness)
	{
		for (UConvaiChatbotComponent* Observer : Subsystem->GetAllChatbotComponents())
		{
			if (!IsValid(Observer) || Observer->PendingContextBatch.bPendingReset)
			{
				continue;
			}
			FObserverSpatialCache& Cache = SpatialCaches.FindOrAdd(Observer);
			Cache.bSpatialAwarenessWasActive = false;
			for (const TPair<FString, FObserverSpatialCache::FFact>& Pair : Cache.Facts)
			{
				Observer->RemoveContextFact(Pair.Key);
			}
			Cache.Facts.Empty();
			Cache.NavCache.Empty();
			AdvanceInitialBaseline(Observer, Cache);
			Cache.bInitialBaselineQueued = true;
		}
		for (auto It = SpatialCaches.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid())
			{
				It.RemoveCurrent();
			}
		}
		return;
	}

	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;

	// ── Gather every subject once (objects + characters + players) ──
	TArray<FSpatialEntity> Subjects;
	// Objects are gathered as LOGICAL objects: BuildObjectGroups collapses a merged
	// same-named set into one group (centroid position, shared name), and leaves every
	// other object as a group of one. One FSpatialEntity per group.
	TArray<UConvaiSubsystem::FConvaiObjectGroup> ObjectGroups;
	Subsystem->BuildObjectGroups(ObjectGroups);
	for (const UConvaiSubsystem::FConvaiObjectGroup& Group : ObjectGroups)
	{
		// Include the logical object only if at least one member opts into spatial
		// awareness; that member also supplies the line-of-sight frame/actor.
		UConvaiObjectComponent* Representative = nullptr;
		for (UConvaiObjectComponent* Member : Group.Members)
		{
			if (IsValid(Member) && Member->bIncludeInSpatialAwareness)
			{
				Representative = Member;
				break;
			}
		}
		if (!Representative) { continue; }
		AActor* Owner = Representative->GetOwner();
		if (!IsValid(Owner)) { continue; }

		FSpatialEntity E;
		E.Name = Group.Name;
		// Keyed by the (unique, stable) logical-object name so change-gating and
		// retirement track the merged object correctly across polls.
		E.CacheKey = FString::Printf(TEXT("Object:%s"), *Group.Name.ToLower());
		E.Location = Group.Centroid;
		const FResolvedMovementSettings StateSettings = ResolveMovementSettings(Group);
		E.bReportsMovementState =
			StateSettings.bStateEnabled && !StateSettings.bStateKeyCollision;
		if (E.bReportsMovementState)
		{
			E.MotionStateKey = ConvaiContextFormat::SanitizeKey(
				FString::Printf(TEXT("%s.Movement"), *Group.Name));
			E.MotionDelivery = StateSettings.Delivery;
		}
		if (StateSettings.bAwarenessEnabled)
		{
			if (const FObjectMotionCache* Motion = ObjectMotionCaches.Find(E.CacheKey))
			{
				E.bIsMoving = Motion->bMoving;
				E.MotionVelocity = Motion->SemanticVelocity;
				E.bRotationOnly = Motion->bRotationOnly;
				E.bMotionReinitialized = Motion->bReinitializedThisPoll;

				const ConvaiSpatial::EMotionTransition Transition =
					static_cast<ConvaiSpatial::EMotionTransition>(Motion->TransitionThisPoll);
				E.bMotionBaselineCorrection = !Motion->bReinitializedThisPoll
					&& Motion->bAwaitingPostReinitMotion
					&& Transition == ConvaiSpatial::EMotionTransition::Stopped;
				if (StateSettings.bStateEnabled && !StateSettings.bStateKeyCollision
					&& ConvaiSpatial::IsGameplayMotionTransition(Transition,
						Motion->bReinitializedThisPoll,
						Motion->bAwaitingPostReinitMotion))
				{
					E.bHasMotionTransition = true;
					const double MotionNow = World
						? static_cast<double>(World->GetTimeSeconds()) : FPlatformTime::Seconds();
					if (MotionNow - Motion->InitializedAt >= 1.0)
					{
						E.MotionTransitionResponse =
							Transition == ConvaiSpatial::EMotionTransition::Started
								? StateSettings.StartedResponse
								: StateSettings.StoppedResponse;
					}
				}
			}
		}
		E.Actor = Owner;
		E.Priority = 0;
		E.bIsObject = true;
		E.ObjectMembers = Group.Members;
		// Union of member bounds (component bounds when component-scoped, else
		// actor colliding bounds) — consistent with the centroid Location.
		FBox GroupBox(ForceInit);
		for (UConvaiObjectComponent* Member : Group.Members)
		{
			if (!IsValid(Member) || !IsValid(Member->GetOwner())) { continue; }
			if (USceneComponent* Comp = Member->GetResolvedComponent())
			{
				const FVector MemberLocation = Comp->GetComponentLocation();
				if (!MemberLocation.ContainsNaN())
				{
					E.ProximityLocations.Add(MemberLocation);
				}
				GroupBox += FBox::BuildAABB(Comp->Bounds.Origin, Comp->Bounds.BoxExtent);
			}
			else
			{
				const FVector MemberLocation = Member->GetOwner()->GetActorLocation();
				if (!MemberLocation.ContainsNaN())
				{
					E.ProximityLocations.Add(MemberLocation);
				}
				FVector BO, BE;
				GetPawnBlockingBounds(Member->GetOwner(), BO, BE);
				GroupBox += FBox::BuildAABB(BO, BE);
			}
		}
		if (GroupBox.IsValid)
		{
			E.BoundsOrigin = GroupBox.GetCenter();
			E.BoundsExtent = GroupBox.GetExtent();
		}

		// ── Named movement points → virtual sub-subjects ──
		// "Door" + points named "Other Side" also emits a "Door Other Side"
		// subject located at those points' centroid, with its own facts,
		// direction, and nav verdict. The base subject then owns only the
		// unnamed points (mirroring the entry expansion at registration).
		TArray<FString> SubNames;
		{
			TArray<const FConvaiObjectEntry*> MemberEntries;
			for (UConvaiObjectComponent* Member : Group.Members)
			{
				if (IsValid(Member)) { MemberEntries.Add(&Member->ObjectEntry); }
			}
			FConvaiObjectEntry::CollectMovementPointSubNames(MemberEntries, SubNames);
		}
		E.bUnnamedPointsOnly = SubNames.Num() > 0;
		Subjects.Add(MoveTemp(E));

		for (const FString& SubName : SubNames)
		{
			const FString Normalized = FConvaiObjectEntry::NormalizeMovementPointName(SubName);
			const FString KeyLower = Normalized.ToLower();
			// A generated name colliding with a REAL logical object would give
			// two spatial subjects one cache key (ambiguous facts/reachability).
			// The real object wins; registration already warns about the skip.
			const FString GeneratedLower = FString::Printf(TEXT("%s %s"), *Group.Name, *Normalized).ToLower();
			const bool bCollidesWithRealObject = ObjectGroups.ContainsByPredicate(
				[&GeneratedLower](const UConvaiSubsystem::FConvaiObjectGroup& G)
				{ return G.Name.ToLower() == GeneratedLower; });
			if (bCollidesWithRealObject)
			{
				continue;
			}
			FVector Sum = FVector::ZeroVector;
			TArray<FVector> ProximityLocations;
			int32 Count = 0;
			for (UConvaiObjectComponent* Member : Group.Members)
			{
				if (!IsValid(Member) || !IsValid(Member->GetOwner())) { continue; }
				USceneComponent* Anchor = Member->GetResolvedComponent();
				for (const FConvaiMovementPoint& Point : Member->ObjectEntry.MovementPoints)
				{
					if (Point.bEnabled && FConvaiObjectEntry::EffectiveMovementPointName(Point).ToLower() == KeyLower)
					{
						const FVector PointLocation = Point.ResolveWorldLocation(Member->GetOwner(), Anchor);
						if (PointLocation.ContainsNaN()) { continue; }
						Sum += PointLocation;
						ProximityLocations.Add(PointLocation);
						++Count;
					}
				}
			}
			if (Count == 0) { continue; }

			FSpatialEntity S;
			S.Name = FString::Printf(TEXT("%s %s"), *Group.Name, *SubName);
			S.CacheKey = FString::Printf(TEXT("Object:%s"), *S.Name.ToLower());
			S.Location = Sum / Count;
			S.LosLocation = Group.Centroid; // seen whenever the object is seen
			S.Actor = Owner;
			S.Priority = 0;
			S.bIsObject = true;
			S.ObjectMembers = Group.Members;
			S.ProximityLocations = MoveTemp(ProximityLocations);
			S.PointNameFilter = Normalized;
			Subjects.Add(MoveTemp(S));
		}
	}
	for (UConvaiChatbotComponent* Bot : Subsystem->GetAllChatbotComponents())
	{
		if (!IsValid(Bot)) { continue; }
		AActor* Owner = Bot->GetOwner();
		if (!IsValid(Owner)) { continue; }
		const FString Name = Bot->GetConversationalName();
		if (Name.IsEmpty()) { continue; }

		FSpatialEntity E;
		E.Name = Name;
		E.CacheKey = FString::Printf(TEXT("Chatbot:%s"), *Bot->GetPathName());
		E.Location = Owner->GetActorLocation();
		E.Forward = Owner->GetActorForwardVector();
		E.Actor = Owner;
		E.Priority = 1;
		E.bHasFacing = true;
		E.AsChatbot = Bot;
		GetPawnBlockingBounds(Owner, E.BoundsOrigin, E.BoundsExtent);
		Subjects.Add(MoveTemp(E));
	}
	for (UConvaiPlayerComponent* Player : Subsystem->GetAllPlayerComponents())
	{
		if (!IsValid(Player)) { continue; }
		AActor* Owner = Player->GetOwner();
		if (!IsValid(Owner)) { continue; }
		// GetConversationalName() is public on the base UConvaiConversationComponent,
		// but UConvaiPlayerComponent declares its override private — call via the base.
		const FString Name = static_cast<const UConvaiConversationComponent*>(Player)->GetConversationalName();
		if (Name.IsEmpty()) { continue; }

		FSpatialEntity E;
		E.Name = Name;
		E.CacheKey = FString::Printf(TEXT("Player:%s"), *Player->GetPathName());
		E.Location = Owner->GetActorLocation();
		E.Forward = Owner->GetActorForwardVector();
		E.Actor = Owner;
		E.Priority = 2;
		E.bHasFacing = true;
		if (const APawn* Pawn = Cast<APawn>(Owner))
		{
			if (const APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
			{
				FVector ViewLoc; FRotator ViewRot;
				PC->GetPlayerViewPoint(ViewLoc, ViewRot);
				E.ViewRotation = FRotator(0.0f, ViewRot.Yaw, 0.0f).Quaternion();
				E.bHasViewRotation = true;
			}
		}
		GetPawnBlockingBounds(Owner, E.BoundsOrigin, E.BoundsExtent);
		Subjects.Add(MoveTemp(E));
	}

	// ── Drop caches for destroyed observers ──
	for (auto It = SpatialCaches.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	// ── Per observer: compose one Context Fact per subject ──
	for (UConvaiChatbotComponent* Observer : Subsystem->GetAllChatbotComponents())
	{
		if (!IsValid(Observer)) { continue; }
		// Reset flushes last. While a disconnected reset is pending, rebuilding
		// now would send the baseline and immediately erase it; wait until the
		// reset has actually left, then the next poll seeds the snapshot silently.
		if (Observer->PendingContextBatch.bPendingReset) { continue; }

		const FConvaiSpatialAwarenessPreferences& Pref = Observer->SpatialAwareness;
		FObserverSpatialCache& Cache = SpatialCaches.FindOrAdd(Observer);

		AActor* ObsOwner = Observer->GetOwner();
		const bool bWantsAnything = Pref.bReceiveSurroundings
			|| (Pref.bReceiveRelations && Settings->bEnableRelations);
		const bool bSpatialAwarenessActive = IsValid(ObsOwner) && bWantsAnything;
		if (bSpatialAwarenessActive && !Cache.bSpatialAwarenessWasActive)
		{
			// Turning spatial awareness on creates a new snapshot even in an
			// established session; its first view is baseline knowledge too.
			Cache.bInitialBaselineQueued = false;
			Cache.bInitialBaselineDelivered = false;
		}
		Cache.bSpatialAwarenessWasActive = bSpatialAwarenessActive;
		AdvanceInitialBaseline(Observer, Cache);
		if (!bSpatialAwarenessActive)
		{
			// Nothing wanted (or no body) — clear anything we previously published.
			for (const auto& Pair : Cache.Facts)
			{
				Observer->RemoveContextFact(Pair.Key);
			}
			Cache.Facts.Empty();
			Cache.NavCache.Empty();
			Cache.bHasPlayerPerspectiveFacts = false;
			Cache.bInitialBaselineQueued = true;
			continue;
		}
		// Initial spatial knowledge is a silent baseline, just like the first
		// value of a tracked property. Without this clamp, an Auto/Always
		// surroundings preference raises the whole debounced startup batch and
		// wakes the character at boot.
		const bool bSeedSilently = !Cache.bInitialBaselineDelivered;

		const FVector ObsLoc = ObsOwner->GetActorLocation();
		const FTransform ObsXform = ObsOwner->GetActorTransform();
		FVector EyePos; float HeadAbove, FootBelow;
		ConvaiSpatial::ComputeEyeLine(ObsOwner, EyePos, HeadAbove, FootBelow);

		// The perspective player: whoever this chatbot is most plausibly talking
		// to — the nearest player. Their camera frame powers the "From X's
		// position" clause appended to every fact below.
		const FSpatialEntity* PerspectivePlayer = nullptr;
		float PlayerHeadAbove = 0.0f, PlayerFootBelow = 0.0f;
		if (Settings->bDescribePlayerPerspective && Pref.bReceiveSurroundings)
		{
			float BestPlayerDistSq = TNumericLimits<float>::Max();
			for (const FSpatialEntity& Candidate : Subjects)
			{
				if (Candidate.Priority != 2) { continue; }
				const float DistSq = static_cast<float>(
					FVector::DistSquared(ObsLoc, Candidate.Location));
				if (DistSq < BestPlayerDistSq)
				{
					BestPlayerDistSq = DistSq;
					PerspectivePlayer = &Candidate;
				}
			}
			if (PerspectivePlayer)
			{
				FVector PlayerEyeUnused;
				ConvaiSpatial::ComputeEyeLine(PerspectivePlayer->Actor,
					PlayerEyeUnused, PlayerHeadAbove, PlayerFootBelow);
			}
		}

		TSet<FString> CurrentKeys;
		bool bAnyPlayerClauseThisPass = false;

		for (int32 si = 0; si < Subjects.Num(); ++si)
		{
			const FSpatialEntity& Subj = Subjects[si];
			if (Subj.AsChatbot == Observer) { continue; } // never relative to itself

			// Line-of-sight gate: a subject the chatbot can't see is withheld.
			// Virtual sub-objects trace to their LosLocation (the base object) —
			// a door's far side is occluded by the door itself, but its existence
			// is authored knowledge, known whenever the door is seen. Keyed on
			// PointNameFilter, not an IsZero() sentinel: a base at world origin
			// must not flip a sub back to tracing its own occluded point.
			const FVector LosTarget = !Subj.PointNameFilter.IsEmpty() ? Subj.LosLocation : Subj.Location;
			if (Settings->bEnableLineOfSight &&
				!ConvaiSpatial::HasLineOfSight(World, EyePos, LosTarget, ObsOwner, Subj.Actor))
			{
				continue;
			}

			TArray<FString, TInlineAllocator<2>> Clauses;
			EC_RunLLMOption Resp = EC_RunLLMOption::Never;
			// Which categories actually contributed a clause to THIS fact — its
			// delivery honors only the contributing categories' preferences.
			bool bSurroundingsContributed = false;
			bool bRelationsContributed = false;
			bool bResponsiveMotionLane = false;
			bool bFollowHeldMotionLane = false;
			bool bAlreadyThere = false;

			// Relation to the nearest higher-priority neighbour, computed first so a
			// tight support relation (on top of / underneath / next to) can take over
			// the position description instead of repeating distance/direction.
			FString RelationClause;
			bool bSupportRelation = false;
			if (Pref.bReceiveRelations && Settings->bEnableRelations)
			{
				// Support relations (on top of / underneath / next to) list EVERY
				// outranking entity in tight contact — a plate the observer AND the
				// player both stand on reads "underneath you and User", not just
				// whichever anchor happened to be closest. Directional relations
				// stay single-anchor (closest), as before.
				struct FSupportHit { const FSpatialEntity* T; FString Word; float DistSq; };
				TArray<FSupportHit, TInlineAllocator<4>> SupportHits;
				const float ClusterDistSq = FMath::Square(Settings->RelationClusterDistance);
				for (int32 ti = 0; ti < Subjects.Num(); ++ti)
				{
					if (ti == si) { continue; }
					const FSpatialEntity& T = Subjects[ti];
					const bool bOutranks = (T.Priority > Subj.Priority)
						|| (T.Priority == Subj.Priority && T.CacheKey < Subj.CacheKey);
					if (!bOutranks) { continue; }
					const float DistSq = FVector::DistSquared(Subj.Location, T.Location);
					if (DistSq > ClusterDistSq) { continue; }
					FString Word = SupportRelation(Subj, T);
					if (!Word.IsEmpty())
					{
						SupportHits.Add({ &T, MoveTemp(Word), DistSq });
					}
				}

				if (SupportHits.Num() > 0)
				{
					bSupportRelation = true;
					// Observer first ("underneath you and User"), then nearest-first;
					// cap at 3 so a crowd doesn't turn the fact into a roster.
					SupportHits.Sort([Observer](const FSupportHit& A, const FSupportHit& B)
					{
						const bool bAObs = (A.T->AsChatbot == Observer);
						const bool bBObs = (B.T->AsChatbot == Observer);
						if (bAObs != bBObs) { return bAObs; }
						return A.DistSq < B.DistSq;
					});
					if (SupportHits.Num() > 3) { SupportHits.SetNum(3); }

					// One clause per relation word, names sharing the preposition:
					// "underneath you, User, and Stack Bot"; mixed words join as
					// clauses: "underneath you and next to the Crate".
					TArray<FString, TInlineAllocator<3>> WordClauses;
					TArray<FString, TInlineAllocator<3>> DoneWords;
					for (int32 h = 0; h < SupportHits.Num(); ++h)
					{
						if (DoneWords.Contains(SupportHits[h].Word)) { continue; }
						DoneWords.Add(SupportHits[h].Word);
						TArray<FString, TInlineAllocator<3>> Names;
						for (int32 k = h; k < SupportHits.Num(); ++k)
						{
							if (SupportHits[k].Word == SupportHits[h].Word)
							{
								Names.Add(SupportHits[k].T->AsChatbot == Observer
									? FString(TEXT("you"))
									: MakeDisplayName(*SupportHits[k].T));
							}
						}
						FString NameList = Names[0];
						if (Names.Num() == 2)
						{
							NameList = FString::Printf(TEXT("%s and %s"), *Names[0], *Names[1]);
						}
						else if (Names.Num() == 3)
						{
							NameList = FString::Printf(TEXT("%s, %s, and %s"), *Names[0], *Names[1], *Names[2]);
						}
						WordClauses.Add(FString::Printf(TEXT("%s %s"), *SupportHits[h].Word, *NameList));
					}
					// ", and" (not bare " and") — a multi-name first group would
					// otherwise misparse: "underneath you and User and next to
					// the Crate".
					RelationClause = FString::Join(WordClauses, TEXT(", and "));
				}
				else if (const FSpatialEntity* Anchor =
						FindAnchor(Subj, si, Subjects, Settings->RelationClusterDistance,
							/*bSkipPlayers*/ PerspectivePlayer != nullptr))
				{
					if (Anchor->AsChatbot != Observer)
					{
						RelationClause = ComposeRelationClause(Subj, *Anchor, ObsXform);
					}
					// else: a directional relation to the observer itself would
					// only repeat the egocentric clause composed below — omit it.
				}
			}

			// Category 1 — where the subject is relative to THIS chatbot.
			if (Pref.bReceiveSurroundings)
			{
				const EC_RunLLMOption* HeldMotionResponse =
					!Subj.bHasMotionTransition
						&& Subj.bReportsMovementState
						&& !Subj.MotionStateKey.IsEmpty()
					? Observer->HeldContext.StateRespond.Find(
						Subj.MotionStateKey)
					: nullptr;
				if (Subj.bHasMotionTransition || HeldMotionResponse)
				{
					bFollowHeldMotionLane = HeldMotionResponse != nullptr;
					EC_RunLLMOption MotionResponse = HeldMotionResponse
						? *HeldMotionResponse : Subj.MotionTransitionResponse;
					if (HeldMotionResponse)
					{
						// A fresh Watch Property promotion intentionally leaves
						// the held state's original policy at Never until release,
						// while the paired fact already owns the promoted Always
						// rank. Preserve that rank on later spatial refreshes.
						if (const EC_RunLLMOption* HeldFactResponse =
							Observer->HeldContext.DeclarativeRespond.Find(
								Subj.CacheKey))
						{
							MotionResponse = MaxResp(
								MotionResponse, *HeldFactResponse);
						}
					}
					if (Subj.bHasMotionTransition)
					{
						const FString MotionStateValue =
							ConvaiSpatial::MotionStateValue(
								Subj.bIsMoving, Subj.MotionVelocity,
								Subj.bRotationOnly, ObsXform, Subj.Location);
						const FString* HeldMotionValue =
							Observer->HeldContext.StateValues.Find(
								Subj.MotionStateKey);
						if (Observer->WouldWatchContextStateChange(
							Subj.MotionStateKey, MotionStateValue,
							HeldMotionValue)
							|| Observer->PendingContextBatch.WatchPromotedStateKeys.Contains(
								Subj.MotionStateKey)
							|| Observer->HeldContext.WatchPromotedStateKeys.Contains(
								Subj.MotionStateKey))
						{
							MotionResponse = EC_RunLLMOption::Always;
						}
					}
					Resp = MaxResp(Resp, MotionResponse);
					// A responsive Movement state and the spatial sentence driven
					// by it must leave idle delivery together. If the object shifts
					// spatially while the state is held, keep the refreshed fact in
					// that same lane instead of publishing a contradictory pair.
					bResponsiveMotionLane =
						MotionResponse != EC_RunLLMOption::Never;
				}
				const FVector WorldDelta = Subj.Location - ObsLoc;
				// Nav reachability, every subject. Reused until the observer or the
				// subject moves >100 uu (10000 = 100^2, kept squared) so the pathfind
				// doesn't run every poll. Unreachable -> "no walking path", which
				// is what the AI uses to avoid trying to move to things it can't path
				// to — a player on an unreachable ledge misleads exactly like an
				// unreachable object did.
				bool bReachable = true;
				{
					auto& Nav = Cache.NavCache.FindOrAdd(Subj.CacheKey);
					// Stop-edge: a drift under the 100 uu gate can flip reachability
					// and then settle, leaving the verdict stale forever. Detect the
					// poll where sub-gate motion (either end) stops and recompute
					// once. Gated on bValid so first sight doesn't fake an edge.
					constexpr float PollMotionEpsSq = 4.0f; // ~2 uu between polls
					const bool bMovingNow = Nav.bValid &&
						(  FVector::DistSquared(Subj.Location, Nav.PrevPollObjectLoc)   > PollMotionEpsSq
						|| FVector::DistSquared(ObsLoc,        Nav.PrevPollObserverLoc) > PollMotionEpsSq);
					const bool bJustStopped = Nav.bWasMoving && !bMovingNow;
					Nav.PrevPollObjectLoc   = Subj.Location;
					Nav.PrevPollObserverLoc = ObsLoc;
					Nav.bWasMoving          = bMovingNow;

					const bool bRecompute = !Nav.bValid
						|| bJustStopped
						|| Nav.LastMemberCount != Subj.ObjectMembers.Num()
						|| FVector::DistSquared(ObsLoc, Nav.LastObserverLoc) >= 10000.0f
						|| FVector::DistSquared(Subj.Location, Nav.LastObjectLoc) >= 10000.0f;
					if (bRecompute)
					{
						if (Subj.ObjectMembers.Num() == 0)
						{
							// Characters/players: one pathfind to where the subject
							// stands. Partial or failed = no walking path (the same
							// signal objects emit). Endpoints use nav-agent locations —
							// raw actor locations are capsule centres above the mesh.
							auto NavLoc = [](AActor* A, const FVector& Fallback)
							{
								if (const INavAgentInterface* NavAgent = Cast<INavAgentInterface>(A))
								{
									return NavAgent->GetNavAgentLocation();
								}
								return IsValid(A) ? A->GetActorLocation() : Fallback;
							};
							UNavigationSystemV1* NavSys = World
								? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
							UNavigationPath* Path = NavSys
								? NavSys->FindPathToLocationSynchronously(World,
									NavLoc(ObsOwner, ObsLoc), NavLoc(Subj.Actor, Subj.Location), ObsOwner)
								: nullptr;
							Nav.bReachable = Path && Path->IsValid() && !Path->IsPartial();
							Nav.bAlreadyThere = ConvaiSpatial::IsPersonAlreadyThere(
								Nav.bReachable,
								FVector::Dist2D(ObsLoc, Subj.Location),
								FMath::Abs(ObsLoc.Z - Subj.Location.Z),
								Path ? Path->GetPathLength() : -1.0);
						}
						else
						{
							// Reachable if ANY member can be pathed to — a scattered pile is
							// reachable as long as one of its objects is. Also track the NEAREST
							// reachable member so a merged object's move-to target points at the
							// closest one the bot can actually walk to. "Nearest" is nav path
							// length (what the bot actually walks — resolves through each
							// member's Movement Points), with straight-line distance only as
							// the already-there tie-in (travel distance is 0 there).
							UConvaiObjectComponent* NearestReachable = nullptr;
							float NearestTravel = TNumericLimits<float>::Max();
							bool bAtAnyMember = false;
							for (UConvaiObjectComponent* Member : Subj.ObjectMembers)
							{
								if (!IsValid(Member)) { continue; }
								// Named-point subjects resolve a filtered copy: only the
								// member's points with that name (relative points keep
								// their own actor as anchor — pooling them onto one
								// entry would anchor them wrong). The base subject of
								// an expanded set resolves only its unnamed points.
								FConvaiObjectEntry FilteredEntry;
								FConvaiObjectEntry* EntryToResolve = &Member->ObjectEntry;
								if (!Subj.PointNameFilter.IsEmpty())
								{
									FilteredEntry = Member->ObjectEntry;
									FilteredEntry.FilterMovementPointsToName(Subj.PointNameFilter);
									if (FilteredEntry.MovementPoints.Num() == 0) { continue; }
									FilteredEntry.bFallbackToObjectWhenPointsUnreachable = false;
									EntryToResolve = &FilteredEntry;
								}
								else if (Subj.bUnnamedPointsOnly)
								{
									FilteredEntry = Member->ObjectEntry;
									FilteredEntry.FilterMovementPointsToObjectItself();
									EntryToResolve = &FilteredEntry;
								}
								AActor* GA = nullptr; USceneComponent* GC = nullptr;
								FVector ResolvedLoc = FVector::ZeroVector; float AR = 0.0f;
								bool Mode = false;
								bool bResolved = false, bMemberAlreadyThere = false, bReach = false;
								FVector PathEnd = FVector::ZeroVector; TArray<FVector> PathPoints;
								float TravelDist = 0.0f; int32 PointIdx = INDEX_NONE;
								EntryToResolve->ResolveGoalLocation(ObsOwner,
									GA, GC, ResolvedLoc, AR, Mode, bResolved, bMemberAlreadyThere, bReach, PathEnd, PathPoints,
									TravelDist, PointIdx);
								bAtAnyMember |= bMemberAlreadyThere;
								if (ConvaiSpatial::HasUsableDestination(
									bReach, bMemberAlreadyThere) && IsValid(Member->GetOwner()))
								{
									// Already-there beats everything (travel 0 by definition —
									// don't substitute a distance that would let a short path to
									// another member outrank the one the bot is standing at).
									// Reachable-but-not-pathed edge cases fall back to
									// straight-line so they still rank sensibly.
									const float Travel = bMemberAlreadyThere
										? 0.0f
										: ((TravelDist > 0.0f) ? TravelDist : FVector::Dist(ObsLoc, ResolvedLoc));
									if (Travel < NearestTravel)
									{
										NearestTravel = Travel;
										NearestReachable = Member;
									}
								}
							}
							Nav.bReachable = (NearestReachable != nullptr);
							Nav.bAlreadyThere = bAtAnyMember;

							// For a merged set, repoint THIS chatbot's object entry at the nearest
							// reachable member so "go to <name>" walks to the closest one. A single
							// object's Ref is already itself. Local nav target only — Ref isn't part
							// of the server payload, so this causes no scene-metadata churn.
							// Movement Points travel along: relative points re-anchor to the new
							// Ref's transform, and per-member differences (a member with its own
							// authored points) stay correct.
							if (Subj.ObjectMembers.Num() > 1 && NearestReachable)
							{
								// Kind must match: a destination subject may only
								// repoint destination entries, a base subject only
								// real ones — a same-named REAL object must never
								// have its movement config overwritten by a
								// movement point (fresh-codex P1).
								const bool bSubjectIsDestination = !Subj.PointNameFilter.IsEmpty();
								if (FConvaiObjectEntry* MovingEntry = Observer->EnvironmentData.Objects.FindByPredicate(
										[&Subj, bSubjectIsDestination](const FConvaiObjectEntry& E)
										{ return E.Name == Subj.Name && E.bIsMovementPointSubObject == bSubjectIsDestination; }))
								{
									// Carry the nearest member's whole movement configuration —
									// a stale radius/reference/fallback from the old representative
									// would misresolve against the new Ref. Name/description stay:
									// they identify the logical merged object.
									const FConvaiObjectEntry& Src = NearestReachable->ObjectEntry;
									MovingEntry->Ref = NearestReachable->GetOwner();
									MovingEntry->MovementPoints = Src.MovementPoints;
									MovingEntry->bFallbackToObjectWhenPointsUnreachable = Src.bFallbackToObjectWhenPointsUnreachable;
									MovingEntry->ObjectReference = Src.ObjectReference;
									MovingEntry->AcceptanceRadius = Src.AcceptanceRadius;
									MovingEntry->ComponentName = Src.ComponentName;
									MovingEntry->SocketOrBoneName = Src.SocketOrBoneName;
									MovingEntry->ResolvedComponent = nullptr;
									// A sub-object entry must keep only ITS points from
									// the new representative; the base of an expanded
									// set keeps only the unnamed ones.
									if (!Subj.PointNameFilter.IsEmpty())
									{
										MovingEntry->FilterMovementPointsToName(Subj.PointNameFilter);
										MovingEntry->bFallbackToObjectWhenPointsUnreachable = false;
									}
									else if (Subj.bUnnamedPointsOnly)
									{
										MovingEntry->FilterMovementPointsToObjectItself();
									}
								}
							}
						}
						Nav.LastObserverLoc = ObsLoc;
						Nav.LastObjectLoc = Subj.Location;
						Nav.LastMemberCount = Subj.ObjectMembers.Num();
						Nav.bValid = true;
					}
					bAlreadyThere = Nav.bAlreadyThere;
					bReachable = ConvaiSpatial::HasUsableDestination(
						Nav.bReachable, bAlreadyThere);
				}

				// A merged logical object may span a room. Its centroid remains the stable
				// directional and relational anchor, but distance describes the nearest
				// concrete copy instead of an empty point between copies.
				const float ProximityDistance = ConvaiContextPrivate::ComputeNearestDistance(
					ObsLoc,
					Subj.ProximityLocations,
					Subj.Location);
				const ConvaiSpatial::EProximityBand Band =
					ConvaiSpatial::ClassifyDistance(ProximityDistance, bReachable,
						Settings->NearbyDistance, Settings->ModerateDistance);
				const FString Motion = ConvaiSpatial::MotionStatusPhrase(
					Subj.bIsMoving, Subj.bReportsMovementState,
					Subj.MotionVelocity, Subj.bRotationOnly, ObsXform, Subj.Location);

				if (bSupportRelation && !RelationClause.IsEmpty())
				{
					// Position is already pinned by the support relation, so drop the
					// redundant distance + direction but keep an explicit current nav
					// verdict. The positive wording is important when a previously
					// unreachable subject becomes reachable again.
					// Extra predicates retain the sentence's original subject; do not
					// repeat its name or use an ambiguous "it" after an anchor.
					// When already there, the explicit arrival sentence below
					// replaces both walking verdicts and remains unambiguous
					// after a support relation.
					RelationClause = ConvaiSpatial::ComposeSupportClause(
						RelationClause, Motion, /*bIncludeReachability*/ !bAlreadyThere,
						Band != ConvaiSpatial::EProximityBand::Unreachable);
					Resp = MaxResp(Resp, Pref.SurroundingsResponse);
					bSurroundingsContributed = true;
				}
				else
				{
					FString Ego;
					if (!bAlreadyThere)
					{
						const FVector LocalDelta = ObsXform.InverseTransformVectorNoScale(WorldDelta);
						const ConvaiSpatial::FDirection Dir =
							ConvaiSpatial::ComputeDirection(LocalDelta, HeadAbove, FootBelow);
						Ego = ConvaiSpatial::BandPhrase(Band, ConvaiSpatial::DirectionToYou(Dir));
					}
					if (Subj.bHasFacing)
					{
						const FString Facing = ConvaiSpatial::FacingPhrase(Subj.Forward, Subj.Location, ObsLoc);
						if (!Facing.IsEmpty())
						{
							Ego = Ego.IsEmpty() ? Facing
								: FString::Printf(TEXT("%s, %s"), *Ego, *Facing);
						}
					}
					if (!Motion.IsEmpty())
					{
						if (Ego.IsEmpty())
						{
							Ego = Motion;
						}
						else if (RelationClause.IsEmpty())
						{
							Ego = FString::Printf(TEXT("%s, and %s"), *Ego, *Motion);
						}
						else
						{
							Ego = FString::Printf(TEXT("%s, %s"), *Ego, *Motion);
						}
					}
					if (!Ego.IsEmpty())
					{
						Clauses.Add(Ego);
					}
					Resp = MaxResp(Resp, Pref.SurroundingsResponse);
					bSurroundingsContributed = true;
				}
			}

			// Category 2 — how the subject relates to a nearby higher-priority
			// neighbour (bagged into this same fact, described once per pair).
			if (!RelationClause.IsEmpty())
			{
				Clauses.Add(RelationClause);
				Resp = MaxResp(Resp, Pref.RelationsResponse);
				bRelationsContributed = true;
			}

			const FString JoinedClauses = JoinClauses(Clauses);
			FString Sentence;
			if (bAlreadyThere)
			{
				Sentence = ConvaiSpatial::AlreadyThereFact(
					MakeDisplayName(Subj), /*bIsPerson*/ !Subj.bIsObject, JoinedClauses);
			}
			else if (!JoinedClauses.IsEmpty())
			{
				Sentence = FString::Printf(TEXT("%s is %s."),
					*MakeDisplayName(Subj), *JoinedClauses);
			}
			if (Sentence.IsEmpty()) { continue; }
			if (Sentence.Len() > 0)
			{
				Sentence[0] = FChar::ToUpper(Sentence[0]);
			}

			// Player-perspective clause: the same subject located from the
			// perspective player's camera frame, with the frame named explicitly
			// so no "you" can be misread. Never distance-gated — beyond the
			// moderate band it degrades to a bare "far away", actively
			// overwriting an earlier "close by" instead of leaving it stale in
			// the model's history. Direction is dropped in the far band: it
			// flips with every player turn and is rarely asked about at range.
			FString PlayerClause;
			if (PerspectivePlayer && Subj.CacheKey != PerspectivePlayer->CacheKey)
			{
				const float PlayerDistance = ConvaiContextPrivate::ComputeNearestDistance(
					PerspectivePlayer->Location, Subj.ProximityLocations, Subj.Location);
				const ConvaiSpatial::EProximityBand PlayerBand =
					ConvaiSpatial::ClassifyDistance(PlayerDistance, /*bReachable*/ true,
						Settings->NearbyDistance, Settings->ModerateDistance);
				FString Body = ConvaiSpatial::BandWord(PlayerBand);
				if (PlayerBand != ConvaiSpatial::EProximityBand::Far)
				{
					const FQuat PlayerFrame = PerspectivePlayer->bHasViewRotation
						? PerspectivePlayer->ViewRotation
						: PerspectivePlayer->Actor->GetActorQuat();
					const FVector PlayerLocal = PlayerFrame.UnrotateVector(
						Subj.Location - PerspectivePlayer->Location);
					const FString DirectionWords = ConvaiSpatial::DirectionPlain(
						ConvaiSpatial::ComputeDirection(PlayerLocal,
							PlayerHeadAbove, PlayerFootBelow));
					if (!DirectionWords.IsEmpty())
					{
						Body += TEXT(", ") + DirectionWords;
					}
				}
					// The subject is re-named inside the clause (not "it"): these
				// clauses arrive as a wall of near-identical sentences, and a
				// pronoun lets the model bind the wrong subject's geometry to a
				// question ("where is the 5m target?" answered with the 15m
				// target's clause). Self-contained clauses can't cross-bind.
				PlayerClause = FString::Printf(TEXT(" From %s's position, %s is %s."),
					*PerspectivePlayer->Name, *MakeDisplayName(Subj), *Body);
				bAnyPlayerClauseThisPass = true;
			}
			const FString Published = Sentence + PlayerClause;

			// Delivery: wait-until-idle when any CONTRIBUTING category that can
			// actually nudge the AI (non-Never response) asks for it — a
			// Never-response category can't interrupt, so its delivery
			// preference doesn't force the whole fact to wait.
			const bool bForceSilentBaseline = bSeedSilently
				|| Subj.bMotionReinitialized || Subj.bMotionBaselineCorrection;
			const EC_RunLLMOption EffectiveResp = bForceSilentBaseline
				? EC_RunLLMOption::Never
				: bFollowHeldMotionLane
					? Resp
					: ConvaiSpatial::ResolveSnapshotResponse(
						Resp, Cache.bInitialBaselineDelivered);
			EConvaiContextDelivery Deliver = EConvaiContextDelivery::SendNormally;
			if (!bForceSilentBaseline &&
				((bSurroundingsContributed && Pref.SurroundingsResponse != EC_RunLLMOption::Never
					&& Pref.SurroundingsDelivery == EConvaiContextDelivery::WaitUntilConversationIsIdle) ||
				(bRelationsContributed && Pref.RelationsResponse != EC_RunLLMOption::Never
					&& Pref.RelationsDelivery == EConvaiContextDelivery::WaitUntilConversationIsIdle) ||
				(bResponsiveMotionLane
					&& Subj.MotionDelivery == EConvaiContextDelivery::WaitUntilConversationIsIdle)))
			{
				Deliver = EConvaiContextDelivery::WaitUntilConversationIsIdle;
			}

			CurrentKeys.Add(Subj.CacheKey);
			const FObserverSpatialCache::FFact* Last = Cache.Facts.Find(Subj.CacheKey);
			const bool bRebaselineMotionFact =
				Subj.bMotionReinitialized || Subj.bMotionBaselineCorrection;
			if (!Last || Last->Sentence != Published || bRebaselineMotionFact)
			{
				// Ego part unchanged means only the player clause moved (the
				// player walked or turned, or the setting flipped). Republish
				// silently — one player turn changes the clause on every fact in
				// range in the same poll, and that burst must not wake the AI.
				const bool bPlayerClauseOnlyChange = Last && !bRebaselineMotionFact
					&& Last->EgoSentence == Sentence;
				const EC_RunLLMOption PublishResp = bPlayerClauseOnlyChange
					? EC_RunLLMOption::Never : EffectiveResp;
				const EConvaiContextDelivery PublishDeliver = bPlayerClauseOnlyChange
					? EConvaiContextDelivery::SendNormally : Deliver;
				if (bRebaselineMotionFact)
				{
					// Structural recovery is baseline truth, not a queued
					// surroundings/movement wake-up. Clear max-rank metadata before
					// re-staging even when the rendered sentence happens to match.
					Observer->HeldContext.DropDeclarativeKey(Subj.CacheKey);
					Observer->PendingContextBatch.DropDeclarativeKey(Subj.CacheKey);
				}
				else if (Subj.bHasMotionTransition)
				{
					// Start/stop edges are authoritative for both sources driven by
					// this detector. Withdraw an older pending or held surroundings
					// sentence before staging the current edge, so a stale motion
					// direction cannot flush first or retain a stronger wake-up rank.
					Observer->HeldContext.DropDeclarativeKey(Subj.CacheKey);
					Observer->PendingContextBatch.WithdrawDeclarativeForHold(
						Observer->DynamicContextTracker, Subj.CacheKey);
				}
				if (bFollowHeldMotionLane && !bRebaselineMotionFact)
				{
					// This is a sibling of an already-held Movement value, not a
					// new independent response decision. Force it into the same
					// lane even when a fresh Watch Property keeps the state's
					// authored response at Never until release.
					Observer->PendingContextBatch.WithdrawDeclarativeForHold(
						Observer->DynamicContextTracker, Subj.CacheKey);
					Observer->HeldContext.HoldDeclarative(
						Subj.CacheKey, Published, PublishResp);
				}
				else
				{
					Observer->SetContextFact(Published, Subj.CacheKey,
						PublishResp, PublishDeliver, /*bFlushImmediately*/ false);
				}
				Cache.Facts.Add(Subj.CacheKey, { Subj.Name, Published, Sentence });
			}
		}

		// Retire facts for subjects that dropped out (destroyed, out of view, or
		// no longer wanted).
		for (auto It = Cache.Facts.CreateIterator(); It; ++It)
		{
			if (!CurrentKeys.Contains(It.Key()))
			{
				Observer->RemoveContextFact(It.Key());
				It.RemoveCurrent();
			}
		}

		// Drop nav-cache entries for subjects no longer present this pass.
		for (auto It = Cache.NavCache.CreateIterator(); It; ++It)
		{
			if (!CurrentKeys.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}

		Cache.bHasPlayerPerspectiveFacts = bAnyPlayerClauseThisPass;

		// One complete poll has now had the chance to stage both the snapshot and,
		// immediately after this pass, every enabled synthetic Movement state.
		Cache.bInitialBaselineQueued = true;
	}
}
