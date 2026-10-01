// Copyright Convai Inc. All Rights Reserved.

#include "SceneObjects/ConvaiMovementPointGenerator.h"

#include "ConvaiDefinitions.h"
#include "ConvaiObjectComponent.h"

#include "AI/Navigation/NavigationTypes.h"
#include "CollisionShape.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#if __has_include("Engine/HitResult.h")
#include "Engine/HitResult.h"
#else
#include "Engine/EngineTypes.h"
#endif
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelUtils.h"
#include "Misc/SecureHash.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ConvaiMovementPointGenerator"

namespace ConvaiMovementPointGeneratorPrivate
{
	constexpr TCHAR GeneratedMarkerPrefix[] = TEXT("ConvaiMovementPoint.Generated.v1.");
	constexpr int32 DirectionSampleCount = 12;
	constexpr int32 MaximumGeneratedPoints = 2;
	constexpr float MinimumAgentRadius = 34.0f;
	constexpr float MinimumAgentHeight = 176.0f;
	constexpr float MinimumEyeHeight = 160.0f;
	constexpr float MinimumSurfaceStandOffDistance = 90.0f;
	constexpr float MaximumSurfaceStandOffDistance = 160.0f;
	constexpr float AgentSurfacePadding = 45.0f;
	constexpr float MinimumCapsuleToObjectClearance = 25.0f;
	constexpr float ObjectScaleContributionFactor = 0.20f;
	constexpr float MaximumObjectScaleContribution = 60.0f;
	constexpr float MaximumSurfaceProjectionDeviation = 20.0f;
	constexpr float VisibilityCorridorRadius = 12.5f;
	constexpr float VisibilityCorridorTargetInset = VisibilityCorridorRadius + 2.0f;
	constexpr float VisibilitySampleDeduplicationDistance = 1.0f;
	constexpr float MinimumVisibilityRatio = 0.60f;
	constexpr float MinimumPointSeparationDegrees = 40.0f;

	enum class EPlanIssue : uint8
	{
		None,
		InvalidComponent,
		ExistingPoints,
		NoGeneratedPoints,
		NotEditable,
		MissingNavigation,
		NoValidCandidate,
	};

	struct FGeneratedCandidate
	{
		FVector Location = FVector::ZeroVector;
		FVector2D Direction = FVector2D::ZeroVector;
		float SideAlignment = 0.0f;
		float Score = 0.0f;
		int32 SampleIndex = INDEX_NONE;
	};

	struct FVisibilityMeasurement
	{
		float VisibleRatio = 0.0f;
		int32 VisibleSamples = 0;
		int32 TotalSamples = 0;
		bool bCenterVisible = false;
	};

	struct FGenerationPlan
	{
		TWeakObjectPtr<UConvaiObjectComponent> Component;
		TWeakObjectPtr<AActor> Owner;
		FName OriginalComponentObjectName;
		FString OriginalConvaiName;
		EConvaiObjectReference OriginalObjectReference = EConvaiObjectReference::WholeActor;
		FString OriginalComponentFilter;
		TArray<FConvaiMovementPoint> FinalPoints;
		TArray<FName> FinalTags;
		EPlanIssue Issue = EPlanIssue::None;
		int32 GeneratedPoints = 0;
		int32 RemovedGeneratedPoints = 0;
		int32 PreservedManualPoints = 0;
		bool bChanged = false;
	};

	UConvaiObjectComponent* ResolveComponentAfterEditorReconstruction(
		const FGenerationPlan& Plan)
	{
		AActor* Owner = Plan.Owner.Get();
		UConvaiObjectComponent* OriginalComponent = Plan.Component.Get();
		if (!IsValid(Owner))
		{
			return nullptr;
		}
		if (IsValid(OriginalComponent) && OriginalComponent->GetOwner() == Owner
			&& Owner->OwnsComponent(OriginalComponent))
		{
			return OriginalComponent;
		}

		TArray<UConvaiObjectComponent*> Components;
		Owner->GetComponents<UConvaiObjectComponent>(Components);
		UConvaiObjectComponent* MatchingEntry = nullptr;
		for (UConvaiObjectComponent* Candidate : Components)
		{
			if (!IsValid(Candidate))
			{
				continue;
			}
			if (Candidate->GetFName() == Plan.OriginalComponentObjectName)
			{
				return Candidate;
			}
			if (!MatchingEntry
				&& Candidate->ObjectEntry.Name == Plan.OriginalConvaiName
				&& Candidate->ObjectEntry.ObjectReference == Plan.OriginalObjectReference
				&& Candidate->ObjectEntry.ComponentName == Plan.OriginalComponentFilter)
			{
				MatchingEntry = Candidate;
			}
		}
		return MatchingEntry;
	}

