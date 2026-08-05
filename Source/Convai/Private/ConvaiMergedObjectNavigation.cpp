// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiMergedObjectNavigation.h"

#include "ConvaiChatbotComponent.h"
#include "ConvaiObjectComponent.h"
#include "ConvaiSubsystem.h"
#include "ConvaiUtils.h"

namespace
{
	bool SameLogicalObject(const FConvaiObjectEntry& A, const FConvaiObjectEntry& B)
	{
		if (A.Name != B.Name || A.bIsMovementPointSubObject != B.bIsMovementPointSubObject)
		{
			return false;
		}

		return !A.bIsMovementPointSubObject
			|| (A.MovementPointSubObjectBaseName == B.MovementPointSubObjectBaseName
				&& A.MovementPointSubObjectPointName == B.MovementPointSubObjectPointName);
	}

	void RestoreLogicalIdentity(
		const FConvaiObjectEntry& LogicalEntry,
		FConvaiObjectEntry& ResolvedEntry)
	{
		ResolvedEntry.Name = LogicalEntry.Name;
		ResolvedEntry.Description = LogicalEntry.Description;
		ResolvedEntry.bIsMovementPointSubObject = LogicalEntry.bIsMovementPointSubObject;
		ResolvedEntry.MovementPointSubObjectBaseName = LogicalEntry.MovementPointSubObjectBaseName;
		ResolvedEntry.MovementPointSubObjectPointName = LogicalEntry.MovementPointSubObjectPointName;
	}

	void RebuildDeprecatedReferenceMirror(FConvaiResultAction& Action)
	{
		Action.RelatedObjectOrCharacter = FConvaiObjectEntry();
		for (const TPair<FString, FConvaiResultParam>& Pair : Action.Parameters)
		{
			if (!Pair.Value.RefValue.Name.IsEmpty())
			{
				Action.RelatedObjectOrCharacter = Pair.Value.RefValue;
				break;
			}
		}
	}
}

