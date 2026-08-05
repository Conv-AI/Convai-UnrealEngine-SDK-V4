// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Actions/ConvaiEscortToTask.h"
#include "Actions/ConvaiMoveToTask.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

struct FConvaiEscortToTaskTestAccessor
{
	static bool IsEscorteeNear(float Distance)
	{
		return UConvaiEscortToTask::IsEscorteeNear(Distance);
	}

	static bool IsLocationClearlyAheadOnPath(
		const TArray<FVector>& PathPoints,
		const FVector& GuideLocation,
		const FVector& EscorteeLocation)
	{
		return UConvaiEscortToTask::IsLocationClearlyAheadOnPath(
			PathPoints, GuideLocation, EscorteeLocation);
	}

	static EConvaiEscortResultCode MapMoveSuccessCode(
		UConvaiEscortToTask& Task,
		const FConvaiMoveToResult& Result)
	{
		return Task.MapMoveSuccessCode(Result);
	}

	static void SetReachedAfterTravel(
		UConvaiEscortToTask& Task,
		bool bReachedAfterTravel)
	{
		Task.bReachedDestinationAfterTravel = bReachedAfterTravel;
	}
};

struct FConvaiMoveToProxyTestAccessor
{
	static void QueueResult(
		UConvaiMoveToProxy& Proxy,
		const FConvaiMoveToResult& Result)
	{
		Proxy.QueueProxyResult(Result);
	}

	static void FinishResult(
		UConvaiMoveToProxy& Proxy,
		const FConvaiMoveToResult& Result)
	{
		Proxy.FinishProxy(Result);
	}

	static bool IsNaturalCompletionPending(
		const UConvaiMoveToProxy& Proxy)
	{
		return Proxy.bNaturalCompletionPending;
	}

	static bool IsTerminal(const UConvaiMoveToProxy& Proxy)
	{
		return Proxy.bTerminal;
	}
};

struct FConvaiEscortProxyTestAccessor
{
	static void QueueResult(
		UConvaiEscortProxy& Proxy,
		const FConvaiEscortResult& Result)
	{
		Proxy.QueueProxyResult(Result);
	}

	static void FinishResult(
		UConvaiEscortProxy& Proxy,
		const FConvaiEscortResult& Result)
	{
		Proxy.FinishProxy(Result);
	}

	static bool IsNaturalCompletionPending(
		const UConvaiEscortProxy& Proxy)
	{
		return Proxy.bNaturalCompletionPending;
	}

	static bool IsTerminal(const UConvaiEscortProxy& Proxy)
	{
		return Proxy.bTerminal;
	}
};

namespace
{
	struct FScopedMoveToTestWorld
	{
		FScopedMoveToTestWorld()
		{
			const FName WorldName = MakeUniqueObjectName(
				GetTransientPackage(),
				UWorld::StaticClass(),
				TEXT("ConvaiMoveToTestWorld"));
			World = UWorld::CreateWorld(
				EWorldType::Editor,
				false,
				WorldName,
				GetTransientPackage(),
				true);
		}

		~FScopedMoveToTestWorld()
		{
			if (World)
			{
				World->DestroyWorld(false);
				World->MarkObjectsPendingKill();
			}
		}

		template <typename TActor>
		TActor* SpawnActorAt(const FVector& Location) const
		{
			if (!World)
			{
				return nullptr;
			}

			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags = RF_Transient;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			TActor* Actor = World->SpawnActor<TActor>(SpawnParameters);
			if (!Actor)
			{
				return nullptr;
			}

			USceneComponent* Root = NewObject<USceneComponent>(
				Actor, TEXT("MoveToTestRoot"), RF_Transient);
			Actor->SetRootComponent(Root);
			Root->RegisterComponent();
			Root->SetWorldLocation(Location);
			return Actor;
		}

		USceneComponent* AddChildComponent(
			AActor& Actor,
			const FName Name,
			const FVector& RelativeLocation) const
		{
			USceneComponent* Component =
				NewObject<USceneComponent>(&Actor, Name, RF_Transient);
			Actor.AddInstanceComponent(Component);
			Component->SetupAttachment(Actor.GetRootComponent());
			Component->RegisterComponent();
			Component->SetRelativeLocation(RelativeLocation);
			return Component;
		}

