// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Actions/ConvaiMoveToTask.h"

#include "AIController.h"
#include "AIResources.h"
#include "AITypes.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavMesh/NavMeshPath.h"
#include "NavigationSystem.h"
#include "Tasks/AITask.h"
#include "Utility/Log/ConvaiLogger.h"

void UConvaiMoveOwnedAITask::OnRequestFinished(
	FAIRequestID RequestID,
	const FPathFollowingResult& Result)
{
	const bool bReplacedByAnotherRequest =
		RequestID == MoveRequestID &&
		Result.HasFlag(FPathFollowingResultFlags::UserAbort) &&
		Result.HasFlag(FPathFollowingResultFlags::NewRequest) &&
		!Result.HasFlag(FPathFollowingResultFlags::ForcedScript);
	if (bReplacedByAnotherRequest)
	{
		// UAITask_MoveTo intentionally ignores this result because most callers
		// lock AI logic. Convai Move To also supports an unlocked public mode,
		// where ignoring preemption would leave the request active forever.
		MoveRequestID = FAIRequestID::InvalidRequest;
		FinishMoveTask(Result.Code);
		return;
	}

	Super::OnRequestFinished(RequestID, Result);
}

void UConvaiMoveOwnedAITask::OnDestroy(bool bInOwnerFinished)
{
	const EPathFollowingResult::Type Result = GetMoveResult();

	// During a GC purge or world teardown the controller and its path-following
	// component can be partially destroyed while this task is being cancelled;
	// UAITask_MoveTo::OnDestroy would then call AbortMove into freed component
	// state. Dropping the request ID makes the engine skip the abort.
	const bool bControllerObjectUsable =
		IsValid(OwnerController) && !OwnerController->IsUnreachable();
	UWorld* OwnerWorld = bControllerObjectUsable
		? OwnerController->GetWorld() : nullptr;
	const bool bControllerUsable = bControllerObjectUsable &&
		IsValid(OwnerWorld) && !OwnerWorld->IsUnreachable();
	UPathFollowingComponent* PathFollowing = bControllerUsable
		? OwnerController->GetPathFollowingComponent() : nullptr;
	const bool bPathFollowingUnusable =
		PathFollowing &&
		(!IsValid(PathFollowing) || PathFollowing->IsUnreachable());
	if (!bControllerUsable || bPathFollowingUnusable)
	{
		MoveRequestID = FAIRequestID::InvalidRequest;
		// ResetObservers only null-checks this pointer before following it to
		// the path component through UE 5.8; older ResetTimers versions do the
		// same. Clear it so base cleanup cannot enter unreachable object state.
		OwnerController = nullptr;
	}
	else if (!PathFollowing)
	{
		MoveRequestID = FAIRequestID::InvalidRequest;
	}

	Super::OnDestroy(bInOwnerFinished);

	OnOwnedMoveFinished.Broadcast(
		Result == EPathFollowingResult::Success,
		static_cast<int32>(Result));
	OnOwnedMoveFinished.Clear();
}

bool UConvaiMoveOwnedAITask::SuspendOwnedRequest()
{
	if (!OwnerController || !MoveRequestID.IsValid())
	{
		return false;
	}

	if (!OwnerController->PauseMove(MoveRequestID))
	{
		return false;
	}

	// Keep the task active so it retains its movement and AI-logic resources.
	ResetTimers();
	return true;
}

bool UConvaiMoveOwnedAITask::RefreshOwnedRequest(
	const FAIMoveRequest& InMoveRequest)
{
	if (!OwnerController || IsFinished())
	{
		return false;
	}

	MoveRequest = InMoveRequest;
	if (!IsActive())
	{
		// Resource arbitration has not activated this task yet. Staging the
		// latest request is enough; Activate will issue it after ownership.
		return true;
	}

	ResetTimers();
	ConditionalPerformMove();
	return !IsFinished();
}