	bool MovementPointArraysMatch(
		const TArray<FConvaiMovementPoint>& Left,
		const TArray<FConvaiMovementPoint>& Right)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			const FConvaiMovementPoint& LeftPoint = Left[Index];
			const FConvaiMovementPoint& RightPoint = Right[Index];
			if (!LeftPoint.Transform.Equals(RightPoint.Transform)
				|| LeftPoint.Attachment != RightPoint.Attachment
				|| LeftPoint.bEnabled != RightPoint.bEnabled
				|| LeftPoint.bCreatesSeparateDestination
					!= RightPoint.bCreatesSeparateDestination
				|| LeftPoint.Name != RightPoint.Name)
			{
				return false;
			}
		}
		return true;
	}

	bool IsGeneratedMarker(const FName Tag)
	{
		return Tag.ToString().StartsWith(GeneratedMarkerPrefix, ESearchCase::CaseSensitive);
	}

	FName BuildGeneratedMarker(const FConvaiMovementPoint& Point)
	{
		const FVector Location = Point.Transform.GetLocation();
		const FQuat Rotation = Point.Transform.GetRotation();
		const FString Identity = FString::Printf(
			TEXT("%.6f|%.6f|%.6f|%.6f|%.6f|%.6f|%.6f|%u|%u|%u|%s"),
			Location.X,
			Location.Y,
			Location.Z,
			Rotation.X,
			Rotation.Y,
			Rotation.Z,
			Rotation.W,
			static_cast<uint8>(Point.Attachment),
			Point.bEnabled ? 1u : 0u,
			Point.bCreatesSeparateDestination ? 1u : 0u,
			*Point.Name);
		return FName(*(FString(GeneratedMarkerPrefix) + FMD5::HashAnsiString(*Identity)));
	}

	int32 StripGeneratedPoints(
		TArray<FConvaiMovementPoint>& Points,
		TArray<FName>& ComponentTags)
	{
		TMap<FName, int32> MarkerCounts;
		for (const FName Tag : ComponentTags)
		{
			if (IsGeneratedMarker(Tag))
			{
				MarkerCounts.FindOrAdd(Tag) += 1;
			}
		}
		ComponentTags.RemoveAll([](const FName Tag)
		{
			return IsGeneratedMarker(Tag);
		});

		int32 RemovedCount = 0;
		// Generated points are appended by this tool. Scanning backwards also
		// preserves an older manual point when it happens to be byte-identical.
		for (int32 PointIndex = Points.Num() - 1; PointIndex >= 0; --PointIndex)
		{
			const FName Marker = BuildGeneratedMarker(Points[PointIndex]);
			int32* RemainingCount = MarkerCounts.Find(Marker);
			if (RemainingCount && *RemainingCount > 0)
			{
				Points.RemoveAt(PointIndex);
				--*RemainingCount;
				++RemovedCount;
			}
		}
		return RemovedCount;
	}

	UPrimitiveComponent* FindTargetPrimitive(const UConvaiObjectComponent& Component)
	{
		AActor* Owner = Component.GetOwner();
		if (!IsValid(Owner)
			|| Component.ObjectEntry.ObjectReference != EConvaiObjectReference::SpecificComponent
			|| Component.ObjectEntry.ComponentName.IsEmpty())
		{
			return nullptr;
		}

		TArray<USceneComponent*> SceneComponents;
		Owner->GetComponents<USceneComponent>(SceneComponents);
		USceneComponent* Match = nullptr;
		int32 MatchCount = 0;
		for (USceneComponent* SceneComponent : SceneComponents)
		{
			if (!IsValid(SceneComponent)
				|| !SceneComponent->GetName().Contains(
					Component.ObjectEntry.ComponentName,
					ESearchCase::IgnoreCase))
			{
				continue;
			}
			Match = SceneComponent;
			++MatchCount;
		}
		// Runtime component filters use substring matching. Generation is stricter:
		// an ambiguous or non-renderable target must not receive plausible-looking
		// points authored relative to a different component.
		return MatchCount == 1 ? Cast<UPrimitiveComponent>(Match) : nullptr;
	}

	bool ResolveTargetBounds(
		const UConvaiObjectComponent& Component,
		FBox& OutBounds,
		UPrimitiveComponent*& OutTargetPrimitive)
	{
		AActor* Owner = Component.GetOwner();
		if (!IsValid(Owner))
		{
			return false;
		}
		OutTargetPrimitive = FindTargetPrimitive(Component);
		if (Component.ObjectEntry.ObjectReference == EConvaiObjectReference::SpecificComponent
			&& !OutTargetPrimitive)
		{
			return false;
		}
		OutBounds = OutTargetPrimitive
			? OutTargetPrimitive->Bounds.GetBox()
			: Owner->GetComponentsBoundingBox(true, false);
		return OutBounds.IsValid && OutBounds.GetExtent().GetMax() > KINDA_SMALL_NUMBER;
	}

	float HorizontalDistanceToFootprint(
		const FVector& Point,
		const FBox& Bounds)
	{
		const double DeltaX = FMath::Max3(
			Bounds.Min.X - Point.X,
			0.0,
			Point.X - Bounds.Max.X);
		const double DeltaY = FMath::Max3(
			Bounds.Min.Y - Point.Y,
			0.0,
			Point.Y - Bounds.Max.Y);
		return static_cast<float>(FVector2D(DeltaX, DeltaY).Size());
	}

	float ComputeSurfaceStandOffDistance(
		const FVector& TargetExtent,
		const float AgentRadius)
	{
		// AcceptanceRadius is an arrival tolerance, not physical clearance. Keep
		// the authored point close to the visible surface while scaling gently for
		// larger targets and preserving room for the navigation-agent capsule.
		const float SafeMinimum = AgentRadius + MinimumCapsuleToObjectClearance;
		const float MinimumDistance = FMath::Max(
			MinimumSurfaceStandOffDistance,
			SafeMinimum);
		const float MaximumDistance = FMath::Max(
			MaximumSurfaceStandOffDistance,
			SafeMinimum + MaximumObjectScaleContribution);
		const float ObjectScaleContribution = FMath::Clamp(
			static_cast<float>(TargetExtent.Size()) * ObjectScaleContributionFactor,
			0.0f,
			MaximumObjectScaleContribution);
		return FMath::Clamp(
			AgentRadius + AgentSurfacePadding + ObjectScaleContribution,
			MinimumDistance,
			MaximumDistance);
	}

	FVector ComputeFootprintSeedLocation(
		const FVector& TargetCenter,
		const FVector& TargetExtent,
		const FVector2D& InDirection,
		const float StandOffDistance)
	{
		const FVector2D Direction = InDirection.GetSafeNormal();
		const FVector2D SurfaceOffset(
			FMath::IsNearlyZero(Direction.X, KINDA_SMALL_NUMBER)
				? 0.0f
				: FMath::Sign(Direction.X) * TargetExtent.X,
			FMath::IsNearlyZero(Direction.Y, KINDA_SMALL_NUMBER)
				? 0.0f
				: FMath::Sign(Direction.Y) * TargetExtent.Y);
		return FVector(
			TargetCenter.X + SurfaceOffset.X + Direction.X * StandOffDistance,
			TargetCenter.Y + SurfaceOffset.Y + Direction.Y * StandOffDistance,
			TargetCenter.Z - TargetExtent.Z);
	}

	FVector2D ComputeCandidateDirection(
		const FVector& TargetCenter,
		const FVector& CandidateLocation)
	{
		return FVector2D(
			CandidateLocation.X - TargetCenter.X,
			CandidateLocation.Y - TargetCenter.Y).GetSafeNormal();
	}

	bool IsProjectedSurfaceGapAccepted(
		const float SurfaceGap,
		const float StandOffDistance,
		const float AgentRadius)
	{
		const float MinimumGap = FMath::Max(
			AgentRadius + MinimumCapsuleToObjectClearance,
			StandOffDistance - MaximumSurfaceProjectionDeviation);
		const float MaximumGap = StandOffDistance + MaximumSurfaceProjectionDeviation;
		return SurfaceGap >= MinimumGap && SurfaceGap <= MaximumGap;
	}

	bool IsVisibilityHitAccepted(
		const bool bComponentScoped,
		const bool bHitOwner,
		const bool bHitTargetComponent)
	{
		return bComponentScoped ? bHitTargetComponent : bHitOwner;
	}

	bool IsVisibilityCoverageAccepted(
		const bool bCenterVisible,
		const int32 VisibleSamples,
		const int32 TotalSamples)
	{
		if (!bCenterVisible || TotalSamples < 3
			|| VisibleSamples < 0 || VisibleSamples > TotalSamples)
		{
			return false;
		}
		const int32 MinimumVisibleSamples = FMath::CeilToInt(
			static_cast<float>(TotalSamples) * MinimumVisibilityRatio);
		return VisibleSamples >= FMath::Max(3, MinimumVisibleSamples);
	}

	FVector ComputeTargetLateralSampleOffset(
		const FVector& Center,
		const FVector& Extent,
		const FVector& EyeLocation)
	{
		FVector ViewDirection = Center - EyeLocation;
		ViewDirection.Z = 0.0f;
		if (!ViewDirection.Normalize())
		{
			ViewDirection = FVector::ForwardVector;
		}
		const FVector LateralDirection(
			-ViewDirection.Y,
			ViewDirection.X,
			0.0f);

		float LateralLimit = TNumericLimits<float>::Max();
		if (FMath::Abs(LateralDirection.X) > SMALL_NUMBER)
		{
			LateralLimit = FMath::Min(
				LateralLimit,
				static_cast<float>(Extent.X / FMath::Abs(LateralDirection.X)));
		}
		if (FMath::Abs(LateralDirection.Y) > SMALL_NUMBER)
		{
			LateralLimit = FMath::Min(
				LateralLimit,
				static_cast<float>(Extent.Y / FMath::Abs(LateralDirection.Y)));
		}
		if (!FMath::IsFinite(LateralLimit))
		{
			return FVector::ZeroVector;
		}
		return LateralDirection * (FMath::Max(0.0f, LateralLimit) * 0.30f);
	}

	template <typename AllocatorType>
	void BuildVisibilityTargetSamples(
		const FBox& TargetBounds,
		const FVector& EyeLocation,
		TArray<FVector, AllocatorType>& OutSamples)
	{
		OutSamples.Reset(5);
		const FVector Center = TargetBounds.GetCenter();
		const FVector Extent = TargetBounds.GetExtent();
		const FVector LateralOffset = ComputeTargetLateralSampleOffset(
			Center,
			Extent,
			EyeLocation);
		const FVector Candidates[] = {
			Center,
			Center + FVector(0.0f, 0.0f, Extent.Z * 0.35f),
			Center - FVector(0.0f, 0.0f, Extent.Z * 0.35f),
			Center + LateralOffset,
			Center - LateralOffset,
		};
		const double MinimumSeparationSquared = FMath::Square(
			static_cast<double>(VisibilitySampleDeduplicationDistance));
		for (const FVector& Candidate : Candidates)
		{
			const bool bDuplicate = OutSamples.ContainsByPredicate(
				[&Candidate, MinimumSeparationSquared](const FVector& Existing)
				{
					return FVector::DistSquared(Candidate, Existing)
						<= MinimumSeparationSquared;
				});
			if (!bDuplicate)
			{
				OutSamples.Add(Candidate);
			}
		}
	}

	FVisibilityMeasurement MeasureVisibility(
		UWorld& World,
		AActor& TargetActor,
		UPrimitiveComponent* TargetPrimitive,
		const FBox& TargetBounds,
		const FVector& EyeLocation)
	{
		// Center is authoritative. The other samples measure useful vertical and
		// view-relative lateral coverage while remaining inside the target AABB.
		TArray<FVector, TInlineAllocator<5>> TargetSamples;
		BuildVisibilityTargetSamples(TargetBounds, EyeLocation, TargetSamples);

		FCollisionQueryParams QueryParams(
			SCENE_QUERY_STAT(ConvaiMovementPointVisibility),
			true);
		FVisibilityMeasurement Measurement;
		Measurement.TotalSamples = TargetSamples.Num();
		for (int32 SampleIndex = 0; SampleIndex < TargetSamples.Num(); ++SampleIndex)
		{
			const FVector& TargetSample = TargetSamples[SampleIndex];
			const FVector ToTarget = TargetSample - EyeLocation;
			const double TargetDistance = ToTarget.Size();
			const FVector CorridorEnd = TargetDistance > VisibilityCorridorTargetInset
				? TargetSample - ToTarget / TargetDistance * VisibilityCorridorTargetInset
				: EyeLocation;

			// Stop the wide corridor just before the sample so its leading edge checks
			// almost the entire view without wrapping through a collision-disabled thin
			// panel into the supporting wall behind it.
			FHitResult SweepHit;
			const bool bSweepHit = !CorridorEnd.Equals(EyeLocation)
				&& World.SweepSingleByChannel(
					SweepHit,
					EyeLocation,
					CorridorEnd,
					FQuat::Identity,
					ECC_Visibility,
					FCollisionShape::MakeSphere(VisibilityCorridorRadius),
					QueryParams);
			bool bVisible = bSweepHit && IsVisibilityHitAccepted(
				TargetPrimitive != nullptr,
				SweepHit.GetActor() == &TargetActor,
				TargetPrimitive && SweepHit.GetComponent() == TargetPrimitive);
			if (!bSweepHit)
			{
				// A narrow final segment reaches the requested target sample without the
				// swept sphere extending through the target into support geometry.
				FHitResult FinalHit;
				const bool bFinalHit = World.LineTraceSingleByChannel(
					FinalHit,
					CorridorEnd,
					TargetSample,
					ECC_Visibility,
					QueryParams);
				bVisible = !bFinalHit || IsVisibilityHitAccepted(
					TargetPrimitive != nullptr,
					FinalHit.GetActor() == &TargetActor,
					TargetPrimitive && FinalHit.GetComponent() == TargetPrimitive);
			}
			Measurement.VisibleSamples += bVisible ? 1 : 0;
			if (SampleIndex == 0)
			{
				Measurement.bCenterVisible = bVisible;
			}
		}
		Measurement.VisibleRatio = Measurement.TotalSamples > 0
			? static_cast<float>(Measurement.VisibleSamples)
				/ static_cast<float>(Measurement.TotalSamples)
			: 0.0f;
		return Measurement;
	}

	void SelectCandidateIndices(
		const TArray<FGeneratedCandidate>& Candidates,
		TArray<int32>& OutIndices)
	{
		OutIndices.Reset();
		if (Candidates.IsEmpty())
		{
			return;
		}

		TArray<int32> SortedIndices;
		SortedIndices.Reserve(Candidates.Num());
		for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
		{
			SortedIndices.Add(CandidateIndex);
		}
		SortedIndices.Sort([&Candidates](const int32 LeftIndex, const int32 RightIndex)
		{
			const FGeneratedCandidate& Left = Candidates[LeftIndex];
			const FGeneratedCandidate& Right = Candidates[RightIndex];
			return !FMath::IsNearlyEqual(Left.Score, Right.Score)
				? Left.Score > Right.Score
				: Left.SampleIndex < Right.SampleIndex;
		});

		const int32 BestIndex = SortedIndices[0];
		OutIndices.Add(BestIndex);
		const float MaximumSeparationDot = FMath::Cos(
			FMath::DegreesToRadians(MinimumPointSeparationDegrees));
		// Prefer a counterpart on the opposite lateral side. For exhibits this
		// produces useful left/right escort positions instead of front/back points
		// that either block the view or put the character behind the object.
		for (int32 SortedIndex = 1; SortedIndex < SortedIndices.Num(); ++SortedIndex)
		{
			const int32 CandidateIndex = SortedIndices[SortedIndex];
			const float DirectionDot = FVector2D::DotProduct(
				Candidates[BestIndex].Direction,
				Candidates[CandidateIndex].Direction);
			const bool bOppositeLateralSide = Candidates[BestIndex].SideAlignment
				* Candidates[CandidateIndex].SideAlignment < -0.15f;
			if (DirectionDot <= MaximumSeparationDot && bOppositeLateralSide)
			{
				OutIndices.Add(CandidateIndex);
				return;
			}
		}
		// If only one lateral side is navigable, retain a separated point in the
		// same viewing hemisphere. A pure opposite front/back fallback is omitted.
		for (int32 SortedIndex = 1; SortedIndex < SortedIndices.Num(); ++SortedIndex)
		{
			const int32 CandidateIndex = SortedIndices[SortedIndex];
			const float DirectionDot = FVector2D::DotProduct(
				Candidates[BestIndex].Direction,
				Candidates[CandidateIndex].Direction);
			if (DirectionDot >= 0.0f && DirectionDot <= MaximumSeparationDot)
			{
				OutIndices.Add(CandidateIndex);
				return;
			}
		}
	}

	EPlanIssue BuildGeneratedPoints(
		UConvaiObjectComponent& Component,
		TArray<FConvaiMovementPoint>& OutPoints)
	{
		OutPoints.Reset();
		AActor* Owner = Component.GetOwner();
		UWorld* World = IsValid(Owner) ? Owner->GetWorld() : nullptr;
		if (!IsValid(Owner) || !World || Owner->IsTemplate())
		{
			return EPlanIssue::InvalidComponent;
		}

		FBox TargetBounds(ForceInit);
		UPrimitiveComponent* TargetPrimitive = nullptr;
		if (!ResolveTargetBounds(Component, TargetBounds, TargetPrimitive))
		{
			return EPlanIssue::NoValidCandidate;
		}

		UNavigationSystemV1* NavigationSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		ANavigationData* NavigationData = NavigationSystem
			? NavigationSystem->GetDefaultNavDataInstance(FNavigationSystem::DontCreate)
			: nullptr;
		if (!NavigationSystem || !NavigationData)
		{
			return EPlanIssue::MissingNavigation;
		}

		const FNavDataConfig& NavConfig = NavigationData->GetConfig();
		const float AgentRadius = FMath::Max(NavConfig.AgentRadius, MinimumAgentRadius);
		const float AgentHeight = FMath::Max(NavConfig.AgentHeight, MinimumAgentHeight);
		const float CapsuleHalfHeight = AgentHeight * 0.5f;
		const float EyeHeight = FMath::Max(AgentHeight * 0.90f, MinimumEyeHeight);
		const FVector TargetExtent = TargetBounds.GetExtent();
		const float StandOffDistance = ComputeSurfaceStandOffDistance(
			TargetExtent,
			AgentRadius);
		const float MaximumProjectionError = FMath::Max(
			60.0f,
			StandOffDistance * 0.50f);
		const float MaximumVerticalProjectionError = FMath::Max(180.0f, AgentHeight);
		const FVector ProjectionExtent(
			FMath::Max(100.0f, AgentRadius * 2.0f),
			FMath::Max(100.0f, AgentRadius * 2.0f),
			FMath::Max(250.0f, AgentHeight));
		const FVector TargetCenter = TargetBounds.GetCenter();
		const FVector ReferenceRight = TargetPrimitive
			? TargetPrimitive->GetRightVector()
			: Owner->GetActorRightVector();
		const FVector2D ActorRight = FVector2D(
			ReferenceRight.X,
			ReferenceRight.Y).GetSafeNormal();

		TArray<FGeneratedCandidate> Candidates;
		Candidates.Reserve(DirectionSampleCount);
		for (int32 SampleIndex = 0; SampleIndex < DirectionSampleCount; ++SampleIndex)
		{
			const float Angle = 2.0f * PI * static_cast<float>(SampleIndex)
				/ static_cast<float>(DirectionSampleCount);
			const FVector2D Direction(FMath::Cos(Angle), FMath::Sin(Angle));
			const FVector SeedLocation = ComputeFootprintSeedLocation(
				TargetCenter,
				TargetExtent,
				Direction,
				StandOffDistance);

			FNavLocation ProjectedLocation;
			if (!NavigationSystem->ProjectPointToNavigation(
				SeedLocation,
				ProjectedLocation,
				ProjectionExtent,
				NavigationData))
			{
				continue;
			}
			const float ProjectionError = FVector::Dist2D(SeedLocation, ProjectedLocation.Location);
			const float VerticalProjectionError = FMath::Abs(
				SeedLocation.Z - ProjectedLocation.Location.Z);
			const float ProjectedSurfaceGap = HorizontalDistanceToFootprint(
				ProjectedLocation.Location,
				TargetBounds);
			if (ProjectionError > MaximumProjectionError
				|| VerticalProjectionError > MaximumVerticalProjectionError
				|| !IsProjectedSurfaceGapAccepted(
					ProjectedSurfaceGap,
					StandOffDistance,
					AgentRadius))
			{
				continue;
			}

			FCollisionQueryParams ClearanceParams(
				SCENE_QUERY_STAT(ConvaiMovementPointClearance),
				false);
			const FVector CapsuleCenter = ProjectedLocation.Location
				+ FVector(0.0f, 0.0f, CapsuleHalfHeight + 2.0f);
			if (World->OverlapBlockingTestByChannel(
				CapsuleCenter,
				FQuat::Identity,
				ECC_Pawn,
				FCollisionShape::MakeCapsule(AgentRadius, CapsuleHalfHeight),
				ClearanceParams))
			{
				continue;
			}

			const FVector EyeLocation = ProjectedLocation.Location
				+ FVector(0.0f, 0.0f, EyeHeight);
			const FVisibilityMeasurement Visibility = MeasureVisibility(
				*World,
				*Owner,
				TargetPrimitive,
				TargetBounds,
				EyeLocation);
			if (!IsVisibilityCoverageAccepted(
				Visibility.bCenterVisible,
				Visibility.VisibleSamples,
				Visibility.TotalSamples))
			{
				continue;
			}

			const FVector2D PhysicalDirection = ComputeCandidateDirection(
				TargetCenter,
				ProjectedLocation.Location);
			if (PhysicalDirection.IsNearlyZero())
			{
				continue;
			}

			FGeneratedCandidate& Candidate = Candidates.AddDefaulted_GetRef();
			Candidate.Location = ProjectedLocation.Location;
			Candidate.Direction = PhysicalDirection;
			Candidate.SideAlignment = ActorRight.IsNearlyZero()
				? 0.0f
				: FVector2D::DotProduct(Direction, ActorRight);
			Candidate.SampleIndex = SampleIndex;
			const float ProjectionQuality = 1.0f
				- FMath::Clamp(ProjectionError / MaximumProjectionError, 0.0f, 1.0f);
			const float SidePreference = FMath::Abs(Candidate.SideAlignment);
			Candidate.Score = Visibility.VisibleRatio * 1000.0f
				+ ProjectionQuality * 100.0f
				+ SidePreference * 250.0f;
		}

		TArray<int32> SelectedCandidateIndices;
		SelectCandidateIndices(Candidates, SelectedCandidateIndices);
		if (SelectedCandidateIndices.IsEmpty())
		{
			return EPlanIssue::NoValidCandidate;
		}

		const FTransform AnchorTransform = TargetPrimitive
			? TargetPrimitive->GetComponentTransform()
			: Owner->GetActorTransform();
		for (int32 SelectedIndex = 0;
			SelectedIndex < SelectedCandidateIndices.Num()
				&& OutPoints.Num() < MaximumGeneratedPoints;
			++SelectedIndex)
		{
			const FGeneratedCandidate& Candidate = Candidates[SelectedCandidateIndices[SelectedIndex]];
			const float FacingYaw = (TargetCenter - Candidate.Location).Rotation().Yaw;
			const FTransform WorldTransform(
				FRotator(0.0f, FacingYaw, 0.0f),
				Candidate.Location,
				FVector::OneVector);
			FConvaiMovementPoint& Point = OutPoints.AddDefaulted_GetRef();
			Point.Transform = WorldTransform.GetRelativeTransform(AnchorTransform);
			Point.Transform.SetScale3D(FVector::OneVector);
			Point.Attachment = EConvaiMovementPointAttachment::RelativeToObject;
			Point.bEnabled = true;
			Point.bCreatesSeparateDestination = false;
			Point.Name.Reset();
		}
		return EPlanIssue::None;
	}

	FGenerationPlan BuildPlan(
		UConvaiObjectComponent& Component,
		const EConvaiMovementPointGenerationOperation Operation)
	{
		FGenerationPlan Plan;
		Plan.Component = &Component;
		Plan.Owner = Component.GetOwner();
		Plan.OriginalComponentObjectName = Component.GetFName();
		Plan.OriginalConvaiName = Component.ObjectEntry.Name;
		Plan.OriginalObjectReference = Component.ObjectEntry.ObjectReference;
		Plan.OriginalComponentFilter = Component.ObjectEntry.ComponentName;
		FText EditReason;
		if (!FConvaiMovementPointGenerator::CanEditMovementPoints(
			Component,
			EditReason))
		{
			Plan.Issue = EPlanIssue::NotEditable;
			return Plan;
		}
		Plan.FinalPoints = Component.ObjectEntry.MovementPoints;
		Plan.FinalTags = Component.ComponentTags;
		Plan.RemovedGeneratedPoints = StripGeneratedPoints(Plan.FinalPoints, Plan.FinalTags);
		Plan.PreservedManualPoints = Plan.FinalPoints.Num();

		if (Operation == EConvaiMovementPointGenerationOperation::ClearGenerated)
		{
			Plan.bChanged = Plan.RemovedGeneratedPoints > 0
				|| Plan.FinalTags != Component.ComponentTags;
			Plan.Issue = Plan.bChanged ? EPlanIssue::None : EPlanIssue::NoGeneratedPoints;
			return Plan;
		}

		if (Operation == EConvaiMovementPointGenerationOperation::Generate
			&& (Plan.RemovedGeneratedPoints > 0 || Plan.PreservedManualPoints > 0))
		{
			Plan.Issue = EPlanIssue::ExistingPoints;
			return Plan;
		}
		if (Operation == EConvaiMovementPointGenerationOperation::Regenerate
			&& Plan.RemovedGeneratedPoints == 0)
		{
			Plan.Issue = EPlanIssue::NoGeneratedPoints;
			return Plan;
		}

		TArray<FConvaiMovementPoint> GeneratedPoints;
		Plan.Issue = BuildGeneratedPoints(Component, GeneratedPoints);
		if (Plan.Issue != EPlanIssue::None)
		{
			return Plan;
		}
		for (const FConvaiMovementPoint& Point : GeneratedPoints)
		{
			Plan.FinalPoints.Add(Point);
			Plan.FinalTags.Add(BuildGeneratedMarker(Point));
		}
		Plan.GeneratedPoints = GeneratedPoints.Num();
		Plan.bChanged = Plan.GeneratedPoints > 0 || Plan.RemovedGeneratedPoints > 0;
		return Plan;
	}

	void MarkOwningPackagesDirty(UConvaiObjectComponent& Component)
	{
		Component.MarkPackageDirty();
		if (AActor* Owner = Component.GetOwner())
		{
			Owner->MarkPackageDirty();
			if (ULevel* Level = Owner->GetLevel())
			{
				Level->MarkPackageDirty();
			}
		}
	}
}