bool ConvaiMergedObjectNavigation::ResolveNearestReachableMember(
	AActor* SourceActor,
	const FConvaiObjectEntry& LogicalEntry,
	TConstArrayView<UConvaiObjectComponent*> RegisteredComponents,
	FConvaiObjectEntry& OutResolvedEntry)
{
	check(IsInGameThread());
	OutResolvedEntry = LogicalEntry;

	if (!IsValid(SourceActor) || LogicalEntry.Name.IsEmpty())
	{
		return false;
	}

	const FString BaseName = LogicalEntry.bIsMovementPointSubObject
		? LogicalEntry.MovementPointSubObjectBaseName
		: LogicalEntry.Name;
	if (BaseName.IsEmpty())
	{
		return false;
	}

	TArray<UConvaiObjectComponent*, TInlineAllocator<8>> Members;
	for (UConvaiObjectComponent* Component : RegisteredComponents)
	{
		if (IsValid(Component)
			&& IsValid(Component->GetOwner())
			&& Component->bMergeWithSameNamedObjects
			&& Component->ObjectEntry.Name.Equals(BaseName, ESearchCase::IgnoreCase))
		{
			Members.Add(Component);
		}
	}
	if (Members.Num() <= 1)
	{
		return false;
	}

	bool bBaseKeepsUnnamedPointsOnly = false;
	if (!LogicalEntry.bIsMovementPointSubObject)
	{
		TArray<const FConvaiObjectEntry*> MemberEntries;
		MemberEntries.Reserve(Members.Num());
		for (const UConvaiObjectComponent* Member : Members)
		{
			MemberEntries.Add(&Member->ObjectEntry);
		}

		TArray<FString> SubNames;
		FConvaiObjectEntry::CollectMovementPointSubNames(MemberEntries, SubNames);
		bBaseKeepsUnnamedPointsOnly = !SubNames.IsEmpty();
	}

	const FString PointName = FConvaiObjectEntry::NormalizeMovementPointName(
		LogicalEntry.MovementPointSubObjectPointName);
	const FString PointKey = PointName.ToLower();
	const FVector SourceLocation = SourceActor->GetActorLocation();

	bool bFoundReachable = false;
	float BestTravel = TNumericLimits<float>::Max();
	FConvaiObjectEntry BestEntry;

	for (UConvaiObjectComponent* Member : Members)
	{
		FConvaiObjectEntry Candidate = Member->ObjectEntry;
		if (!Candidate.Ref.IsValid())
		{
			Candidate.Ref = Member->GetOwner();
		}

		if (LogicalEntry.bIsMovementPointSubObject)
		{
			const bool bHasEnabledPoint = Candidate.MovementPoints.ContainsByPredicate(
				[&PointKey](const FConvaiMovementPoint& Point)
				{
					return Point.bEnabled
						&& FConvaiObjectEntry::EffectiveMovementPointName(Point).ToLower() == PointKey;
				});
			if (PointKey.IsEmpty() || !bHasEnabledPoint)
			{
				continue;
			}

			Candidate.FilterMovementPointsToName(PointName);
			Candidate.bFallbackToObjectWhenPointsUnreachable = false;
		}
		else if (bBaseKeepsUnnamedPointsOnly)
		{
			Candidate.FilterMovementPointsToObjectItself();
		}

		AActor* GoalActor = nullptr;
		USceneComponent* GoalComponent = nullptr;
		FVector GoalLocation = FVector::ZeroVector;
		float AcceptanceRadius = 0.0f;
		bool bMoveToLocation = false;
		bool bResolved = false;
		bool bAlreadyThere = false;
		bool bReachable = false;
		FVector PathEndPoint = FVector::ZeroVector;
		TArray<FVector> PathPoints;
		float TravelDistance = 0.0f;
		int32 MovementPointIndex = INDEX_NONE;
		Candidate.ResolveGoalLocation(
			SourceActor,
			GoalActor,
			GoalComponent,
			GoalLocation,
			AcceptanceRadius,
			bMoveToLocation,
			bResolved,
			bAlreadyThere,
			bReachable,
			PathEndPoint,
			PathPoints,
			TravelDistance,
			MovementPointIndex);

		// Being at a member is a complete answer even when this world has no
		// navigation data. Otherwise only a positive reachability verdict may win.
		if (!bResolved || (!bAlreadyThere && !bReachable))
		{
			continue;
		}

		const float RankedTravel = bAlreadyThere
			? 0.0f
			: ((TravelDistance > 0.0f)
				? TravelDistance
				: FVector::Dist(SourceLocation, GoalLocation));
		if (!bFoundReachable || RankedTravel < BestTravel)
		{
			bFoundReachable = true;
			BestTravel = RankedTravel;
			BestEntry = MoveTemp(Candidate);
		}
	}

	if (!bFoundReachable)
	{
		return false;
	}

	RestoreLogicalIdentity(LogicalEntry, BestEntry);
	OutResolvedEntry = MoveTemp(BestEntry);
	return true;
}

void ConvaiMergedObjectNavigation::ResolveActionObjectReferences(
	UConvaiChatbotComponent& Chatbot,
	FConvaiResultAction& Action,
	TConstArrayView<UConvaiObjectComponent*> RegisteredComponents)
{
	check(IsInGameThread());

	for (TPair<FString, FConvaiResultParam>& Pair : Action.Parameters)
	{
		FConvaiObjectEntry& ParamEntry = Pair.Value.RefValue;
		if (ParamEntry.Name.IsEmpty())
		{
			continue;
		}

		const FConvaiObjectEntry* LogicalEntry = Chatbot.EnvironmentData.Objects.FindByPredicate(
			[&ParamEntry](const FConvaiObjectEntry& EnvironmentEntry)
			{
				return SameLogicalObject(EnvironmentEntry, ParamEntry);
			});
		if (!LogicalEntry)
		{
			continue; // Character reference (or an unresolved free-form value).
		}

		FConvaiObjectEntry ResolvedEntry;
		if (ResolveNearestReachableMember(
			Chatbot.GetOwner(), *LogicalEntry, RegisteredComponents, ResolvedEntry))
		{
			ParamEntry = MoveTemp(ResolvedEntry);
		}
	}

	RebuildDeprecatedReferenceMirror(Action);
}

void ConvaiMergedObjectNavigation::ResolveActionObjectReferences(
	UConvaiChatbotComponent& Chatbot,
	FConvaiResultAction& Action)
{
	check(IsInGameThread());

	TArray<UConvaiObjectComponent*> RegisteredComponents;
	if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(&Chatbot))
	{
		RegisteredComponents = Subsystem->GetAllObjectComponents();
	}
	ResolveActionObjectReferences(Chatbot, Action, RegisteredComponents);
}