bool UConvaiMoveOwnedAITask::CopyRemainingPathPoints(
	TArray<FVector>& OutPathPoints) const
{
	OutPathPoints.Reset();
	if (!OwnerController || !MoveRequestID.IsValid() || !Path.IsValid())
	{
		return false;
	}

	const UPathFollowingComponent* PathFollowing =
		OwnerController->GetPathFollowingComponent();
	if (!PathFollowing || !PathFollowing->HasValidPath() ||
		PathFollowing->GetCurrentRequestId() != MoveRequestID ||
		PathFollowing->GetPath() != Path ||
		PathFollowing->IsCurrentSegmentNavigationLink())
	{
		return false;
	}

	// Crowd following can expose a polygon corridor rather than an ordered,
	// string-pulled polyline. That is unsafe for route-progress checks.
	if (const FNavMeshPath* NavMeshPath = Path->CastPath<FNavMeshPath>();
		NavMeshPath && !NavMeshPath->IsStringPulled())
	{
		return false;
	}

	const TArray<FNavPathPoint>& PathPoints = Path->GetPathPoints();
	if (PathPoints.Num() < 2)
	{
		return false;
	}

	const uint32 CurrentPathIndex = PathFollowing->GetCurrentPathIndex();
	const uint32 NextPathIndex = PathFollowing->GetNextPathIndex();
	if (CurrentPathIndex >= static_cast<uint32>(PathPoints.Num()) ||
		NextPathIndex >= static_cast<uint32>(PathPoints.Num()) ||
		NextPathIndex <= CurrentPathIndex)
	{
		return false;
	}

	const int32 CurrentPointIndex = static_cast<int32>(CurrentPathIndex);
	const int32 NextPointIndex = static_cast<int32>(NextPathIndex);
	OutPathPoints.Reserve(PathPoints.Num() - CurrentPointIndex);
	OutPathPoints.Add(*Path->GetPathPointLocation(CurrentPathIndex));
	OutPathPoints.Add(PathFollowing->GetCurrentTargetLocation());

	if (FNavMeshNodeFlags(PathPoints[NextPointIndex].Flags).IsNavLink())
	{
		return true;
	}

	for (int32 PointIndex = NextPointIndex + 1;
		PointIndex < PathPoints.Num(); ++PointIndex)
	{
		OutPathPoints.Add(*Path->GetPathPointLocation(PointIndex));
		if (FNavMeshNodeFlags(PathPoints[PointIndex].Flags).IsNavLink())
		{
			break;
		}
	}
	return OutPathPoints.Num() >= 2;
}

namespace
{
	constexpr float EngineAcceptanceRadiusScale = 0.25f;
	constexpr float LocationRefreshIntervalSeconds = 0.35f;
	constexpr float LocationRefreshDistance = 50.0f;
	constexpr int32 MaxReachableMoveRetries = 1;

	FString EntryLabel(const FConvaiObjectEntry& Entry)
	{
		const FString Trimmed = Entry.Name.TrimStartAndEnd();
		return Trimmed.IsEmpty() ? TEXT("the destination") : Trimmed;
	}

	FString QuotedEntryLabel(const FConvaiObjectEntry& Entry)
	{
		return FString::Printf(TEXT("\"%s\""), *EntryLabel(Entry));
	}

	FConvaiMoveToResult MakeResult(
		EConvaiMoveToResultCode Code,
		const FVector& ResolvedLocation,
		FString AdditionalNote,
		FString DeveloperDetails)
	{
		FConvaiMoveToResult Result;
		Result.Code = Code;
		Result.AdditionalNote = MoveTemp(AdditionalNote);
		Result.DeveloperDetails = MoveTemp(DeveloperDetails);
		Result.ResolvedLocation = ResolvedLocation;
		return Result;
	}

	void LogMoveFailureResult(const FConvaiMoveToResult& Result)
	{
		if (!Result.DeveloperDetails.IsEmpty() && !Result.IsSuccess() &&
			Result.Code != EConvaiMoveToResultCode::Cancelled)
		{
			CONVAI_LOG(
				ConvaiDefinitionsLog,
				Warning,
				TEXT("Convai Move To: %s"),
				*Result.DeveloperDetails);
		}
	}

	FConvaiMoveToResult MakeUnknownDestinationResult(
		const FConvaiObjectEntry& Destination,
		const FVector& ResolvedLocation)
	{
		return MakeResult(
			EConvaiMoveToResultCode::UnknownDestination,
			ResolvedLocation,
			FString::Printf(TEXT("You do not know where %s is."),
				*QuotedEntryLabel(Destination)),
			FString::Printf(
				TEXT("Convai Move To could not resolve a live actor for destination '%s'."),
				*EntryLabel(Destination)));
	}

	FConvaiMoveToResult MakeUnreachableResult(
		const FConvaiObjectEntry& Destination,
		const FVector& ResolvedLocation)
	{
		return MakeResult(
			EConvaiMoveToResultCode::Unreachable,
			ResolvedLocation,
			FString::Printf(TEXT("You cannot reach %s from here."),
				*QuotedEntryLabel(Destination)),
			FString::Printf(
				TEXT("No Convai-resolved navigation path reached destination '%s' from the moving pawn."),
				*EntryLabel(Destination)));
	}

