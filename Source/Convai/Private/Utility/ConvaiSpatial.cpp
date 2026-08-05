// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Utility/ConvaiSpatial.h"

#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
// FHitResult lived in Engine/EngineTypes.h up to UE 5.0; it was split into its
// own Engine/HitResult.h in 5.1. Select whichever exists so this translation
// unit gets the full definition (not just a forward declaration) on 5.0-5.7.
// This only makes the type visible — it does not change behaviour on any version.
#if __has_include("Engine/HitResult.h")
#include "Engine/HitResult.h"
#else
#include "Engine/EngineTypes.h"
#endif

namespace
{
	// Join 1-3 phrase parts with natural "and" / comma punctuation, or return the
	// fallback when there are none.
	FString JoinParts(const TArray<FString, TInlineAllocator<3>>& Parts, const TCHAR* EmptyFallback)
	{
		switch (Parts.Num())
		{
			case 0:  return EmptyFallback;
			case 1:  return Parts[0];
			case 2:  return FString::Printf(TEXT("%s and %s"), *Parts[0], *Parts[1]);
			default: return FString::Printf(TEXT("%s, %s, and %s"), *Parts[0], *Parts[1], *Parts[2]);
		}
	}

	// Predicate lists follow the sentence's already-written subject + "is". A compound
	// relation such as "underneath you and Player" needs a comma before the
	// next predicate or it reads as though Player were reachable or moving.
	FString JoinPredicates(const TArray<FString, TInlineAllocator<3>>& Parts)
	{
		switch (Parts.Num())
		{
			case 0: return FString();
			case 1: return Parts[0];
			case 2:
				if (Parts[0].Contains(TEXT(" and ")))
				{
					return FString::Printf(TEXT("%s, and %s"), *Parts[0], *Parts[1]);
				}
				return FString::Printf(TEXT("%s and %s"), *Parts[0], *Parts[1]);
			default:
				return FString::Printf(TEXT("%s, %s, and %s"),
					*Parts[0], *Parts[1], *Parts[2]);
		}
	}

	enum class EMotionDirection : uint8
	{
		Moving,
		Upward,
		Downward,
		TowardObserver,
		AwayFromObserver,
		Left,
		Right,
		Rotating
	};

	// One classifier feeds both the detailed spatial phrase and the compact
	// Movement property, so they cannot disagree about direction.
	EMotionDirection ClassifyMotionDirection(const FVector& WorldVelocity,
		bool bRotationOnly,
		const FTransform& ObserverTransform, const FVector& SubjectLocation)
	{
		if (bRotationOnly)
		{
			return EMotionDirection::Rotating;
		}
		if (WorldVelocity.Size() <= ConvaiSpatial::MotionMinimumLinearSpeed)
		{
			return EMotionDirection::Moving;
		}

		const FVector Direction = WorldVelocity.GetSafeNormal();
		const float HorizontalMagnitude = FVector(Direction.X, Direction.Y, 0.0f).Size();
		// Give world vertical priority only when it is genuinely dominant. This
		// keeps a gently sloped platform from churning between "up" and a useful
		// horizontal direction.
		if (FMath::Abs(Direction.Z) >= FMath::Max(0.55f, HorizontalMagnitude * 0.85f))
		{
			return Direction.Z > 0.0f
				? EMotionDirection::Upward : EMotionDirection::Downward;
		}

		FVector HorizontalVelocity(Direction.X, Direction.Y, 0.0f);
		HorizontalVelocity.Normalize();

		FVector ToObserver = ObserverTransform.GetLocation() - SubjectLocation;
		ToObserver.Z = 0.0f;
		const bool bHasRadialFrame = ToObserver.Normalize();
		const float Radial = bHasRadialFrame
			? FVector::DotProduct(HorizontalVelocity, ToObserver) : 0.0f;

		FVector ObserverRight = ObserverTransform.GetUnitAxis(EAxis::Y);
		ObserverRight.Z = 0.0f;
		ObserverRight.Normalize();
		const float Lateral = FVector::DotProduct(HorizontalVelocity, ObserverRight);

		// Toward/away conveys the useful closing/opening relationship and is
		// stable even when the observer is not facing the subject. Fall back to
		// the observer's left/right for motion across their view.
		if (bHasRadialFrame && FMath::Abs(Radial) >= 0.55f)
		{
			return Radial > 0.0f
				? EMotionDirection::TowardObserver : EMotionDirection::AwayFromObserver;
		}
		if (FMath::Abs(Lateral) >= 0.45f)
		{
			return Lateral > 0.0f ? EMotionDirection::Right : EMotionDirection::Left;
		}
		return EMotionDirection::Moving;
	}