FConvaiMovementPointGenerationResult FConvaiMovementPointGenerator::Apply(
	const TArray<TWeakObjectPtr<UConvaiObjectComponent>>& Components,
	const EConvaiMovementPointGenerationOperation Operation,
	const bool bCreateTransaction)
{
	using namespace ConvaiMovementPointGeneratorPrivate;
	FConvaiMovementPointGenerationResult Result;
	if (!IsInGameThread())
	{
		return Result;
	}

	TSet<TWeakObjectPtr<UConvaiObjectComponent>> UniqueComponents;
	for (const TWeakObjectPtr<UConvaiObjectComponent>& Component : Components)
	{
		if (Component.IsValid())
		{
			UniqueComponents.Add(Component);
		}
	}
	Result.RequestedComponents = UniqueComponents.Num();

	TArray<FGenerationPlan> Plans;
	Plans.Reserve(UniqueComponents.Num());
	for (const TWeakObjectPtr<UConvaiObjectComponent>& WeakComponent : UniqueComponents)
	{
		UConvaiObjectComponent* Component = WeakComponent.Get();
		if (!IsValid(Component) || !IsValid(Component->GetOwner()))
		{
			++Result.SkippedComponents;
			++Result.InvalidComponents;
			continue;
		}
		FGenerationPlan Plan = BuildPlan(*Component, Operation);
		Result.PreservedManualPoints += Plan.PreservedManualPoints;
		if (!Plan.bChanged)
		{
			++Result.SkippedComponents;
			Result.ExistingPointComponents += Plan.Issue == EPlanIssue::ExistingPoints ? 1 : 0;
			Result.NoGeneratedPointComponents += Plan.Issue == EPlanIssue::NoGeneratedPoints ? 1 : 0;
			Result.InvalidComponents += Plan.Issue == EPlanIssue::InvalidComponent ? 1 : 0;
			Result.NonEditableComponents += Plan.Issue == EPlanIssue::NotEditable ? 1 : 0;
			Result.MissingNavigationComponents += Plan.Issue == EPlanIssue::MissingNavigation ? 1 : 0;
			Result.NoValidCandidateComponents += Plan.Issue == EPlanIssue::NoValidCandidate ? 1 : 0;
			continue;
		}
		Plans.Add(MoveTemp(Plan));
	}

	if (Plans.IsEmpty())
	{
		return Result;
	}

	const FText TransactionDescription = Operation == EConvaiMovementPointGenerationOperation::ClearGenerated
		? LOCTEXT("ClearGeneratedTransaction", "Clear generated Convai movement points")
		: LOCTEXT("GenerateTransaction", "Generate Convai movement points");
	FScopedTransaction Transaction(TransactionDescription, bCreateTransaction);
	for (FGenerationPlan& Plan : Plans)
	{
		AActor* Owner = Plan.Owner.Get();
		UConvaiObjectComponent* Component = ResolveComponentAfterEditorReconstruction(Plan);
		if (!IsValid(Component) || !IsValid(Owner))
		{
			++Result.SkippedComponents;
			continue;
		}

		Owner->SetFlags(RF_Transactional);
		Component->SetFlags(RF_Transactional);
		Owner->Modify();
		Component->Modify();
		Component->PreEditChange(nullptr);
		Component->ObjectEntry.MovementPoints = Plan.FinalPoints;
		Component->ComponentTags = Plan.FinalTags;
		Component->PostEditChange();
		Component = ResolveComponentAfterEditorReconstruction(Plan);
		if (!IsValid(Component)
			|| Component->ComponentTags != Plan.FinalTags
			|| !MovementPointArraysMatch(
				Component->ObjectEntry.MovementPoints,
				Plan.FinalPoints))
		{
			++Result.SkippedComponents;
			++Result.InvalidComponents;
			continue;
		}
		MarkOwningPackagesDirty(*Component);

		++Result.ChangedComponents;
		Result.GeneratedPoints += Plan.GeneratedPoints;
		Result.RemovedGeneratedPoints += Plan.RemovedGeneratedPoints;
	}
	if (Result.ChangedComponents > 0 && GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}
	return Result;
}