	bool ValidateMovementSetup(
		AActor* MovingCharacter,
		APawn*& OutPawn,
		AAIController*& OutController,
		FConvaiMoveToResult& OutFailure,
		const FVector& ResolvedLocation)
	{
		OutPawn = Cast<APawn>(MovingCharacter);
		OutController = nullptr;
		if (!IsValid(OutPawn))
		{
			OutFailure = MakeResult(
				EConvaiMoveToResultCode::InvalidCharacter,
				ResolvedLocation,
				TEXT("You cannot move right now."),
				TEXT("Convai Move To requires Moving Actor to be a live Pawn."));
			return false;
		}

		OutController = Cast<AAIController>(OutPawn->GetController());
		if (!IsValid(OutController) || OutController->GetPawn() != OutPawn)
		{
			OutFailure = MakeResult(
				EConvaiMoveToResultCode::MissingController,
				ResolvedLocation,
				TEXT("You cannot move right now."),
				TEXT("Convai Move To requires the Pawn to be possessed by an AI Controller."));
			return false;
		}

		if (!IsValid(OutPawn->GetMovementComponent()))
		{
			OutFailure = MakeResult(
				EConvaiMoveToResultCode::MissingMovementComponent,
				ResolvedLocation,
				TEXT("You cannot move right now."),
				TEXT("Convai Move To requires the Pawn to have a movement component."));
			return false;
		}

		if (!IsValid(OutController->GetPathFollowingComponent()))
		{
			OutFailure = MakeResult(
				EConvaiMoveToResultCode::MissingPathFollowingComponent,
				ResolvedLocation,
				TEXT("You cannot move right now."),
				TEXT("Convai Move To requires the AI Controller to have a path-following component."));
			return false;
		}

		UWorld* World = OutPawn->GetWorld();
		UNavigationSystemV1* NavigationSystem =
			World ? UNavigationSystemV1::GetNavigationSystem(World) : nullptr;
		const FNavAgentProperties& AgentProperties =
			OutPawn->GetNavAgentPropertiesRef();
		const FVector AgentLocation = OutPawn->GetNavAgentLocation();
		if (!NavigationSystem ||
			!NavigationSystem->GetNavDataForProps(
				AgentProperties, AgentLocation))
		{
			OutFailure = MakeResult(
				EConvaiMoveToResultCode::MissingNavigationData,
				ResolvedLocation,
				TEXT("You cannot move right now."),
				TEXT("Convai Move To found no compatible navigation data for the Pawn. ")
				TEXT("Add/build a Nav Mesh Bounds Volume and verify the Pawn's navigation-agent settings."));
			return false;
		}

		return true;
	}

	// Emitted when Unreal ends a move that Convai's resolver still considers
	// reachable; captures the controller/pawn/navmesh state the Visual Logger
	// would show, so plain editor logs are enough to diagnose field reports.
	void LogOwnedMoveDiagnostics(
		AAIController* Controller, APawn* Pawn, int32 EngineResult)
	{
		UPathFollowingComponent* PathFollowing =
			Controller ? Controller->GetPathFollowingComponent() : nullptr;
		UPawnMovementComponent* MoveComp =
			Pawn ? Pawn->GetMovementComponent() : nullptr;

		bool bStartOnNavData = false;
		FNavLocation Projected;
		if (Pawn)
		{
			if (UNavigationSystemV1* NavSys =
				FNavigationSystem::GetCurrent<UNavigationSystemV1>(
					Pawn->GetWorld()))
			{
				bStartOnNavData = NavSys->ProjectPointToNavigation(
					Pawn->GetNavAgentLocation(), Projected, INVALID_NAVEXTENT,
					Controller ? &Controller->GetNavAgentPropertiesRef()
							   : nullptr);
			}
		}

		CONVAI_LOG(ConvaiDefinitionsLog, Warning,
			TEXT("Convai Move To diagnostics (engine result %d): Pawn=%s Location=%s NavAgentLocation-on-navmesh=%d PathFollowingStatus=%d MovementComp=%s UpdatedComponent=%s ControllerPawnMatch=%d"),
			EngineResult,
			Pawn ? *Pawn->GetName() : TEXT("null"),
			Pawn ? *Pawn->GetActorLocation().ToCompactString() : TEXT("-"),
			bStartOnNavData ? 1 : 0,
			PathFollowing
				? static_cast<int32>(PathFollowing->GetStatus()) : -1,
			MoveComp ? *MoveComp->GetClass()->GetName() : TEXT("null"),
			(MoveComp && MoveComp->UpdatedComponent)
				? TEXT("valid") : TEXT("null"),
			(Controller && Pawn && Controller->GetPawn() == Pawn) ? 1 : 0);
	}