	FString MotionDirectionPhrase(const FVector& WorldVelocity, bool bRotationOnly,
		const FTransform& ObserverTransform, const FVector& SubjectLocation)
	{
		switch (ClassifyMotionDirection(
			WorldVelocity, bRotationOnly, ObserverTransform, SubjectLocation))
		{
			case EMotionDirection::Upward:          return TEXT("moving upward");
			case EMotionDirection::Downward:        return TEXT("moving downward");
			case EMotionDirection::TowardObserver:  return TEXT("moving toward you");
			case EMotionDirection::AwayFromObserver: return TEXT("moving away from you");
			case EMotionDirection::Left:            return TEXT("moving to your left");
			case EMotionDirection::Right:           return TEXT("moving to your right");
			case EMotionDirection::Rotating:        return TEXT("rotating");
			default:                                return TEXT("moving");
		}
	}
}

namespace ConvaiSpatial
{
	FMotionThresholds GetMotionThresholds(EConvaiMovementSensitivity Sensitivity)
	{
		float Multiplier = 1.0f;
		switch (Sensitivity)
		{
			case EConvaiMovementSensitivity::VeryLow:  Multiplier = 4.0f; break;
			case EConvaiMovementSensitivity::Low:      Multiplier = 2.0f; break;
			case EConvaiMovementSensitivity::High:     Multiplier = 0.5f; break;
			case EConvaiMovementSensitivity::VeryHigh: Multiplier = 0.25f; break;
			case EConvaiMovementSensitivity::Medium:
			default:                                   Multiplier = 1.0f; break;
		}

		FMotionThresholds Result;
		Result.StartLinearSpeed = MotionStartLinearSpeed * Multiplier;
		Result.StopLinearSpeed = MotionStopLinearSpeed * Multiplier;
		Result.StartAngularSpeed = MotionStartAngularSpeed * Multiplier;
		Result.StopAngularSpeed = MotionStopAngularSpeed * Multiplier;
		Result.LinearExcursionFloor = MotionLinearExcursionFloor * Multiplier;
		Result.AngularExcursionFloor = MotionAngularExcursionFloor * Multiplier;
		return Result;
	}

	FDirection ComputeDirection(const FVector& LocalDelta, float HeadAboveUU, float FootBelowUU)
	{
		FDirection Dir;

		const float Magnitude = LocalDelta.Size();
		if (Magnitude < KINDA_SMALL_NUMBER)
		{
			Dir.bColocated = true;
			return Dir;
		}

		const FVector N = LocalDelta / Magnitude;
		constexpr float Threshold = 0.35f;
		if (FMath::Abs(N.X) > Threshold) { Dir.Forward = N.X > 0 ? EForwardDir::Front : EForwardDir::Behind; }
		if (FMath::Abs(N.Y) > Threshold) { Dir.Lateral = N.Y > 0 ? ELateralDir::Right : ELateralDir::Left; }

		// Vertical is anchored on the observer's own silhouette, not the
		// normalised axis: only objects clearly above the head or below the feet
		// (with a 30 uu grace band) get tagged, so at-eye-level targets stay level.
		constexpr float VerticalGraceUU = 30.0f;
		if (LocalDelta.Z > HeadAboveUU + VerticalGraceUU)      { Dir.Vertical = EVerticalDir::Above; }
		else if (LocalDelta.Z < FootBelowUU - VerticalGraceUU) { Dir.Vertical = EVerticalDir::Below; }

		return Dir;
	}