		UWorld* World = nullptr;
	};

	struct FResolvedGoal
	{
		AActor* Actor = nullptr;
		USceneComponent* Component = nullptr;
		FVector Location = FVector::ZeroVector;
		float AcceptanceRadius = 0.0f;
		bool bMoveToLocation = false;
		bool bSuccess = false;
		bool bAlreadyThere = false;
		bool bReachable = false;
		FVector PathEndPoint = FVector::ZeroVector;
		TArray<FVector> PathPoints;
		float TravelDistance = 0.0f;
		int32 MovementPointIndex = INDEX_NONE;
	};

	FResolvedGoal ResolveWithoutNavigation(FConvaiObjectEntry& Entry)
	{
		FResolvedGoal Result;
		Entry.ResolveGoalLocation(
			/*SourceActor*/ nullptr,
			Result.Actor,
			Result.Component,
			Result.Location,
			Result.AcceptanceRadius,
			Result.bMoveToLocation,
			Result.bSuccess,
			Result.bAlreadyThere,
			Result.bReachable,
			Result.PathEndPoint,
			Result.PathPoints,
			Result.TravelDistance,
			Result.MovementPointIndex);
		return Result;
	}

	FConvaiObjectEntry MakeWholeActorEntry(
		AActor* Actor,
		const TCHAR* Name)
	{
		FConvaiObjectEntry Entry;
		Entry.Name = Name;
		Entry.Ref = Actor;
		Entry.ObjectReference = EConvaiObjectReference::WholeActor;
		return Entry;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMoveToResultContractTest,
	"Convai.Actions.MoveTo.ResultContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiMoveToResultContractTest::RunTest(const FString& Parameters)
{
	struct FExpectedResultSemantics
	{
		EConvaiMoveToResultCode Code;
		bool bSuccess;
		bool bReportToCharacter;
		const TCHAR* Label;
	};

	const FExpectedResultSemantics Cases[] = {
		{ EConvaiMoveToResultCode::Reached, true, false, TEXT("Reached") },
		{ EConvaiMoveToResultCode::AlreadyAtDestination, true, false,
			TEXT("Already at destination") },
		{ EConvaiMoveToResultCode::UnknownDestination, false, true,
			TEXT("Unknown destination") },
		{ EConvaiMoveToResultCode::Unreachable, false, true,
			TEXT("Unreachable") },
		{ EConvaiMoveToResultCode::InvalidCharacter, false, false,
			TEXT("Invalid character") },
		{ EConvaiMoveToResultCode::MissingController, false, false,
			TEXT("Missing controller") },
		{ EConvaiMoveToResultCode::MissingMovementComponent, false, false,
			TEXT("Missing movement component") },
		{ EConvaiMoveToResultCode::MissingPathFollowingComponent, false, false,
			TEXT("Missing path-following component") },
		{ EConvaiMoveToResultCode::MissingNavigationData, false, false,
			TEXT("Missing navigation data") },
		{ EConvaiMoveToResultCode::MoveFailed, false, true,
			TEXT("Move failed") },
		{ EConvaiMoveToResultCode::Cancelled, false, false,
			TEXT("Cancelled") }
	};

	for (const FExpectedResultSemantics& Case : Cases)
	{
		FConvaiMoveToResult Result;
		Result.Code = Case.Code;
		TestTrue(
			FString::Printf(TEXT("%s success contract"), Case.Label),
			Result.IsSuccess() == Case.bSuccess);
		TestTrue(
			FString::Printf(TEXT("%s reporting contract"), Case.Label),
			Result.ShouldReportToCharacter() == Case.bReportToCharacter);
	}

	FConvaiEscortResult ReachedEscort;
	ReachedEscort.Code = EConvaiEscortResultCode::Reached;
	TestTrue(TEXT("Reached is a successful Escort result"),
		ReachedEscort.IsSuccess());

	FConvaiEscortResult AlreadyAtEscort;
	AlreadyAtEscort.Code = EConvaiEscortResultCode::AlreadyAtDestination;
	TestTrue(TEXT("Already-at is a successful Escort result"),
		AlreadyAtEscort.IsSuccess());

	FConvaiMoveToResult ReachedMove;
	ReachedMove.Code = EConvaiMoveToResultCode::Reached;
	UConvaiEscortToTask* EscortTask = NewObject<UConvaiEscortToTask>();
	TestNotNull(TEXT("An Escort task is available for result mapping"), EscortTask);
	if (!EscortTask)
	{
		return false;
	}
	TestTrue(TEXT("Escort preserves a travelled Move To result as Reached"),
		FConvaiEscortToTaskTestAccessor::MapMoveSuccessCode(
			*EscortTask, ReachedMove) ==
			EConvaiEscortResultCode::Reached);

	FConvaiMoveToResult AlreadyAtMove;
	AlreadyAtMove.Code = EConvaiMoveToResultCode::AlreadyAtDestination;
	TestTrue(TEXT("Escort preserves an already-arrived Move To result"),
		FConvaiEscortToTaskTestAccessor::MapMoveSuccessCode(
			*EscortTask, AlreadyAtMove) ==
			EConvaiEscortResultCode::AlreadyAtDestination);
	FConvaiEscortToTaskTestAccessor::SetReachedAfterTravel(*EscortTask, true);
	TestTrue(TEXT("A prior real move dominates a catch-up recheck at the destination"),
		FConvaiEscortToTaskTestAccessor::MapMoveSuccessCode(
			*EscortTask, AlreadyAtMove) ==
			EConvaiEscortResultCode::Reached);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMovementProxyCancellationRaceTest,
	"Convai.Actions.MovementProxy.CancellationRace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiMovementProxyCancellationRaceTest::RunTest(
	const FString& Parameters)
{
	FConvaiMoveToResult MoveSuccess;
	MoveSuccess.Code = EConvaiMoveToResultCode::AlreadyAtDestination;
	MoveSuccess.AdditionalNote = TEXT("Already at the destination.");

	UConvaiMoveToProxy* Move = NewObject<UConvaiMoveToProxy>();
	TestNotNull(TEXT("A Move To proxy can be created"), Move);
	if (Move)
	{
		TestTrue(
			TEXT("The first Move To cancellation claims the terminal path"),
			Move->Cancel());
		TestTrue(
			TEXT("A claimed Move To cancellation marks the proxy terminal"),
			FConvaiMoveToProxyTestAccessor::IsTerminal(*Move));
		TestFalse(
			TEXT("A repeated Move To cancellation cannot acknowledge twice"),
			Move->Cancel());
		FConvaiMoveToProxyTestAccessor::QueueResult(*Move, MoveSuccess);
		TestFalse(
			TEXT("A result queued after Move To cancellation is suppressed"),
			FConvaiMoveToProxyTestAccessor::IsNaturalCompletionPending(*Move));
		FConvaiMoveToProxyTestAccessor::FinishResult(*Move, MoveSuccess);
		TestTrue(
			TEXT("A stale Move To completion cannot replace cancellation"),
			FConvaiMoveToProxyTestAccessor::IsTerminal(*Move));
	}

	UConvaiMoveToProxy* PendingMove = NewObject<UConvaiMoveToProxy>();
	TestNotNull(TEXT("A pending Move To proxy can be created"), PendingMove);
	if (PendingMove)
	{
		FConvaiMoveToProxyTestAccessor::QueueResult(
			*PendingMove, MoveSuccess);
		TestTrue(
			TEXT("Queueing Move To completion reserves the terminal path"),
			FConvaiMoveToProxyTestAccessor::IsNaturalCompletionPending(
				*PendingMove));
		TestFalse(
			TEXT("Queued Move To completion wins over a late cancellation"),
			PendingMove->Cancel());
		TestFalse(
			TEXT("Losing cancellation does not steal Move To's terminal path"),
			FConvaiMoveToProxyTestAccessor::IsTerminal(*PendingMove));
		FConvaiMoveToProxyTestAccessor::FinishResult(
			*PendingMove, MoveSuccess);
		TestFalse(
			TEXT("Delivered Move To completion clears its pending marker"),
			FConvaiMoveToProxyTestAccessor::IsNaturalCompletionPending(
				*PendingMove));
		TestTrue(
			TEXT("Delivered Move To completion owns the terminal path"),
			FConvaiMoveToProxyTestAccessor::IsTerminal(*PendingMove));
		TestFalse(
			TEXT("Move To cannot be cancelled after natural completion"),
			PendingMove->Cancel());
	}

	FConvaiEscortResult EscortSuccess;
	EscortSuccess.Code = EConvaiEscortResultCode::Reached;
	EscortSuccess.AdditionalNote = TEXT("Escort reached the destination.");

	UConvaiEscortProxy* Escort = NewObject<UConvaiEscortProxy>();
	TestNotNull(TEXT("An Escort proxy can be created"), Escort);
	if (Escort)
	{
		TestTrue(
			TEXT("The first Escort cancellation claims the terminal path"),
			Escort->Cancel());
		TestTrue(
			TEXT("A claimed Escort cancellation marks the proxy terminal"),
			FConvaiEscortProxyTestAccessor::IsTerminal(*Escort));
		TestFalse(
			TEXT("A repeated Escort cancellation cannot acknowledge twice"),
			Escort->Cancel());
		FConvaiEscortProxyTestAccessor::QueueResult(*Escort, EscortSuccess);
		TestFalse(
			TEXT("A result queued after Escort cancellation is suppressed"),
			FConvaiEscortProxyTestAccessor::IsNaturalCompletionPending(
				*Escort));
		FConvaiEscortProxyTestAccessor::FinishResult(*Escort, EscortSuccess);
		TestTrue(
			TEXT("A stale Escort completion cannot replace cancellation"),
			FConvaiEscortProxyTestAccessor::IsTerminal(*Escort));
	}

	UConvaiEscortProxy* PendingEscort = NewObject<UConvaiEscortProxy>();
	TestNotNull(TEXT("A pending Escort proxy can be created"), PendingEscort);
	if (PendingEscort)
	{
		FConvaiEscortProxyTestAccessor::QueueResult(
			*PendingEscort, EscortSuccess);
		TestTrue(
			TEXT("Queueing Escort completion reserves the terminal path"),
			FConvaiEscortProxyTestAccessor::IsNaturalCompletionPending(
				*PendingEscort));
		TestFalse(
			TEXT("Queued Escort completion wins over a late cancellation"),
			PendingEscort->Cancel());
		TestFalse(
			TEXT("Losing cancellation does not steal Escort's terminal path"),
			FConvaiEscortProxyTestAccessor::IsTerminal(*PendingEscort));
		FConvaiEscortProxyTestAccessor::FinishResult(
			*PendingEscort, EscortSuccess);
		TestFalse(
			TEXT("Delivered Escort completion clears its pending marker"),
			FConvaiEscortProxyTestAccessor::IsNaturalCompletionPending(
				*PendingEscort));
		TestTrue(
			TEXT("Delivered Escort completion owns the terminal path"),
			FConvaiEscortProxyTestAccessor::IsTerminal(*PendingEscort));
		TestFalse(
			TEXT("Escort cannot be cancelled after natural completion"),
			PendingEscort->Cancel());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMovementProxyTeardownTest,
	"Convai.Actions.MovementProxy.Teardown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiMovementProxyTeardownTest::RunTest(
	const FString& Parameters)
{
	UConvaiMoveToProxy* Move = NewObject<UConvaiMoveToProxy>();
	UConvaiEscortProxy* Escort = NewObject<UConvaiEscortProxy>();
	if (!TestNotNull(TEXT("A Move To proxy can be created for teardown"), Move) ||
		!TestNotNull(TEXT("An Escort proxy can be created for teardown"), Escort))
	{
		return false;
	}

	// The concrete proxy must detach before the shared base seals lifecycle
	// state. This exact destruction path previously dispatched a pure virtual.
	TestTrue(TEXT("Move To proxy begins destruction without virtual teardown dispatch"),
		Move->ConditionalBeginDestroy());
	TestTrue(TEXT("Escort proxy begins destruction without virtual teardown dispatch"),
		Escort->ConditionalBeginDestroy());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMoveToImmediateResultTest,
	"Convai.Actions.MoveTo.ImmediateResults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FConvaiMoveToImmediateResultTest::RunTest(const FString& Parameters)
{
	FScopedMoveToTestWorld Scene;
	if (!TestNotNull(TEXT("A disposable world is available"), Scene.World))
	{
		return false;
	}

	AActor* NonPawn = Scene.SpawnActorAt<AActor>(FVector::ZeroVector);
	APawn* Pawn = Scene.SpawnActorAt<APawn>(FVector::ZeroVector);
	AActor* Target = Scene.SpawnActorAt<AActor>(FVector::ZeroVector);
	if (!TestNotNull(TEXT("The non-pawn source exists"), NonPawn) ||
		!TestNotNull(TEXT("The pawn source exists"), Pawn) ||
		!TestNotNull(TEXT("The target exists"), Target))
	{
		return false;
	}

	FConvaiMoveToResult ImmediateResult;
	FConvaiObjectEntry NearbyEntry =
		MakeWholeActorEntry(Target, TEXT("Nearby exhibit"));
	UConvaiMoveToTask* Task = UConvaiMoveToTask::CreateMoveToTask(
		NonPawn, NearbyEntry, ImmediateResult);
	TestNull(TEXT("An invalid moving actor does not create a task"), Task);
	TestTrue(
		TEXT("A non-pawn source has the explicit invalid-character result"),
		ImmediateResult.Code == EConvaiMoveToResultCode::InvalidCharacter);
	TestEqual(
		TEXT("The invalid-character note stays player-safe"),
		ImmediateResult.AdditionalNote,
		FString(TEXT("You cannot move right now.")));
	TestTrue(
		TEXT("The invalid-character developer details identify the setup issue"),
		ImmediateResult.DeveloperDetails.Contains(TEXT("Pawn")));
	TestFalse(
		TEXT("Technical Pawn terminology does not leak into the player note"),
		ImmediateResult.AdditionalNote.Contains(TEXT("Pawn")));

	FConvaiObjectEntry MissingEntry;
	MissingEntry.Name = TEXT("Missing exhibit");
	Task = UConvaiMoveToTask::CreateMoveToTask(
		Pawn, MissingEntry, ImmediateResult);
	TestNull(TEXT("An unconfigured pawn does not create a task"), Task);
	TestTrue(
		TEXT("Movement setup is validated before destination resolution"),
		ImmediateResult.Code == EConvaiMoveToResultCode::MissingController);
	TestEqual(
		TEXT("The setup-failure note stays player-safe"),
		ImmediateResult.AdditionalNote,
		FString(TEXT("You cannot move right now.")));
	TestTrue(
		TEXT("The setup diagnostic identifies the missing controller"),
		ImmediateResult.DeveloperDetails.Contains(TEXT("AI Controller")));

	Task = UConvaiMoveToTask::CreateMoveToTask(
		Pawn, NearbyEntry, ImmediateResult);
	TestNull(TEXT("A nearby goal does not hide invalid movement setup"), Task);
	TestTrue(
		TEXT("Strict setup validation wins even when the goal is already nearby"),
		ImmediateResult.Code == EConvaiMoveToResultCode::MissingController);
	TestEqual(
		TEXT("The nearby setup failure remains player-safe"),
		ImmediateResult.AdditionalNote,
		FString(TEXT("You cannot move right now.")));
	TestTrue(
		TEXT("The nearby setup failure remains actionable in logs"),
		ImmediateResult.DeveloperDetails.Contains(TEXT("AI Controller")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMoveToLiveGoalRefreshTest,
	"Convai.Actions.MoveTo.LiveGoalRefresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FConvaiMoveToLiveGoalRefreshTest::RunTest(const FString& Parameters)
{
	FScopedMoveToTestWorld Scene;
	if (!TestNotNull(TEXT("A disposable world is available"), Scene.World))
	{
		return false;
	}

	AActor* Target =
		Scene.SpawnActorAt<AActor>(FVector(100.0f, 200.0f, 30.0f));
	if (!TestNotNull(TEXT("The moving target exists"), Target))
	{
		return false;
	}

	FConvaiObjectEntry WholeActor =
		MakeWholeActorEntry(Target, TEXT("Moving exhibit"));
	const FResolvedGoal WholeGoal = ResolveWithoutNavigation(WholeActor);
	TestTrue(TEXT("A whole-actor goal resolves"), WholeGoal.bSuccess);
	TestFalse(
		TEXT("A whole-actor goal uses Unreal actor tracking"),
		WholeGoal.bMoveToLocation);
	TestTrue(
		TEXT("The resolved actor is retained"),
		WholeGoal.Actor == Target);

	const FVector MovedActorLocation(450.0f, -125.0f, 80.0f);
	Target->GetRootComponent()->SetWorldLocation(MovedActorLocation);
	const FResolvedGoal RefreshedWholeGoal =
		ResolveWithoutNavigation(WholeActor);
	TestEqual(
		TEXT("Re-resolving a whole-actor goal uses its current location"),
		RefreshedWholeGoal.Location,
		MovedActorLocation);

	USceneComponent* MovingComponent = Scene.AddChildComponent(
		*Target,
		TEXT("MovingDisplay"),
		FVector(25.0f, 40.0f, 10.0f));
	if (!TestNotNull(TEXT("The moving component exists"), MovingComponent))
	{
		return false;
	}

	FConvaiObjectEntry ComponentEntry;
	ComponentEntry.Name = TEXT("Moving display");
	ComponentEntry.Ref = Target;
	ComponentEntry.ObjectReference =
		EConvaiObjectReference::SpecificComponent;
	ComponentEntry.ComponentName = MovingComponent->GetName();
	const FResolvedGoal ComponentGoal =
		ResolveWithoutNavigation(ComponentEntry);
	TestTrue(TEXT("A component goal resolves"), ComponentGoal.bSuccess);
	TestTrue(
		TEXT("A component goal uses a tracked location"),
		ComponentGoal.bMoveToLocation);
	TestTrue(
		TEXT("The intended component is retained"),
		ComponentGoal.Component == MovingComponent);

	MovingComponent->SetRelativeLocation(FVector(-75.0f, 90.0f, 35.0f));
	const FResolvedGoal RefreshedComponentGoal =
		ResolveWithoutNavigation(ComponentEntry);
	TestEqual(
		TEXT("Re-resolving a component goal uses its current location"),
		RefreshedComponentGoal.Location,
		MovingComponent->GetComponentLocation());

	FConvaiObjectEntry MovementPointEntry = ComponentEntry;
	FConvaiMovementPoint RelativePoint;
	RelativePoint.bEnabled = true;
	RelativePoint.Attachment =
		EConvaiMovementPointAttachment::RelativeToObject;
	RelativePoint.Transform.SetLocation(FVector(60.0f, 0.0f, 5.0f));
	MovementPointEntry.MovementPoints.Add(RelativePoint);
	const FResolvedGoal MovementPointGoal =
		ResolveWithoutNavigation(MovementPointEntry);
	TestTrue(
		TEXT("A relative movement point resolves"),
		MovementPointGoal.bSuccess);
	TestTrue(
		TEXT("A relative movement point uses a tracked location"),
		MovementPointGoal.bMoveToLocation);
	TestEqual(
		TEXT("The selected movement-point index is retained"),
		MovementPointGoal.MovementPointIndex,
		0);

	MovingComponent->SetRelativeLocation(FVector(125.0f, -30.0f, 45.0f));
	const FVector ExpectedPointLocation =
		MovingComponent->GetComponentTransform().TransformPosition(
			RelativePoint.Transform.GetLocation());
	const FResolvedGoal RefreshedMovementPointGoal =
		ResolveWithoutNavigation(MovementPointEntry);
	TestEqual(
		TEXT("Re-resolving a relative point keeps its authored offset"),
		RefreshedMovementPointGoal.Location,
		ExpectedPointLocation);

	MovementPointEntry.MovementPoints[0].bEnabled = false;
	const FResolvedGoal DisabledMovementPointGoal =
		ResolveWithoutNavigation(MovementPointEntry);
	TestEqual(
		TEXT("A disabled movement point is no longer selected"),
		DisabledMovementPointGoal.MovementPointIndex,
		INDEX_NONE);
	TestEqual(
		TEXT("A disabled movement point falls back to the component"),
		DisabledMovementPointGoal.Location,
		MovingComponent->GetComponentLocation());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiEscortPathAheadPolicyTest,
	"Convai.Actions.EscortTo.PathAheadPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiEscortPathAheadPolicyTest::RunTest(const FString& Parameters)
{
	auto IsAhead = [](const TArray<FVector>& PathPoints,
		const FVector& Guide, const FVector& Escortee)
	{
		return FConvaiEscortToTaskTestAccessor::IsLocationClearlyAheadOnPath(
			PathPoints, Guide, Escortee);
	};

	const TArray<FVector> StraightPath = {
		FVector(0.0f, 0.0f, 0.0f),
		FVector(1000.0f, 0.0f, 0.0f)
	};
	const FVector GuideLocation(200.0f, 0.0f, 0.0f);

	TestTrue(
		TEXT("An escortee clearly ahead on the remaining route does not pause Escort"),
		FConvaiEscortToTaskTestAccessor::IsLocationClearlyAheadOnPath(
			StraightPath,
			GuideLocation,
			FVector(500.0f, 0.0f, 0.0f)));
	TestFalse(
		TEXT("An escortee behind the guide on a long segment is not ahead"),
		FConvaiEscortToTaskTestAccessor::IsLocationClearlyAheadOnPath(
			StraightPath,
			GuideLocation,
			FVector(100.0f, 0.0f, 0.0f)));
	TestFalse(
		TEXT("Small forward jitter does not count as clearly ahead"),
		FConvaiEscortToTaskTestAccessor::IsLocationClearlyAheadOnPath(
			StraightPath,
			GuideLocation,
			FVector(250.0f, 0.0f, 0.0f)));
	TestFalse(
		TEXT("A laterally distant escortee is outside the route corridor"),
		FConvaiEscortToTaskTestAccessor::IsLocationClearlyAheadOnPath(
			StraightPath,
			GuideLocation,
			FVector(500.0f, 251.0f, 0.0f)));
	TestFalse(
		TEXT("A vertically distant escortee is outside the route corridor"),
		FConvaiEscortToTaskTestAccessor::IsLocationClearlyAheadOnPath(
			StraightPath,
			GuideLocation,
			FVector(500.0f, 0.0f, 151.0f)));

	const TArray<FVector> PathWithDuplicatePoint = {
		FVector(0.0f, 0.0f, 0.0f),
		FVector(0.0f, 0.0f, 0.0f),
		FVector(1000.0f, 0.0f, 0.0f)
	};
	TestTrue(
		TEXT("A duplicate path point does not hide genuine forward progress"),
		FConvaiEscortToTaskTestAccessor::IsLocationClearlyAheadOnPath(
			PathWithDuplicatePoint,
			GuideLocation,
			FVector(500.0f, 0.0f, 0.0f)));

	const TArray<FVector> LoopingPath = {
		FVector(0.0f, 0.0f, 0.0f),
		FVector(1000.0f, 0.0f, 0.0f),
		FVector(1000.0f, 1000.0f, 0.0f),
		FVector(0.0f, 1000.0f, 0.0f),
		FVector(0.0f, 0.0f, 0.0f)
	};
	TestFalse(
		TEXT("An ambiguous projection at a looping route fails closed"),
		FConvaiEscortToTaskTestAccessor::IsLocationClearlyAheadOnPath(
			LoopingPath,
			FVector(100.0f, 0.0f, 0.0f),
			FVector(50.0f, 0.0f, 0.0f)));

	const TArray<FVector> IncompletePath = { FVector::ZeroVector };
	TestFalse(
		TEXT("An incomplete route cannot classify an escortee as ahead"),
		IsAhead(
			IncompletePath,
			FVector::ZeroVector,
			FVector(500.0f, 0.0f, 0.0f)));

	const TArray<FVector> LongPath = {
		FVector(0.0f, 0.0f, 0.0f),
		FVector(2000.0f, 0.0f, 0.0f)
	};
	TestTrue(TEXT("A distant escortee ahead inside the route corridor keeps moving"),
		IsAhead(LongPath, FVector::ZeroVector, FVector(1200.0f, 100.0f, 0.0f)));
	TestFalse(TEXT("Being ahead cannot satisfy the strict arrival distance"),
		FConvaiEscortToTaskTestAccessor::IsEscorteeNear(1200.0f));
	TestTrue(TEXT("The horizontal route-corridor boundary still qualifies as ahead"),
		IsAhead(LongPath, FVector::ZeroVector, FVector(1000.0f, 250.0f, 0.0f)));
	TestTrue(TEXT("The vertical route-corridor boundary still qualifies as ahead"),
		IsAhead(LongPath, FVector::ZeroVector, FVector(1000.0f, 0.0f, 150.0f)));

	const TArray<FVector> SparseLongSegment = {
		FVector(-1000.0f, 0.0f, 0.0f),
		FVector(2000.0f, 0.0f, 0.0f)
	};
	TestFalse(TEXT("A distant escortee behind on a sparse segment is not ahead"),
		IsAhead(SparseLongSegment, FVector::ZeroVector,
			FVector(-800.0f, 0.0f, 0.0f)));
	TestFalse(TEXT("An escortee behind the remaining path start is not ahead"),
		IsAhead(LongPath, FVector::ZeroVector, FVector(-800.0f, 0.0f, 0.0f)));

	const TArray<FVector> CornerPath = {
		FVector(0.0f, 0.0f, 0.0f),
		FVector(1000.0f, 0.0f, 0.0f),
		FVector(1000.0f, 1200.0f, 0.0f)
	};
	TestTrue(TEXT("An escortee ahead after a route corner keeps moving"),
		IsAhead(CornerPath, FVector::ZeroVector,
			FVector(1000.0f, 600.0f, 0.0f)));
	TestTrue(TEXT("An adjacent-segment corner tie is a valid ahead projection"),
		IsAhead(CornerPath, FVector::ZeroVector,
			FVector(800.0f, 200.0f, 0.0f)));
	TestTrue(TEXT("A small overshoot beyond the route endpoint remains ahead"),
		IsAhead(
			{ FVector::ZeroVector, FVector(1000.0f, 0.0f, 0.0f) },
			FVector::ZeroVector,
			FVector(1100.0f, 0.0f, 0.0f)));

	const TArray<FVector> CrossingPath = {
		FVector(-1000.0f, -1000.0f, 0.0f),
		FVector(1000.0f, 1000.0f, 0.0f),
		FVector(-1000.0f, 1000.0f, 0.0f),
		FVector(1000.0f, -1000.0f, 0.0f)
	};
	TestFalse(TEXT("A non-adjacent self-crossing projection fails closed"),
		IsAhead(CrossingPath, CrossingPath[0], FVector::ZeroVector));
	TestFalse(TEXT("A missing route preserves distance-only lag behavior"),
		IsAhead({}, FVector::ZeroVector, FVector(1000.0f, 0.0f, 0.0f)));
	TestFalse(TEXT("A degenerate route preserves distance-only lag behavior"),
		IsAhead(
			{ FVector::ZeroVector, FVector::ZeroVector },
			FVector::ZeroVector,
			FVector(1000.0f, 0.0f, 0.0f)));
	TestFalse(TEXT("A non-finite route preserves distance-only lag behavior"),
		IsAhead(
			{ FVector::ZeroVector,
				FVector(std::numeric_limits<FVector::FReal>::quiet_NaN(),
					0.0f, 0.0f) },
			FVector::ZeroVector,
			FVector(1000.0f, 0.0f, 0.0f)));
	TestFalse(TEXT("A stale route far from the guide fails closed"),
		IsAhead(LongPath, FVector(0.0f, 1000.0f, 0.0f),
			FVector(1000.0f, 0.0f, 0.0f)));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