	FAIMoveRequest MakeMoveRequest(
		AAIController& Controller,
		AActor* GoalActor,
		const FVector& GoalLocation,
		float SemanticAcceptanceRadius,
		bool bUseLocation)
	{
		FAIMoveRequest MoveRequest;
		if (!bUseLocation && IsValid(GoalActor))
		{
			// Unreal observes and repaths toward a moving actor goal. Continuous
			// UAITask tracking remains off because it loops successful moves.
			MoveRequest.SetGoalActor(GoalActor);
		}
		else
		{
			MoveRequest.SetGoalLocation(GoalLocation);
		}

		// Preserve the long-tested Blueprint macro policy. Convai's resolver is
		// still the final authority after Unreal reports completion.
		MoveRequest.SetAcceptanceRadius(
			FMath::Max(0.0f,
				SemanticAcceptanceRadius * EngineAcceptanceRadiusScale));
		MoveRequest.SetReachTestIncludesAgentRadius(true);
		MoveRequest.SetReachTestIncludesGoalRadius(true);
		MoveRequest.SetAllowPartialPath(true);
		MoveRequest.SetUsePathfinding(true);
		MoveRequest.SetProjectGoalLocation(false);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
		MoveRequest.SetRequireNavigableEndLocation(false);
#endif
		MoveRequest.SetNavigationFilter(
			Controller.GetDefaultNavigationFilterClass());
		MoveRequest.SetCanStrafe(false);
		return MoveRequest;
	}

	bool RefreshSelectedGoalLocation(
		FConvaiObjectEntry& Entry,
		int32 SelectedMovementPointIndex,
		FVector& OutGoalLocation)
	{
		check(IsInGameThread());

		AActor* Actor = Entry.Ref.Get();
		if (!Actor)
		{
			return false;
		}

		USceneComponent* Component = Entry.ResolveComponent();
		if (SelectedMovementPointIndex != INDEX_NONE)
		{
			if (!Entry.MovementPoints.IsValidIndex(SelectedMovementPointIndex) ||
				!Entry.MovementPoints[SelectedMovementPointIndex].bEnabled)
			{
				return false;
			}

			const USceneComponent* PointAnchor =
				Entry.ObjectReference ==
						EConvaiObjectReference::SpecificComponent
					? Component
					: nullptr;
			OutGoalLocation =
				Entry.MovementPoints[SelectedMovementPointIndex]
					.ResolveWorldLocation(Actor, PointAnchor);
		}
		else if (Entry.ObjectReference ==
			EConvaiObjectReference::WholeActor)
		{
			OutGoalLocation = Actor->GetActorLocation();
		}
		else if (Component)
		{
			OutGoalLocation =
				!Entry.SocketOrBoneName.IsNone() &&
					Component->DoesSocketExist(Entry.SocketOrBoneName)
				? Component->GetSocketTransform(
					Entry.SocketOrBoneName, RTS_World).GetLocation()
				: Component->GetComponentLocation();
		}
		else
		{
			OutGoalLocation = Actor->GetActorLocation();
		}

		Entry.OptionalPositionVector = OutGoalLocation;
		return true;
	}
}

UConvaiMoveToTask::UConvaiMoveToTask(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bTickingTask = true;
}

UConvaiMoveToTask* UConvaiMoveToTask::CreateMoveToTask(
	AActor* MovingCharacter,
	const FConvaiObjectEntry& Destination,
	FConvaiMoveToResult& OutImmediateResult,
	UGameplayTask* ParentTask,
	bool bLockAILogic)
{
	check(IsInGameThread());

	OutImmediateResult = MakeResult(
		EConvaiMoveToResultCode::MoveFailed,
		FVector::ZeroVector,
		TEXT("You could not move right now."),
		TEXT("Convai Move To did not create a movement task."));

	APawn* Pawn = nullptr;
	AAIController* Controller = nullptr;
	if (!ValidateMovementSetup(
		MovingCharacter, Pawn, Controller, OutImmediateResult,
		FVector::ZeroVector))
	{
		LogMoveFailureResult(OutImmediateResult);
		return nullptr;
	}

	FConvaiObjectEntry ResolvedEntry = Destination;
	AActor* GoalActor = nullptr;
	USceneComponent* GoalComponent = nullptr;
	FVector GoalLocation = FVector::ZeroVector;
	float AcceptanceRadius = 0.0f;
	bool bUseLocation = false;
	bool bResolved = false;
	bool bAlreadyThere = false;
	bool bReachable = false;
	FVector PathEndPoint = FVector::ZeroVector;
	TArray<FVector> PathPoints;
	float TravelDistance = 0.0f;
	int32 MovementPointIndex = INDEX_NONE;
	ResolvedEntry.ResolveGoalLocation(
		Pawn,
		GoalActor,
		GoalComponent,
		GoalLocation,
		AcceptanceRadius,
		bUseLocation,
		bResolved,
		bAlreadyThere,
		bReachable,
		PathEndPoint,
		PathPoints,
		TravelDistance,
		MovementPointIndex);

	if (!bResolved)
	{
		OutImmediateResult =
			MakeUnknownDestinationResult(Destination, GoalLocation);
		LogMoveFailureResult(OutImmediateResult);
		return nullptr;
	}
	if (bAlreadyThere)
	{
		OutImmediateResult = MakeResult(
			EConvaiMoveToResultCode::AlreadyAtDestination,
			GoalLocation,
			FString::Printf(TEXT("You are already at %s."),
				*QuotedEntryLabel(Destination)),
			TEXT(""));
		return nullptr;
	}

	if (!bReachable)
	{
		OutImmediateResult =
			MakeUnreachableResult(Destination, GoalLocation);
		LogMoveFailureResult(OutImmediateResult);
		return nullptr;
	}

	IGameplayTaskOwnerInterface& TaskOwner = ParentTask
		? static_cast<IGameplayTaskOwnerInterface&>(*ParentTask)
		: static_cast<IGameplayTaskOwnerInterface&>(*Controller);
	UConvaiMoveToTask* Task =
		UGameplayTask::NewTask<UConvaiMoveToTask>(
			TaskOwner, TEXT("ConvaiMoveTo"));
	if (!Task)
	{
		OutImmediateResult = MakeResult(
			EConvaiMoveToResultCode::MoveFailed,
			GoalLocation,
			TEXT("You could not move right now."),
			TEXT("Unreal could not allocate the Convai Move To Gameplay Task."));
		LogMoveFailureResult(OutImmediateResult);
		return nullptr;
	}

	Task->MovingPawn = Pawn;
	Task->AIController = Controller;
	Task->DestinationEntry = MoveTemp(ResolvedEntry);
	Task->ResolvedGoalActor = GoalActor;
	Task->ResolvedGoalLocation = GoalLocation;
	Task->LastIssuedGoalLocation = GoalLocation;
	Task->ResolvedAcceptanceRadius = AcceptanceRadius;
	Task->ResolvedMovementPointIndex = MovementPointIndex;
	Task->ResolvedPathPoints = MoveTemp(PathPoints);
	Task->bMoveToLocation = bUseLocation;
	Task->bShouldLockAILogic = bLockAILogic;
	return Task;
}

