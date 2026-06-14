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
			default: return FString::Printf(TEXT("%s, %s and %s"), *Parts[0], *Parts[1], *Parts[2]);
		}
	}
}

namespace ConvaiSpatial
{
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

		return JoinParts(Parts, TEXT("nearby"));
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
			case EProximityBand::Unreachable: return FString::Printf(TEXT("%s, with no walking path to it"), *Direction);
			case EProximityBand::Nearby:      return FString::Printf(TEXT("close by, %s"), *Direction);
			case EProximityBand::Moderate:    return FString::Printf(TEXT("some distance away, %s"), *Direction);
			case EProximityBand::Far:
			default:                          return FString::Printf(TEXT("far away, %s"), *Direction);
		}
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