int32 FConvaiMovementPointGenerator::CountGeneratedPoints(
	const UConvaiObjectComponent& Component)
{
	TArray<FConvaiMovementPoint> Points = Component.ObjectEntry.MovementPoints;
	TArray<FName> Tags = Component.ComponentTags;
	return ConvaiMovementPointGeneratorPrivate::StripGeneratedPoints(Points, Tags);
}

int32 FConvaiMovementPointGenerator::CountManualPoints(
	const UConvaiObjectComponent& Component)
{
	return Component.ObjectEntry.MovementPoints.Num() - CountGeneratedPoints(Component);
}

bool FConvaiMovementPointGenerator::CanEditMovementPoints(
	const UConvaiObjectComponent& Component,
	FText& OutReason)
{
	OutReason = FText::GetEmpty();
	const AActor* Owner = Component.GetOwner();
	const UWorld* World = IsValid(Owner) ? Owner->GetWorld() : nullptr;
	const UWorld* CurrentEditorWorld = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!IsValid(Owner) || Owner->IsTemplate() || !World
		|| World != CurrentEditorWorld || World->WorldType != EWorldType::Editor
		|| !Owner->GetLevel())
	{
		OutReason = LOCTEXT(
			"MovementPointsRequirePlacedEditorActor",
			"Movement points can only be generated for placed actors in the current editor level.");
		return false;
	}
	if (FLevelUtils::IsLevelLocked(Owner->GetLevel()))
	{
		OutReason = LOCTEXT(
			"MovementPointsRequireUnlockedLevel",
			"Unlock this actor's level before changing movement points.");
		return false;
	}
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
FName FConvaiMovementPointGenerator::BuildGeneratedMarkerForTesting(
	const FConvaiMovementPoint& Point)
{
	return ConvaiMovementPointGeneratorPrivate::BuildGeneratedMarker(Point);
}