void UConvaiMoveToTask::Start()
{
	check(IsInGameThread());
	if (bStartRequested || bTerminal)
	{
		return;
	}

	bStartRequested = true;
	ReadyForActivation();
}

void UConvaiMoveToTask::Activate()
{
	Super::Activate();
	if (!bTerminal)
	{
		StartOwnedMove();
	}
}

void UConvaiMoveToTask::TickTask(float DeltaTime)
{
	if (bTerminal || bMoveSuspended || !bMoveToLocation ||
		!ActiveMoveTask || ActiveMoveTask->IsFinished())
	{
		return;
	}

	LocationRefreshElapsed += FMath::Max(0.0f, DeltaTime);
	if (LocationRefreshElapsed < LocationRefreshIntervalSeconds)
	{
		return;
	}
	LocationRefreshElapsed = 0.0f;
	RefreshTrackedLocation();
}

void UConvaiMoveToTask::ExternalCancel()
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiMoveToTask> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis]
		{
			if (UConvaiMoveToTask* Task = WeakThis.Get())
			{
				Task->ExternalCancel();
			}
		});
		return;
	}

	if (bTerminal)
	{
		return;
	}

	Finish(MakeResult(
		EConvaiMoveToResultCode::Cancelled,
		ResolvedGoalLocation,
		TEXT("Movement was cancelled before reaching the destination."),
		TEXT("Convai Move To was cancelled by its caller.")));
}

bool UConvaiMoveToTask::SuspendMovement()
{
	if (bTerminal || bMoveSuspended)
	{
		return bMoveSuspended;
	}

	UConvaiMoveOwnedAITask* Move =
		Cast<UConvaiMoveOwnedAITask>(ActiveMoveTask);
	if (!Move || Move->IsFinished() || !Move->SuspendOwnedRequest())
	{
		return false;
	}

	bMoveSuspended = true;
	return true;
}

bool UConvaiMoveToTask::ResumeMovement()
{
	if (bTerminal)
	{
		return false;
	}

	if (!bMoveSuspended)
	{
		const UConvaiMoveOwnedAITask* ExistingMove =
			Cast<UConvaiMoveOwnedAITask>(ActiveMoveTask);
		return ExistingMove && !ExistingMove->IsFinished();
	}

	FConvaiMoveToResult ImmediateResult;
	if (!ResolveForMovement(ImmediateResult))
	{
		Finish(ImmediateResult);
		return false;
	}

	UConvaiMoveOwnedAITask* Move =
		Cast<UConvaiMoveOwnedAITask>(ActiveMoveTask);
	if (!Move || Move->IsFinished())
	{
		bMoveSuspended = false;
		StartOwnedMove();
		return !bTerminal;
	}

	AAIController* Controller = AIController.Get();
	if (!Controller)
	{
		Finish(MakeResult(
			EConvaiMoveToResultCode::MissingController,
			ResolvedGoalLocation,
			TEXT("You cannot move right now."),
			TEXT("The AI Controller became unavailable while Convai Move To was paused.")));
		return false;
	}

	const FAIMoveRequest Request = MakeMoveRequest(
		*Controller,
		ResolvedGoalActor.Get(),
		ResolvedGoalLocation,
		ResolvedAcceptanceRadius,
		bMoveToLocation);
	LastIssuedGoalLocation = ResolvedGoalLocation;
	bMoveSuspended = false;
	if (Move->RefreshOwnedRequest(Request))
	{
		return true;
	}

	// RefreshOwnedRequest may finish synchronously, re-enter this task, and
	// replace the failed child with the one bounded retry.
	return !bTerminal && ActiveMoveTask != nullptr;
}