	FString DirectionToYou(const FDirection& Dir)
	{
		if (Dir.bColocated) { return TEXT("right next to you"); }

		TArray<FString, TInlineAllocator<3>> Parts;
		if (Dir.Forward == EForwardDir::Front)       { Parts.Add(TEXT("in front of you")); }
		else if (Dir.Forward == EForwardDir::Behind) { Parts.Add(TEXT("behind you")); }
		if (Dir.Lateral == ELateralDir::Right)       { Parts.Add(TEXT("to your right")); }
		else if (Dir.Lateral == ELateralDir::Left)   { Parts.Add(TEXT("to your left")); }
		if (Dir.Vertical == EVerticalDir::Above)     { Parts.Add(TEXT("above you")); }
		else if (Dir.Vertical == EVerticalDir::Below){ Parts.Add(TEXT("below you")); }

		// No axis tags means the subject is within the observer's own height
		// envelope and horizontal cone — i.e. effectively at their position
		// ("nearby" here composed into nonsense like "close by, nearby").
		return JoinParts(Parts, TEXT("right next to you"));
	}

	FString DirectionToRef(const FDirection& Dir, const FString& RefName)
	{
		if (Dir.bColocated) { return FString::Printf(TEXT("right next to %s"), *RefName); }

		// Prepositional cores that all read naturally immediately before the ref
		// name ("in front of Crate", "to the right of Crate", "above Crate"), so
		// the name appears once at the end regardless of how many axes combine.
		TArray<FString, TInlineAllocator<3>> Cores;
		if (Dir.Forward == EForwardDir::Front)       { Cores.Add(TEXT("in front of")); }
		else if (Dir.Forward == EForwardDir::Behind) { Cores.Add(TEXT("behind")); }
		if (Dir.Lateral == ELateralDir::Right)       { Cores.Add(TEXT("to the right of")); }
		else if (Dir.Lateral == ELateralDir::Left)   { Cores.Add(TEXT("to the left of")); }
		if (Dir.Vertical == EVerticalDir::Above)     { Cores.Add(TEXT("above")); }
		else if (Dir.Vertical == EVerticalDir::Below){ Cores.Add(TEXT("below")); }

		const FString Joined = JoinParts(Cores, TEXT("near"));
		return FString::Printf(TEXT("%s %s"), *Joined, *RefName);
	}

	EProximityBand ClassifyDistance(float DistanceUU, bool bReachable, float NearbyUU, float ModerateUU)
	{
		if (!bReachable)            { return EProximityBand::Unreachable; }
		if (DistanceUU < NearbyUU)  { return EProximityBand::Nearby; }
		if (DistanceUU < ModerateUU){ return EProximityBand::Moderate; }
		return EProximityBand::Far;
	}

	FString BandPhrase(EProximityBand Band, const FString& Direction)
	{
		switch (Band)
		{
			case EProximityBand::Unreachable: return FString::Printf(TEXT("%s, with no walking path there"), *Direction);
			case EProximityBand::Nearby:      return FString::Printf(TEXT("close by, %s, reachable by walking"), *Direction);
			case EProximityBand::Moderate:    return FString::Printf(TEXT("some distance away, %s, reachable by walking"), *Direction);
			case EProximityBand::Far:
			default:                          return FString::Printf(TEXT("far away, %s, reachable by walking"), *Direction);
		}
	}

	FString ComposeSupportClause(const FString& Relation, const FString& Motion,
		bool bIncludeReachability, bool bReachable)
	{
		TArray<FString, TInlineAllocator<3>> Predicates;
		if (!Relation.IsEmpty())
		{
			Predicates.Add(Relation);
		}
		if (!Motion.IsEmpty())
		{
			Predicates.Add(Motion);
		}
		if (!bIncludeReachability)
		{
			return JoinPredicates(Predicates);
		}

		if (bReachable)
		{
			Predicates.Add(TEXT("is reachable by walking"));
			return JoinPredicates(Predicates);
		}

		const FString PositionAndMotion = JoinPredicates(Predicates);
		return PositionAndMotion.IsEmpty()
			? TEXT("not reachable by walking")
			: FString::Printf(TEXT("%s, but there is no walking path there"),
				*PositionAndMotion);
	}