int32 FConvaiMovementPointGenerator::StripGeneratedPointsForTesting(
	TArray<FConvaiMovementPoint>& Points,
	TArray<FName>& ComponentTags)
{
	return ConvaiMovementPointGeneratorPrivate::StripGeneratedPoints(Points, ComponentTags);
}

void FConvaiMovementPointGenerator::SelectCandidateIndicesForTesting(
	const TArray<FVector2D>& Directions,
	const TArray<float>& Scores,
	TArray<int32>& OutIndices)
{
	using namespace ConvaiMovementPointGeneratorPrivate;
	TArray<FGeneratedCandidate> Candidates;
	const int32 CandidateCount = FMath::Min(Directions.Num(), Scores.Num());
	Candidates.Reserve(CandidateCount);
	for (int32 CandidateIndex = 0; CandidateIndex < CandidateCount; ++CandidateIndex)
	{
		FGeneratedCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.Direction = Directions[CandidateIndex].GetSafeNormal();
		Candidate.SideAlignment = Candidate.Direction.Y;
		Candidate.Score = Scores[CandidateIndex];
		Candidate.SampleIndex = CandidateIndex;
	}
	SelectCandidateIndices(Candidates, OutIndices);
}

bool FConvaiMovementPointGenerator::IsVisibilityHitAcceptedForTesting(
	const bool bComponentScoped,
	const bool bHitOwner,
	const bool bHitTargetComponent)
{
	return ConvaiMovementPointGeneratorPrivate::IsVisibilityHitAccepted(
		bComponentScoped,
		bHitOwner,
		bHitTargetComponent);
}