bool UConvaiMoveToTask::CopyRemainingPathPoints(
	TArray<FVector>& OutPathPoints) const
{
	if (const UConvaiMoveOwnedAITask* Move =
		Cast<UConvaiMoveOwnedAITask>(ActiveMoveTask))
	{
		return Move->CopyRemainingPathPoints(OutPathPoints);
	}

	OutPathPoints = ResolvedPathPoints;
	return OutPathPoints.Num() >= 2;
}

void UConvaiMoveToTask::StartOwnedMove()
{
	AAIController* Controller = AIController.Get();
	APawn* Pawn = MovingPawn.Get();
	if (!Controller || !Pawn || Controller->GetPawn() != Pawn)
	{
		Finish(MakeResult(
			EConvaiMoveToResultCode::MissingController,
			ResolvedGoalLocation,
			TEXT("You cannot move right now."),
			TEXT("The moving Pawn or its AI Controller became unavailable before movement started.")));
		return;
	}

	UConvaiMoveOwnedAITask* Child =
		UAITask::NewAITask<UConvaiMoveOwnedAITask>(
			*Controller, *this, EAITaskPriority::High,
			TEXT("ConvaiMoveToRequest"));
	if (!Child)
	{
		Finish(MakeResult(
			EConvaiMoveToResultCode::MoveFailed,
			ResolvedGoalLocation,
			TEXT("You could not move right now."),
			TEXT("Unreal could not allocate the owned AI movement task.")));
		return;
	}

	const FAIMoveRequest Request = MakeMoveRequest(
		*Controller,
		ResolvedGoalActor.Get(),
		ResolvedGoalLocation,
		ResolvedAcceptanceRadius,
		bMoveToLocation);
	Child->SetUp(Controller, Request);
	if (bShouldLockAILogic)
	{
		Child->RequestAILogicLocking();
	}
	Child->SetContinuousGoalTracking(false);

	TWeakObjectPtr<UConvaiMoveToTask> WeakThis(this);
	Child->OnOwnedMoveFinished.AddLambda(
		[WeakThis](bool bSucceeded, int32 Result)
		{
			if (UConvaiMoveToTask* Task = WeakThis.Get())
			{
				Task->HandleOwnedMoveFinished(bSucceeded, Result);
			}
		});

	ActiveMoveTask = Child;
	LastIssuedGoalLocation = ResolvedGoalLocation;
	LocationRefreshElapsed = 0.0f;
	bMoveSuspended = false;
	Child->ReadyForActivation();
}

void UConvaiMoveToTask::StopOwnedMove()
{
	UGameplayTask* Move = ActiveMoveTask;
	ActiveMoveTask = nullptr;
	bMoveSuspended = false;
	// An unreachable move task is in the same GC purge as this task; everything
	// it would touch while cancelling is also being destroyed, so let the purge
	// tear it down instead of running the cancel chain.
	if (Move && !Move->IsFinished() && !Move->IsUnreachable())
	{
		bStoppingOwnedMove = true;
		if (UConvaiMoveOwnedAITask* OwnedMove =
			Cast<UConvaiMoveOwnedAITask>(Move))
		{
			OwnedMove->OnOwnedMoveFinished.Clear();
		}
		Move->ExternalCancel();
		bStoppingOwnedMove = false;
	}
}