	FString AlreadyThereSentence(const FString& DisplayName, bool bIsPerson)
	{
		return bIsPerson
			? FString::Printf(TEXT("You are already with %s."), *DisplayName)
			: FString::Printf(TEXT("You are already at %s."), *DisplayName);
	}

	FString AlreadyThereFact(const FString& DisplayName, bool bIsPerson,
		const FString& CurrentClause)
	{
		if (CurrentClause.IsEmpty())
		{
			return AlreadyThereSentence(DisplayName, bIsPerson);
		}
		return bIsPerson
			? FString::Printf(TEXT("You are already with %s, who is %s."),
				*DisplayName, *CurrentClause)
			: FString::Printf(TEXT("You are already at %s, which is %s."),
				*DisplayName, *CurrentClause);
	}

	EMotionTransition UpdateMotionState(bool bAboveStart, bool bBelowStop,
		double NowSeconds, bool& bInOutMoving, double& CandidateSince,
		double StartConfirmationSeconds, double StopConfirmationSeconds)
	{
		const bool bCandidateCondition = bInOutMoving ? bBelowStop : bAboveStart;
		if (!bCandidateCondition)
		{
			CandidateSince = -1.0;
			return EMotionTransition::None;
		}

		if (CandidateSince < 0.0)
		{
			CandidateSince = NowSeconds;
		}

		const double RequiredSeconds = bInOutMoving
			? FMath::Max(0.0, StopConfirmationSeconds)
			: FMath::Max(0.0, StartConfirmationSeconds);
		if (NowSeconds - CandidateSince < RequiredSeconds)
		{
			return EMotionTransition::None;
		}

		CandidateSince = -1.0;
		bInOutMoving = !bInOutMoving;
		return bInOutMoving ? EMotionTransition::Started : EMotionTransition::Stopped;
	}

	FMotionTrend ComputeMotionTrend(const FVector& StartLocation,
		const FQuat& StartRotation, double StartTimeSeconds,
		const FVector& EndLocation, const FQuat& EndRotation,
		double EndTimeSeconds, float MaxLinearExcursion,
		float MaxAngularExcursion, double MinWindowSeconds)
	{
		FMotionTrend Result;
		const double DeltaSeconds = EndTimeSeconds - StartTimeSeconds;
		if (DeltaSeconds <= static_cast<double>(SMALL_NUMBER))
		{
			return Result;
		}

		Result.MaxLinearExcursion = FMath::Max(0.0f, MaxLinearExcursion);
		Result.MaxAngularExcursion = FMath::Max(0.0f, MaxAngularExcursion);
		Result.LinearVelocity = (EndLocation - StartLocation) / DeltaSeconds;
		const double RotationDot = FMath::Clamp(
			FMath::Abs(static_cast<double>(
				StartRotation.GetNormalized() | EndRotation.GetNormalized())),
			0.0, 1.0);
		Result.AngularSpeed = static_cast<float>(
			FMath::RadiansToDegrees(2.0 * FMath::Acos(RotationDot)) / DeltaSeconds);
		Result.bReady = DeltaSeconds >= FMath::Max(
			MinWindowSeconds, static_cast<double>(SMALL_NUMBER));
		return Result;
	}

	bool IsMotionTrendQuiet(const FMotionTrend& Trend,
		const FMotionThresholds& Thresholds)
	{
		return Trend.bReady
			&& Trend.LinearVelocity.Size() <= Thresholds.StopLinearSpeed
			&& Trend.AngularSpeed <= Thresholds.StopAngularSpeed
			&& Trend.MaxLinearExcursion <= Thresholds.LinearExcursionFloor
			&& Trend.MaxAngularExcursion <= Thresholds.AngularExcursionFloor;
	}

	bool IsMotionStartEvidence(bool bRawAboveStart, const FMotionTrend& Trend,
		const FMotionThresholds& Thresholds)
	{
		if (!bRawAboveStart)
		{
			return false;
		}
		if (Trend.bReady)
		{
			return !IsMotionTrendQuiet(Trend, Thresholds);
		}
		return Trend.LinearVelocity.Size() >= Thresholds.StartLinearSpeed
			|| Trend.AngularSpeed >= Thresholds.StartAngularSpeed;
	}