float FConvaiMovementPointGenerator::ComputeSurfaceStandOffDistanceForTesting(
	const FVector& TargetExtent,
	const float AgentRadius)
{
	return ConvaiMovementPointGeneratorPrivate::ComputeSurfaceStandOffDistance(
		TargetExtent,
		AgentRadius);
}

FVector FConvaiMovementPointGenerator::ComputeFootprintSeedLocationForTesting(
	const FBox& TargetBounds,
	const FVector2D& Direction,
	const float StandOffDistance)
{
	return ConvaiMovementPointGeneratorPrivate::ComputeFootprintSeedLocation(
		TargetBounds.GetCenter(),
		TargetBounds.GetExtent(),
		Direction,
		StandOffDistance);
}

FVector2D FConvaiMovementPointGenerator::ComputeCandidateDirectionForTesting(
	const FVector& TargetCenter,
	const FVector& CandidateLocation)
{
	return ConvaiMovementPointGeneratorPrivate::ComputeCandidateDirection(
		TargetCenter,
		CandidateLocation);
}

bool FConvaiMovementPointGenerator::IsProjectedSurfaceGapAcceptedForTesting(
	const float SurfaceGap,
	const float StandOffDistance,
	const float AgentRadius)
{
	return ConvaiMovementPointGeneratorPrivate::IsProjectedSurfaceGapAccepted(
		SurfaceGap,
		StandOffDistance,
		AgentRadius);
}

void FConvaiMovementPointGenerator::BuildVisibilityTargetSamplesForTesting(
	const FBox& TargetBounds,
	const FVector& EyeLocation,
	TArray<FVector>& OutSamples)
{
	ConvaiMovementPointGeneratorPrivate::BuildVisibilityTargetSamples(
		TargetBounds,
		EyeLocation,
		OutSamples);
}

bool FConvaiMovementPointGenerator::IsVisibilityCoverageAcceptedForTesting(
	const bool bCenterVisible,
	const int32 VisibleSamples,
	const int32 TotalSamples)
{
	return ConvaiMovementPointGeneratorPrivate::IsVisibilityCoverageAccepted(
		bCenterVisible,
		VisibleSamples,
		TotalSamples);
}
#endif

#undef LOCTEXT_NAMESPACE