void UConvaiMoveToTask::HandleOwnedMoveFinished(
	bool bEngineReportedSuccess,
	int32 EngineResult)
{
	if (bTerminal || bStoppingOwnedMove)
	{
		return;
	}

	ActiveMoveTask = nullptr;
	bMoveSuspended = false;

	FConvaiMoveToResult ImmediateResult;
	if (!ResolveForMovement(ImmediateResult))
	{
		Finish(ImmediateResult);
		return;
	}

	if (EngineResult == static_cast<int32>(EPathFollowingResult::Aborted))
	{
		// A competing controller request can pre-empt unlocked movement from
		// inside PathFollowing's NewRequest broadcast. Retrying re-entrantly here
		// would be overwritten when that outer request resumes installation.
		Finish(MakeResult(
			EConvaiMoveToResultCode::MoveFailed,
			ResolvedGoalLocation,
			FString::Printf(TEXT("You could not get to %s right now."),
				*QuotedEntryLabel(DestinationEntry)),
			FString::Printf(
				TEXT("Unreal movement toward '%s' was aborted or replaced by another request."),
				*EntryLabel(DestinationEntry))));
		return;
	}

	LogOwnedMoveDiagnostics(AIController.Get(), MovingPawn.Get(), EngineResult);

	if (ReachableMoveRetryCount < MaxReachableMoveRetries)
	{
		++ReachableMoveRetryCount;
		CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
			TEXT("Convai Move To retrying '%s' after engine result %d (engine success=%d); the fresh Convai resolver still reports it reachable."),
			*EntryLabel(DestinationEntry), EngineResult,
			bEngineReportedSuccess ? 1 : 0);
		StartOwnedMove();
		return;
	}

	Finish(MakeResult(
		EConvaiMoveToResultCode::MoveFailed,
		ResolvedGoalLocation,
		FString::Printf(TEXT("You could not get to %s right now."),
			*QuotedEntryLabel(DestinationEntry)),
		FString::Printf(
			TEXT("Unreal movement ended twice (last result %d, engine success=%d) while the fresh Convai resolver still reported '%s' reachable."),
			EngineResult, bEngineReportedSuccess ? 1 : 0,
			*EntryLabel(DestinationEntry))));
}

bool UConvaiMoveToTask::ResolveForMovement(
	FConvaiMoveToResult& OutImmediateResult)
{
	APawn* Pawn = MovingPawn.Get();
	APawn* ValidatedPawn = nullptr;
	AAIController* Controller = nullptr;
	if (!ValidateMovementSetup(
		Pawn, ValidatedPawn, Controller, OutImmediateResult,
		ResolvedGoalLocation))
	{
		return false;
	}
	MovingPawn = ValidatedPawn;
	AIController = Controller;

	AActor* GoalActor = nullptr;
	USceneComponent* GoalComponent = nullptr;
	FVector GoalLocation = FVector::ZeroVector;
	float AcceptanceRadius = 0.0f;
	bool bUseLocation = false;
	bool bResolved = false;
	bool bAlreadyThere = false;
	bool bReachable = false;
	FVector PathEndPoint = FVector::ZeroVector;
	TArray<FVector> PathPoints;
	float TravelDistance = 0.0f;
	int32 MovementPointIndex = INDEX_NONE;
	DestinationEntry.ResolveGoalLocation(
		Pawn,
		GoalActor,
		GoalComponent,
		GoalLocation,
		AcceptanceRadius,
		bUseLocation,
		bResolved,
		bAlreadyThere,
		bReachable,
		PathEndPoint,
		PathPoints,
		TravelDistance,
		MovementPointIndex);

	ResolvedGoalActor = GoalActor;
	ResolvedGoalLocation = GoalLocation;
	ResolvedAcceptanceRadius = AcceptanceRadius;
	bMoveToLocation = bUseLocation;
	ResolvedMovementPointIndex = MovementPointIndex;
	ResolvedPathPoints = MoveTemp(PathPoints);

	if (!bResolved)
	{
		OutImmediateResult =
			MakeUnknownDestinationResult(DestinationEntry, GoalLocation);
		return false;
	}
	if (bAlreadyThere)
	{
		OutImmediateResult = MakeResult(
			EConvaiMoveToResultCode::Reached,
			GoalLocation,
			TEXT(""),
			TEXT(""));
		return false;
	}

	if (!bReachable)
	{
		OutImmediateResult =
			MakeUnreachableResult(DestinationEntry, GoalLocation);
		return false;
	}

	return true;
}

bool UConvaiMoveToTask::RefreshTrackedLocation()
{
	if (!bMoveToLocation)
	{
		return true;
	}

	FVector RefreshedLocation = FVector::ZeroVector;
	if (!RefreshSelectedGoalLocation(
		DestinationEntry,
		ResolvedMovementPointIndex, RefreshedLocation))
	{
		FConvaiMoveToResult ImmediateResult;
		if (!ResolveForMovement(ImmediateResult))
		{
			Finish(ImmediateResult);
			return false;
		}
		RefreshedLocation = ResolvedGoalLocation;
	}

	ResolvedGoalLocation = RefreshedLocation;
	if (FVector::DistSquared(
			RefreshedLocation, LastIssuedGoalLocation) <
		FMath::Square(LocationRefreshDistance))
	{
		return true;
	}

	AAIController* Controller = AIController.Get();
	UConvaiMoveOwnedAITask* Move =
		Cast<UConvaiMoveOwnedAITask>(ActiveMoveTask);
	if (!Controller || !Move || Move->IsFinished())
	{
		return false;
	}

	const FAIMoveRequest Request = MakeMoveRequest(
		*Controller,
		nullptr,
		ResolvedGoalLocation,
		ResolvedAcceptanceRadius,
		/*bUseLocation*/ true);
	LastIssuedGoalLocation = ResolvedGoalLocation;
	if (!Move->RefreshOwnedRequest(Request))
	{
		return !bTerminal && ActiveMoveTask == Move;
	}
	return true;
}