	bool IsGameplayMotionTransition(EMotionTransition Transition,
		bool bReinitializedThisPoll, bool bAwaitingPostReinitMotion)
	{
		return !bReinitializedThisPoll
			&& Transition != EMotionTransition::None
			&& !(bAwaitingPostReinitMotion
				&& Transition == EMotionTransition::Stopped);
	}

	bool HasMeaningfulMotionDirectionChange(const FVector& PreviousVelocity,
		bool bPreviousRotationOnly, const FVector& NewVelocity,
		bool bNewRotationOnly)
	{
		if (bPreviousRotationOnly != bNewRotationOnly)
		{
			return true;
		}
		if (bNewRotationOnly)
		{
			return false;
		}

		const bool bHadLinearDirection =
			PreviousVelocity.Size() > MotionMinimumLinearSpeed;
		const bool bHasLinearDirection =
			NewVelocity.Size() > MotionMinimumLinearSpeed;
		if (bHadLinearDirection != bHasLinearDirection)
		{
			return true;
		}
		return bHasLinearDirection
			&& FVector::DotProduct(PreviousVelocity.GetSafeNormal(),
				NewVelocity.GetSafeNormal()) < 0.98f;
	}

	bool HasClearMotionDirectionEvidence(
		const FVector& RawLinearVelocity,
		float RawAngularSpeed,
		const FVector& FilteredLinearVelocity,
		float FilteredAngularSpeed,
		bool bRotationOnly,
		const FMotionThresholds& Thresholds)
	{
		if (bRotationOnly)
		{
			return FilteredAngularSpeed >= Thresholds.StartAngularSpeed
				&& RawAngularSpeed >= Thresholds.StartAngularSpeed;
		}
		if (FilteredLinearVelocity.Size() < Thresholds.StartLinearSpeed)
		{
			return false;
		}
		// A mature rolling trend can briefly retain an endpoint rebound after the
		// object has already become quiet. Require the latest sample to still make
		// start-level progress in that same direction before relabelling.
		return FVector::DotProduct(
			RawLinearVelocity,
			FilteredLinearVelocity.GetSafeNormal())
				>= Thresholds.StartLinearSpeed;
	}

	bool HasCoherentClearLinearDirectionEvidence(
		const FVector& SemanticCandidate,
		const FVector& ClearVelocitySum,
		int32 ClearVelocityCount,
		int32 ActiveVelocityCount)
	{
		if (ClearVelocityCount <= 0 || ActiveVelocityCount <= 0)
		{
			return false;
		}
		if (SemanticCandidate.Size() <= MotionMinimumLinearSpeed)
		{
			return ClearVelocityCount == ActiveVelocityCount;
		}
		if (ClearVelocitySum.Size() <= MotionMinimumLinearSpeed)
		{
			return false;
		}
		return FVector::DotProduct(
			SemanticCandidate.GetSafeNormal(),
			ClearVelocitySum.GetSafeNormal()) >= 0.98f;
	}

	bool IsMotionDirectionCandidateConfirmed(
		bool bCurrentlyCoherent,
		double CoherentSeconds,
		float IntegratedProgress,
		float RequiredProgress,
		double ConfirmationSeconds)
	{
		return bCurrentlyCoherent
			&& CoherentSeconds >= FMath::Max(0.0, ConfirmationSeconds)
			&& IntegratedProgress >= FMath::Max(0.0f, RequiredProgress);
	}

	float MotionDirectionCandidateProgressSpeed(
		const FVector& CandidateVelocity,
		bool bRotationOnly,
		const FVector& EvidenceVelocitySum,
		float EvidenceLinearSpeedSum,
		int32 EvidenceLinearVelocityCount,
		float EvidenceAngularSpeed)
	{
		if (bRotationOnly)
		{
			return FMath::Max(0.0f, EvidenceAngularSpeed);
		}
		if (EvidenceLinearVelocityCount <= 0)
		{
			return 0.0f;
		}
		if (CandidateVelocity.Size() <= MotionMinimumLinearSpeed)
		{
			return FMath::Max(0.0f,
				EvidenceLinearSpeedSum
					/ static_cast<float>(EvidenceLinearVelocityCount));
		}
		const FVector EvidenceAverageVelocity =
			EvidenceVelocitySum
				/ static_cast<float>(EvidenceLinearVelocityCount);
		return FMath::Max(0.0f, FVector::DotProduct(
			EvidenceAverageVelocity,
			CandidateVelocity.GetSafeNormal()));
	}