void UConvaiMoveToTask::Finish(const FConvaiMoveToResult& Result)
{
	if (bTerminal)
	{
		return;
	}

	bTerminal = true;
	LogMoveFailureResult(Result);

	StopOwnedMove();
	EndTask();
	OnFinished.Broadcast(Result);
}

void UConvaiMoveToTask::OnDestroy(bool bInOwnerFinished)
{
	const bool bUnexpectedTermination = !bTerminal;
	bTerminal = true;
	StopOwnedMove();

	if (bUnexpectedTermination)
	{
		const FConvaiMoveToResult Result = MakeResult(
			EConvaiMoveToResultCode::Cancelled,
			ResolvedGoalLocation,
			TEXT("Movement was cancelled before reaching the destination."),
			bInOwnerFinished
				? TEXT("Convai Move To ended because its Gameplay Task owner ended.")
				: TEXT("Convai Move To ended unexpectedly."));
		OnFinished.Broadcast(Result);
	}

	Super::OnDestroy(bInOwnerFinished);
}

UConvaiMoveToProxy* UConvaiMoveToProxy::ConvaiMoveTo(
	UObject* WorldContextObject,
	AActor* MovingCharacter,
	const FConvaiObjectEntry& Destination,
	bool bLockAILogic)
{
	UConvaiMoveToProxy* Proxy = NewObject<UConvaiMoveToProxy>();
	Proxy->MovingCharacter = MovingCharacter;
	Proxy->DestinationEntry = Destination;
	Proxy->bShouldLockAILogic = bLockAILogic;
	Proxy->RegisterWithGameInstance(WorldContextObject);
	return Proxy;
}

void UConvaiMoveToProxy::Activate()
{
	if (!TryBeginActivation())
	{
		return;
	}

	FConvaiMoveToResult ImmediateResult;
	UConvaiMoveToTask* Task = UConvaiMoveToTask::CreateMoveToTask(
		MovingCharacter.Get(),
		DestinationEntry,
		ImmediateResult,
		/*ParentTask*/ nullptr,
		bShouldLockAILogic);
	if (!Task)
	{
		QueueProxyResult(ImmediateResult);
		return;
	}

	ActiveTask = Task;
	Task->OnFinished.AddUObject(
		this, &UConvaiMoveToProxy::HandleTaskFinished);
	Task->Start();
}

void UConvaiMoveToProxy::HandleTaskFinished(
	const FConvaiMoveToResult& Result)
{
	// Queueing prevents an immediate setup/arrival result from firing before
	// the async node's Then path can save the exposed proxy reference.
	QueueProxyResult(Result);
}

void UConvaiMoveToProxy::QueueProxyResult(
	const FConvaiMoveToResult& Result)
{
	if (!TryReserveNaturalCompletion())
	{
		return;
	}

	TWeakObjectPtr<UConvaiMoveToProxy> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Result]
	{
		if (UConvaiMoveToProxy* Proxy = WeakThis.Get())
		{
			Proxy->FinishProxy(Result);
		}
	});
}

void UConvaiMoveToProxy::FinishProxy(
	const FConvaiMoveToResult& Result)
{
	if (!TryClaimNaturalCompletion())
	{
		return;
	}

	ReleaseOwnedTask(/*bCancelTask*/ false);

	if (Result.IsSuccess())
	{
		Succeeded.Broadcast(Result.Code, Result.AdditionalNote);
	}
	else if (Result.Code == EConvaiMoveToResultCode::Cancelled)
	{
		// Cancelled is native lifecycle vocabulary. Explicit Blueprint
		// cancellation detaches before it reaches here; an unexpected owner
		// shutdown is presented as a normal failure so public graphs never
		// need a hidden Cancelled enum case.
		Failed.Broadcast(
			EConvaiMoveToResultCode::MoveFailed,
			Result.AdditionalNote);
	}
	else
	{
		Failed.Broadcast(Result.Code, Result.AdditionalNote);
	}

	SetReadyToDestroy();
}

void UConvaiMoveToProxy::BeginDestroy()
{
	if (IsInGameThread())
	{
		ReleaseOwnedTask(/*bCancelTask*/ !bTerminal);
	}

	Super::BeginDestroy();
}

void UConvaiMoveToProxy::ReleaseOwnedTask(bool bCancelTask)
{
	UConvaiMoveToTask* Task = ActiveTask;
	ActiveTask = nullptr;
	if (!Task)
	{
		return;
	}

	Task->OnFinished.RemoveAll(this);
	if (bCancelTask && !Task->IsFinished())
	{
		Task->ExternalCancel();
	}
}