	bool IsInsideMotionSettlingEnvelope(
		const FVector& AnchorLocation,
		const FQuat& AnchorRotation,
		const FVector& CurrentLocation,
		const FQuat& CurrentRotation,
		const FMotionThresholds& Thresholds,
		float EnvelopeMultiplier)
	{
		const float SafeMultiplier = FMath::Max(0.0f, EnvelopeMultiplier);
		if (FVector::Dist(AnchorLocation, CurrentLocation)
			> Thresholds.LinearExcursionFloor * SafeMultiplier)
		{
			return false;
		}
		const double RotationDot = FMath::Clamp(
			FMath::Abs(static_cast<double>(
				AnchorRotation.GetNormalized()
					| CurrentRotation.GetNormalized())),
			0.0, 1.0);
		const float AngularDisplacement = static_cast<float>(
			FMath::RadiansToDegrees(2.0 * FMath::Acos(RotationDot)));
		return AngularDisplacement
			<= Thresholds.AngularExcursionFloor * SafeMultiplier;
	}

	bool HasMotionSettlingEvidence(
		bool bBecameIncoherent,
		int32 DirectionSwitchCount,
		bool bCurrentDirectionCoherent,
		bool bCurrentDirectionStable)
	{
		return (bBecameIncoherent
				&& !bCurrentDirectionCoherent)
			|| (DirectionSwitchCount >= 2
				&& !bCurrentDirectionStable);
	}

	bool ShouldRebaseEscapedMotionEpisode(
		bool bEpisodeActive,
		bool bEscapedEnvelope,
		bool bCurrentDirectionCoherent,
		bool bMatchesDirectionCandidate,
		bool bResumedAfterCoherenceLoss)
	{
		return bEpisodeActive
			&& bEscapedEnvelope
			&& bCurrentDirectionCoherent
			&& (!bMatchesDirectionCandidate
				|| bResumedAfterCoherenceLoss);
	}

	bool CanUseBoundedMotionSettling(
		bool bHasSettlingEvidence,
		bool bEscapedEnvelope,
		bool bCandidateConfirmed,
		bool bCurrentDirectionStable,
		double EpisodeSeconds,
		bool bAngularTrendQuiet,
		double SettleSeconds)
	{
		return bHasSettlingEvidence
			&& !bEscapedEnvelope
			&& !bCandidateConfirmed
			&& !bCurrentDirectionStable
			&& EpisodeSeconds >= FMath::Max(0.0, SettleSeconds)
			&& bAngularTrendQuiet;
	}

	FString MotionPhrase(const FVector& WorldVelocity, bool bRotationOnly,
		const FTransform& ObserverTransform, const FVector& SubjectLocation)
	{
		const float Speed = WorldVelocity.Size();
		FString Phrase = MotionDirectionPhrase(
			WorldVelocity, bRotationOnly, ObserverTransform, SubjectLocation);

		// Ordinary speed carries no adjective. Only the extremes earn tokens.
		if (!bRotationOnly && Speed > MotionMinimumLinearSpeed && Speed < 80.0f)
		{
			Phrase += TEXT(" slowly");
		}
		else if (!bRotationOnly && Speed >= 500.0f)
		{
			Phrase += TEXT(" quickly");
		}
		return Phrase;
	}

	FString MotionStateValue(bool bIsMoving, const FVector& WorldVelocity,
		bool bRotationOnly, const FTransform& ObserverTransform,
		const FVector& SubjectLocation)
	{
		if (!bIsMoving)
		{
			return TEXT("Stopped");
		}
		switch (ClassifyMotionDirection(
			WorldVelocity, bRotationOnly, ObserverTransform, SubjectLocation))
		{
			case EMotionDirection::Upward:           return TEXT("Upward");
			case EMotionDirection::Downward:         return TEXT("Downward");
			case EMotionDirection::TowardObserver:   return TEXT("Toward You");
			case EMotionDirection::AwayFromObserver: return TEXT("Away");
			case EMotionDirection::Left:             return TEXT("Left");
			case EMotionDirection::Right:            return TEXT("Right");
			case EMotionDirection::Rotating:         return TEXT("Rotating");
			default:                                 return TEXT("Moving");
		}
	}

	FString MotionStatusPhrase(bool bIsMoving, bool bReportStopped,
		const FVector& WorldVelocity, bool bRotationOnly,
		const FTransform& ObserverTransform, const FVector& SubjectLocation)
	{
		if (bIsMoving)
		{
			return MotionPhrase(WorldVelocity, bRotationOnly,
				ObserverTransform, SubjectLocation);
		}
		return bReportStopped ? TEXT("stopped") : FString();
	}

	void ComputeEyeLine(const AActor* Actor, FVector& OutEyePos, float& OutHeadAboveUU, float& OutFootBelowUU)
	{
		constexpr float DefaultHalfExtentUU = 60.0f;
		OutHeadAboveUU =  DefaultHalfExtentUU;
		OutFootBelowUU = -DefaultHalfExtentUU;

		if (!Actor)
		{
			OutEyePos = FVector::ZeroVector;
			return;
		}

		const FVector Loc = Actor->GetActorLocation();
		FVector Origin = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		Actor->GetActorBounds(/*bOnlyCollidingComponents*/ true, Origin, Extent);
		if (Extent.Z < KINDA_SMALL_NUMBER)
		{
			Actor->GetActorBounds(/*bOnlyCollidingComponents*/ false, Origin, Extent);
		}

		const bool bHasBounds = Extent.Z >= KINDA_SMALL_NUMBER;
		if (bHasBounds)
		{
			OutHeadAboveUU = (Origin.Z + Extent.Z) - Loc.Z;
			OutFootBelowUU = (Origin.Z - Extent.Z) - Loc.Z;
		}

		OutEyePos = bHasBounds
			? FVector(Origin.X, Origin.Y, Origin.Z + Extent.Z)
			: FVector(Loc.X, Loc.Y, Loc.Z + DefaultHalfExtentUU);
	}

	bool HasLineOfSight(const UWorld* World, const FVector& From, const FVector& To,
		const AActor* IgnoreActor, const AActor* ObjectActor)
	{
		if (!World)
		{
			return true;
		}

		FCollisionQueryParams Params(TEXT("ConvaiSpatialLineOfSight"), /*bTraceComplex*/ false);
		if (IgnoreActor)
		{
			Params.AddIgnoredActor(IgnoreActor);
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (const APlayerController* PC = It->Get())
			{
				if (const APawn* PlayerPawn = PC->GetPawn())
				{
					Params.AddIgnoredActor(PlayerPawn);
				}
			}
		}

		FHitResult Hit;
		const bool bBlocked = World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params);
		return !bBlocked || Hit.GetActor() == ObjectActor;
	}

	FString FacingPhrase(const FVector& SubjectForward, const FVector& SubjectLoc, const FVector& ObserverLoc)
	{
		FVector ToObserver = ObserverLoc - SubjectLoc;
		ToObserver.Z = 0.0f;
		FVector Forward = SubjectForward;
		Forward.Z = 0.0f;
		if (ToObserver.IsNearlyZero() || Forward.IsNearlyZero())
		{
			return FString();
		}
		ToObserver.Normalize();
		Forward.Normalize();

		// Dot > 0.5 ≈ within 60° of looking at the observer; < -0.5 ≈ looking away.
		// The sideways band in between is left unsaid to avoid clutter.
		const float Dot = FVector::DotProduct(Forward, ToObserver);
		if (Dot > 0.5f)  { return TEXT("facing toward you"); }
		if (Dot < -0.5f) { return TEXT("facing away from you"); }
		return FString();
	}
}
