// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Actions/ConvaiEscortToTask.h"

#include "Actions/ConvaiMoveToTask.h"
#include "AIController.h"
#include "AIResources.h"
#include "AI/Navigation/NavigationTypes.h"
#include "AITypes.h"
#include "Async/Async.h"
#include "ConvaiChatbotComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameplayTaskOwnerInterface.h"
#include "Tasks/AITask.h"
#include "Utility/Log/ConvaiLogger.h"

UConvaiEscortHoldTask::UConvaiEscortHoldTask(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	AddRequiredResource(UAIResource_Movement::StaticClass());
	AddClaimedResource(UAIResource_Movement::StaticClass());
}

void UConvaiEscortHoldTask::ExternalCancel()
{
	if (!IsFinished())
	{
		EndTask();
	}
}

namespace
{
	// Hysteresis prevents a guide near one threshold from repeatedly stopping
	// and resuming. These are conservative internal defaults.
	constexpr float PauseDistance = 700.0f;
	constexpr float ResumeDistance = 450.0f;
	constexpr float LagGraceSeconds = 0.75f;
	constexpr float MaxCountedTickSeconds = 0.25f;
	constexpr float AheadPathHorizontalCorridorDistance = 250.0f;
	constexpr float AheadPathVerticalCorridorDistance = 150.0f;
	constexpr float MinimumAheadPathProgress = 100.0f;
	constexpr float ProjectionAmbiguityDistance = 50.0f;

	const TCHAR* FollowPrompt =
		TEXT("The person you are escorting has fallen behind. Briefly ask them to ")
		TEXT("follow you, then wait for them without starting a new action plan.");

	const TCHAR* InternalEscortFailureFeedback =
		TEXT("Escort cannot continue in the current state.");

	FConvaiEscortResult MakeEscortResult(
		EConvaiEscortResultCode Code,
		FString AdditionalNote = FString(),
		FString DeveloperDetails = FString())
	{
		FConvaiEscortResult Result;
		Result.Code = Code;
		Result.AdditionalNote = MoveTemp(AdditionalNote);
		Result.DeveloperDetails = MoveTemp(DeveloperDetails);
		return Result;
	}

	FString EntryLabel(const FConvaiObjectEntry& Entry, const TCHAR* Fallback)
	{
		const FString Trimmed = Entry.Name.TrimStartAndEnd();
		return Trimmed.IsEmpty() ? FString(Fallback) : Trimmed;
	}

	FString UnavailableDestinationFeedback(
		const FConvaiObjectEntry& Destination)
	{
		return FString::Printf(
			TEXT("Escort cannot continue because \"%s\" is not an available ")
			TEXT("destination. Retry Escort with another known destination, or ask ")
			TEXT("the user where to go."),
			*EntryLabel(Destination, TEXT("the destination")));
	}

	FString UnavailableEscorteeFeedback(
		const FConvaiObjectEntry& Escortee)
	{
		return FString::Printf(
			TEXT("Escort cannot continue because \"%s\" is not an available ")
			TEXT("character. Retry Escort with another known character, or ask the ")
			TEXT("user whom to escort."),
			*EntryLabel(Escortee, TEXT("the escorted character")));
	}
}

UConvaiEscortToTask::UConvaiEscortToTask(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bTickingTask = true;
}

UConvaiEscortToTask* UConvaiEscortToTask::CreateEscortToTask(
	UConvaiChatbotComponent& Chatbot,
	const FConvaiObjectEntry& EscortedCharacter,
	const FConvaiObjectEntry& Destination,
	FConvaiEscortResult& OutImmediateResult)
{
	check(IsInGameThread());

	OutImmediateResult = MakeEscortResult(
		EConvaiEscortResultCode::InvalidGuideSetup,
		InternalEscortFailureFeedback,
		TEXT("Escort could not create its Gameplay Task."));

	APawn* OwnerPawn = Cast<APawn>(Chatbot.GetOwner());
	AAIController* OwnerController = OwnerPawn
		? Cast<AAIController>(OwnerPawn->GetController())
		: nullptr;
	if (!OwnerPawn || !OwnerController)
	{
		OutImmediateResult.DeveloperDetails =
			TEXT("Escort requires the Convai Chatbot Component to be attached ")
			TEXT("to a Pawn controlled by an AI Controller.");
		CONVAI_LOG(
			ConvaiChatbotComponentLog,
			Warning,
			TEXT("Escort: %s"),
			*OutImmediateResult.DeveloperDetails);
		return nullptr;
	}

	IGameplayTaskOwnerInterface& TaskOwner =
		static_cast<IGameplayTaskOwnerInterface&>(*OwnerController);
	UConvaiEscortToTask* Task =
		UGameplayTask::NewTask<UConvaiEscortToTask>(
			TaskOwner, TEXT("ConvaiEscortTo"));
	if (!Task)
	{
		CONVAI_LOG(
			ConvaiChatbotComponentLog,
			Warning,
			TEXT("Escort: %s"),
			*OutImmediateResult.DeveloperDetails);
		return nullptr;
	}

	Task->ChatbotComponent = &Chatbot;
	Task->AIController = OwnerController;
	Task->MovingPawn = OwnerPawn;
	Task->EscortedCharacterEntry = EscortedCharacter;
	Task->DestinationEntry = Destination;
	return Task;
}

void UConvaiEscortToTask::Start()
{
	check(IsInGameThread());
	if (bStartRequested || bTerminal)
	{
		return;
	}

	bStartRequested = true;
	ReadyForActivation();
}

void UConvaiEscortToTask::Activate()
{
	Super::Activate();

	UConvaiChatbotComponent* Chatbot = ChatbotComponent.Get();
	APawn* Pawn = MovingPawn.Get();
	AAIController* Controller = AIController.Get();
	if (!Chatbot || !Pawn || !Controller || Chatbot->GetOwner() != Pawn ||
		Controller->GetPawn() != Pawn)
	{
		CompleteWithFailure(
			EConvaiEscortResultCode::InvalidGuideSetup,
			InternalEscortFailureFeedback,
			TEXT("Escort requires the Convai Chatbot Component to be attached ")
			TEXT("to a Pawn controlled by an AI Controller."));
		return;
	}

	AActor* EscortedActor = EscortedCharacterEntry.Ref.Get();
	if (!IsValid(EscortedActor))
	{
		CompleteWithFailure(
			EConvaiEscortResultCode::EscorteeUnavailable,
			UnavailableEscorteeFeedback(EscortedCharacterEntry),
			TEXT("The character to escort was unavailable before Escort started."));
		return;
	}
	if (EscortedActor == Pawn)
	{
		CompleteWithFailure(
			EConvaiEscortResultCode::EscorteeUnavailable,
			TEXT("Escort cannot start because a character cannot escort itself. ")
			TEXT("Retry Escort with another available character."),
			TEXT("Escorted Character resolved to Escorting Actor itself."));
		return;
	}

	StartOrFinishMovement();
}

void UConvaiEscortToTask::TickTask(float DeltaTime)
{
	if (bTerminal ||
		EscortState == EEscortTaskState::Finished)
	{
		return;
	}

	APawn* Pawn = MovingPawn.Get();
	AAIController* Controller = AIController.Get();
	if (!Pawn || !Controller || Controller->GetPawn() != Pawn)
	{
		CompleteWithFailure(
			EConvaiEscortResultCode::InvalidGuideSetup,
			InternalEscortFailureFeedback,
			TEXT("The escorting Pawn or its AI Controller became unavailable during Escort."));
		return;
	}
	if (!EscortedCharacterEntry.Ref.IsValid())
	{
		CompleteWithFailure(
			EConvaiEscortResultCode::EscorteeUnavailable,
			UnavailableEscorteeFeedback(EscortedCharacterEntry),
			TEXT("The escorted-character actor became invalid during Escort."));
		return;
	}
	if (!DestinationEntry.Ref.IsValid())
	{
		CompleteWithFailure(
			EConvaiEscortResultCode::DestinationUnavailable,
			UnavailableDestinationFeedback(DestinationEntry),
			TEXT("The destination actor became invalid during Escort."));
		return;
	}

	const float EscorteeDistance = GetEscorteeDistance();
	if (EscorteeDistance < 0.0f || !FMath::IsFinite(EscorteeDistance))
	{
		CompleteWithFailure(
			EConvaiEscortResultCode::EscorteeUnavailable,
			UnavailableEscorteeFeedback(EscortedCharacterEntry),
			TEXT("The escorted character has no valid world position."));
		return;
	}

	if (EscortState == EEscortTaskState::Moving)
	{
		if (IsEscorteeLagging(EscorteeDistance))
		{
			LaggingSeconds += FMath::Min(
				FMath::Max(DeltaTime, 0.0f), MaxCountedTickSeconds);
			if (LaggingSeconds >= LagGraceSeconds)
			{
				EnterWaitingForEscortee();
			}
		}
		else
		{
			LaggingSeconds = 0.0f;
		}
		return;
	}

	const bool bEscorteeNear = IsEscorteeNear(EscorteeDistance);
	if (EscortState == EEscortTaskState::WaitingForEscortee &&
		(bEscorteeNear || IsEscorteeAheadOnRemainingPath()))
	{
		WithdrawFollowPrompt();
		ClearEscortFocus();
		LaggingSeconds = 0.0f;
		StopOwnedHold();

		if (ActiveMoveTask && !ActiveMoveTask->IsFinished())
		{
			EscortState = EEscortTaskState::Moving;
			if (ActiveMoveTask->ResumeMovement())
			{
				return;
			}
			if (bTerminal ||
				EscortState == EEscortTaskState::Finished)
			{
				return;
			}
			StopOwnedMovement();
		}

		StartOrFinishMovement();
	}
}

void UConvaiEscortToTask::ExternalCancel()
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiEscortToTask> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis]
		{
			if (UConvaiEscortToTask* Task = WeakThis.Get())
			{
				Task->ExternalCancel();
			}
		});
		return;
	}

	CompleteCancellation();
}

void UConvaiEscortToTask::OnGameplayTaskDeactivated(
	UGameplayTask& Task)
{
	const bool bWasOwnedHold = &Task == ActiveHoldTask;
	if (bWasOwnedHold && Task.IsFinished())
	{
		ActiveHoldTask = nullptr;
	}

	Super::OnGameplayTaskDeactivated(Task);

	if (bWasOwnedHold && !bStoppingOwnedHold &&
		!bTerminal)
	{
		CompleteWithFailure(
			EConvaiEscortResultCode::InvalidGuideSetup,
			InternalEscortFailureFeedback,
			TEXT("Escort lost its movement reservation while waiting for the escorted character."));
	}
}

void UConvaiEscortToTask::OnDestroy(bool bInOwnerFinished)
{
	const bool bUnexpectedTermination = !bTerminal;
	if (bUnexpectedTermination)
	{
		bTerminal = true;
		EscortState = EEscortTaskState::Finished;
	}

	StopOwnedMovement();
	StopOwnedHold();
	ClearEscortFocus();
	WithdrawFollowPrompt();

	if (bUnexpectedTermination)
	{
		const FConvaiEscortResult Result = MakeEscortResult(
			EConvaiEscortResultCode::Cancelled,
			TEXT("Escort was cancelled before reaching the destination."),
			bInOwnerFinished
				? TEXT("Escort ended because its Gameplay Task owner ended.")
				: TEXT("Escort ended unexpectedly."));
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning,
			TEXT("Escort: %s"), *Result.DeveloperDetails);
		OnFinished.Broadcast(Result);
	}

	Super::OnDestroy(bInOwnerFinished);
}

void UConvaiEscortToTask::StartOrFinishMovement()
{
	if (bTerminal)
	{
		return;
	}

	const float EscorteeDistance = GetEscorteeDistance();
	if (EscorteeDistance < 0.0f || !FMath::IsFinite(EscorteeDistance))
	{
		CompleteWithFailure(
			EConvaiEscortResultCode::EscorteeUnavailable,
			UnavailableEscorteeFeedback(EscortedCharacterEntry),
			TEXT("The escorted character has no valid world position."));
		return;
	}

	APawn* Pawn = MovingPawn.Get();
	FConvaiMoveToResult ImmediateResult;
	UConvaiMoveToTask* Move = UConvaiMoveToTask::CreateMoveToTask(
		Pawn, DestinationEntry, ImmediateResult, this,
		/*bLockAILogic*/ true);
	if (!Move)
	{
		HandleImmediateMoveResult(ImmediateResult);
		return;
	}

	ActiveMoveTask = Move;
	Move->OnFinished.AddUObject(
		this, &UConvaiEscortToTask::HandleMoveFinished);
	Move->CopyRemainingPathPoints(ResolvedPathPoints);

	if (IsEscorteeLagging(EscorteeDistance))
	{
		EnterWaitingForEscortee();
		return;
	}

	EscortState = EEscortTaskState::Moving;
	LaggingSeconds = 0.0f;
	Move->Start();
}

void UConvaiEscortToTask::HandleMoveFinished(
	const FConvaiMoveToResult& Result)
{
	if (bTerminal || bStoppingOwnedMove)
	{
		return;
	}

	if (ActiveMoveTask)
	{
		ActiveMoveTask->OnFinished.RemoveAll(this);
	}
	ActiveMoveTask = nullptr;

	if (Result.IsSuccess())
	{
		if (Result.Code == EConvaiMoveToResultCode::Reached)
		{
			bReachedDestinationAfterTravel = true;
		}

		const float EscorteeDistance = GetEscorteeDistance();
		if (EscorteeDistance < 0.0f ||
			!FMath::IsFinite(EscorteeDistance))
		{
			CompleteWithFailure(
				EConvaiEscortResultCode::EscorteeUnavailable,
				UnavailableEscorteeFeedback(EscortedCharacterEntry),
				TEXT("The escorted character became unavailable at the destination."));
		}
		else if (IsEscorteeNear(EscorteeDistance))
		{
			CompleteSuccessfully(MapMoveSuccessCode(Result));
		}
		else
		{
			ResolvedPathPoints.Reset();
			EnterWaitingForEscortee();
		}
		return;
	}

	HandleImmediateMoveResult(Result);
}

void UConvaiEscortToTask::HandleImmediateMoveResult(
	const FConvaiMoveToResult& Result)
{
	if (Result.IsSuccess())
	{
		const float EscorteeDistance = GetEscorteeDistance();
		if (EscorteeDistance >= 0.0f &&
			FMath::IsFinite(EscorteeDistance) &&
			IsEscorteeNear(EscorteeDistance))
		{
			CompleteSuccessfully(MapMoveSuccessCode(Result));
		}
		else
		{
			ResolvedPathPoints.Reset();
			EnterWaitingForEscortee();
		}
		return;
	}

	if (Result.Code == EConvaiMoveToResultCode::Cancelled)
	{
		CompleteCancellation();
		return;
	}

	const FString Feedback = Result.AdditionalNote.IsEmpty()
		? FString(InternalEscortFailureFeedback)
		: Result.AdditionalNote;
	const EConvaiEscortResultCode Code =
		Result.Code == EConvaiMoveToResultCode::UnknownDestination ||
			Result.Code == EConvaiMoveToResultCode::Unreachable
		? EConvaiEscortResultCode::DestinationUnavailable
		: (Result.Code == EConvaiMoveToResultCode::MoveFailed
			? EConvaiEscortResultCode::MovementFailed
			: EConvaiEscortResultCode::InvalidGuideSetup);
	CompleteWithFailure(
		Code,
		Feedback,
		Result.DeveloperDetails);
}

EConvaiEscortResultCode UConvaiEscortToTask::MapMoveSuccessCode(
	const FConvaiMoveToResult& Result) const
{
	return Result.Code == EConvaiMoveToResultCode::AlreadyAtDestination &&
			!bReachedDestinationAfterTravel
		? EConvaiEscortResultCode::AlreadyAtDestination
		: EConvaiEscortResultCode::Reached;
}

void UConvaiEscortToTask::EnterWaitingForEscortee()
{
	if (bTerminal)
	{
		return;
	}

	bool bOwnsWaitResources = false;
	if (ActiveMoveTask && !ActiveMoveTask->IsFinished())
	{
		ActiveMoveTask->CopyRemainingPathPoints(ResolvedPathPoints);
		bOwnsWaitResources = ActiveMoveTask->SuspendMovement();
		if (!bOwnsWaitResources)
		{
			StopOwnedMovement();
		}
	}
	if (!bOwnsWaitResources)
	{
		bOwnsWaitResources = StartOwnedHold();
	}
	if (!bOwnsWaitResources)
	{
		CompleteWithFailure(
			EConvaiEscortResultCode::InvalidGuideSetup,
			InternalEscortFailureFeedback,
			TEXT("Escort could not reserve movement while waiting for the escorted character."));
		return;
	}

	EscortState = EEscortTaskState::WaitingForEscortee;
	LaggingSeconds = 0.0f;

	AAIController* Controller = AIController.Get();
	AActor* EscortedActor = EscortedCharacterEntry.Ref.Get();
	if (Controller && IsValid(EscortedActor))
	{
		PreviousGameplayFocusActor =
			Controller->GetFocusActorForPriority(
				EAIFocusPriority::Gameplay);
		PreviousGameplayFocalPoint =
			Controller->GetFocalPointForPriority(
				EAIFocusPriority::Gameplay);
		bHadPreviousGameplayFocalPoint =
			!PreviousGameplayFocusActor.IsValid() &&
			FAISystem::IsValidLocation(
				PreviousGameplayFocalPoint);
		Controller->SetFocus(
			EscortedActor, EAIFocusPriority::Gameplay);
		bOwnsEscortFocus = true;
	}

	if (!bFollowPromptSent)
	{
		bFollowPromptSent = true;
		if (UConvaiChatbotComponent* Chatbot = ChatbotComponent.Get())
		{
			Chatbot->AddContextEvent(
				FollowPrompt,
				EC_RunLLMOption::Always,
				EConvaiContextDelivery::WaitUntilConversationIsIdle,
				/*bEphemeral*/ true,
				/*bFlushImmediately*/ false);
		}
	}
}

bool UConvaiEscortToTask::StartOwnedHold()
{
	if (ActiveHoldTask && !ActiveHoldTask->IsFinished())
	{
		return true;
	}

	AAIController* Controller = AIController.Get();
	if (!Controller)
	{
		return false;
	}

	UConvaiEscortHoldTask* Hold =
		UAITask::NewAITask<UConvaiEscortHoldTask>(
			*Controller, *this, EAITaskPriority::High,
			TEXT("ConvaiEscortHold"));
	if (!Hold)
	{
		return false;
	}
	Hold->RequestAILogicLocking();
	ActiveHoldTask = Hold;
	Hold->ReadyForActivation();
	return !Hold->IsFinished();
}

void UConvaiEscortToTask::StopOwnedHold()
{
	UGameplayTask* Hold = ActiveHoldTask;
	ActiveHoldTask = nullptr;
	if (Hold && !Hold->IsFinished())
	{
		bStoppingOwnedHold = true;
		Hold->ExternalCancel();
		bStoppingOwnedHold = false;
	}
}

void UConvaiEscortToTask::StopOwnedMovement()
{
	UConvaiMoveToTask* Move = ActiveMoveTask;
	ActiveMoveTask = nullptr;
	if (Move)
	{
		Move->OnFinished.RemoveAll(this);
		if (!Move->IsFinished())
		{
			bStoppingOwnedMove = true;
			Move->ExternalCancel();
			bStoppingOwnedMove = false;
		}
	}
}

void UConvaiEscortToTask::ClearEscortFocus()
{
	AAIController* Controller = AIController.Get();
	AActor* EscortedActor = EscortedCharacterEntry.Ref.Get();
	if (bOwnsEscortFocus && Controller &&
		Controller->GetFocusActorForPriority(
			EAIFocusPriority::Gameplay) == EscortedActor)
	{
		Controller->ClearFocus(EAIFocusPriority::Gameplay);
		if (AActor* PreviousFocusActor =
			PreviousGameplayFocusActor.Get())
		{
			Controller->SetFocus(
				PreviousFocusActor,
				EAIFocusPriority::Gameplay);
		}
		else if (bHadPreviousGameplayFocalPoint)
		{
			Controller->SetFocalPoint(
				PreviousGameplayFocalPoint,
				EAIFocusPriority::Gameplay);
		}
	}
	PreviousGameplayFocusActor.Reset();
	PreviousGameplayFocalPoint = FVector::ZeroVector;
	bHadPreviousGameplayFocalPoint = false;
	bOwnsEscortFocus = false;
}

void UConvaiEscortToTask::WithdrawFollowPrompt()
{
	if (!bFollowPromptSent)
	{
		return;
	}

	if (UConvaiChatbotComponent* Chatbot = ChatbotComponent.Get())
	{
		Chatbot->WithdrawPendingContextEvent(
			FollowPrompt, /*bEphemeral*/ true);
	}
	bFollowPromptSent = false;
}

bool UConvaiEscortToTask::IsEscorteeNear(float Distance)
{
	return Distance >= 0.0f && Distance <= ResumeDistance;
}

bool UConvaiEscortToTask::IsEscorteeLagging(float Distance)
{
	return Distance >= PauseDistance &&
		!IsEscorteeAheadOnRemainingPath();
}

bool UConvaiEscortToTask::IsEscorteeAheadOnRemainingPath()
{
	const APawn* Pawn = MovingPawn.Get();
	const AActor* EscortedActor = EscortedCharacterEntry.Ref.Get();
	if (!Pawn || !EscortedActor)
	{
		return false;
	}

	if (ActiveMoveTask &&
		!ActiveMoveTask->CopyRemainingPathPoints(
			ResolvedPathPoints))
	{
		ResolvedPathPoints.Reset();
		return false;
	}

	auto GetRouteLocation = [](const AActor& Actor)
	{
		if (const APawn* NavPawn = Cast<APawn>(&Actor))
		{
			const FVector NavLocation =
				NavPawn->GetNavAgentLocation();
			if (FNavigationSystem::IsValidLocation(NavLocation) &&
				FMath::IsFinite(NavLocation.X) &&
				FMath::IsFinite(NavLocation.Y) &&
				FMath::IsFinite(NavLocation.Z))
			{
				return NavLocation;
			}
		}
		return Actor.GetActorLocation();
	};

	return IsLocationClearlyAheadOnPath(
		ResolvedPathPoints,
		GetRouteLocation(*Pawn),
		GetRouteLocation(*EscortedActor));
}

bool UConvaiEscortToTask::IsLocationClearlyAheadOnPath(
	const TArray<FVector>& PathPoints,
	const FVector& GuideLocation,
	const FVector& EscorteeLocation)
{
	auto IsFiniteVector = [](const FVector& Value)
	{
		return FMath::IsFinite(Value.X) &&
			FMath::IsFinite(Value.Y) &&
			FMath::IsFinite(Value.Z);
	};
	auto IsInsidePathCorridor = [](
		const FVector& Location,
		const FVector& Projection)
	{
		return FVector::DistSquaredXY(Location, Projection) <=
			FMath::Square(
				AheadPathHorizontalCorridorDistance) &&
			FMath::Abs(Location.Z - Projection.Z) <=
				AheadPathVerticalCorridorDistance;
	};

	if (PathPoints.Num() < 2 ||
		!IsFiniteVector(GuideLocation) ||
		!IsFiniteVector(EscorteeLocation))
	{
		return false;
	}
	for (const FVector& PathPoint : PathPoints)
	{
		if (!IsFiniteVector(PathPoint))
		{
			return false;
		}
	}

	int32 GuideSegmentIndex = INDEX_NONE;
	FVector GuideProjection = FVector::ZeroVector;
	for (int32 SegmentIndex = 0;
		SegmentIndex + 1 < PathPoints.Num();
		++SegmentIndex)
	{
		if (!PathPoints[SegmentIndex].Equals(
			PathPoints[SegmentIndex + 1]))
		{
			GuideSegmentIndex = SegmentIndex;
			GuideProjection = FMath::ClosestPointOnSegment(
				GuideLocation,
				PathPoints[SegmentIndex],
				PathPoints[SegmentIndex + 1]);
			break;
		}
	}
	if (GuideSegmentIndex == INDEX_NONE ||
		!IsInsidePathCorridor(
			GuideLocation, GuideProjection))
	{
		return false;
	}

	float BestDistance = TNumericLimits<float>::Max();
	float MinimumTiedProgress = TNumericLimits<float>::Max();
	float MaximumTiedProgress = 0.0f;
	int32 EarliestTiedSegment = INDEX_NONE;
	int32 LatestTiedSegment = INDEX_NONE;
	float ProgressAtSegmentStart = 0.0f;

	for (int32 SegmentIndex = GuideSegmentIndex;
		SegmentIndex + 1 < PathPoints.Num();
		++SegmentIndex)
	{
		const FVector SegmentStart =
			SegmentIndex == GuideSegmentIndex
				? GuideProjection
				: PathPoints[SegmentIndex];
		const FVector& SegmentEnd =
			PathPoints[SegmentIndex + 1];
		const float SegmentLength =
			FVector::Distance(SegmentStart, SegmentEnd);
		if (SegmentLength <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		const FVector Projection =
			FMath::ClosestPointOnSegment(
				EscorteeLocation,
				SegmentStart,
				SegmentEnd);
		if (!IsInsidePathCorridor(
			EscorteeLocation, Projection))
		{
			ProgressAtSegmentStart += SegmentLength;
			continue;
		}

		const float Distance =
			FVector::Distance(EscorteeLocation, Projection);
		const float Progress =
			ProgressAtSegmentStart +
			FVector::Distance(SegmentStart, Projection);
		if (Distance + ProjectionAmbiguityDistance <
			BestDistance)
		{
			BestDistance = Distance;
			MinimumTiedProgress = Progress;
			MaximumTiedProgress = Progress;
			EarliestTiedSegment = SegmentIndex;
			LatestTiedSegment = SegmentIndex;
		}
		else if (FMath::Abs(Distance - BestDistance) <=
			ProjectionAmbiguityDistance)
		{
			MinimumTiedProgress =
				FMath::Min(MinimumTiedProgress, Progress);
			MaximumTiedProgress =
				FMath::Max(MaximumTiedProgress, Progress);
			EarliestTiedSegment =
				FMath::Min(EarliestTiedSegment, SegmentIndex);
			LatestTiedSegment =
				FMath::Max(LatestTiedSegment, SegmentIndex);
		}

		ProgressAtSegmentStart += SegmentLength;
	}

	if (EarliestTiedSegment == INDEX_NONE)
	{
		return false;
	}

	const bool bAmbiguousNonAdjacentProjection =
		LatestTiedSegment - EarliestTiedSegment > 1 &&
		MaximumTiedProgress - MinimumTiedProgress >
			AheadPathHorizontalCorridorDistance;
	return !bAmbiguousNonAdjacentProjection &&
		MinimumTiedProgress >= MinimumAheadPathProgress;
}

float UConvaiEscortToTask::GetEscorteeDistance() const
{
	const APawn* Pawn = MovingPawn.Get();
	const AActor* EscortedActor =
		EscortedCharacterEntry.Ref.Get();
	if (!Pawn || !EscortedActor)
	{
		return -1.0f;
	}

	return FVector::Dist(
		Pawn->GetActorLocation(),
		EscortedActor->GetActorLocation());
}

void UConvaiEscortToTask::CompleteSuccessfully(
	EConvaiEscortResultCode Code)
{
	if (bTerminal)
	{
		return;
	}

	check(Code == EConvaiEscortResultCode::Reached ||
		Code == EConvaiEscortResultCode::AlreadyAtDestination);

	// Do not add an arrival context event here. The Blueprint action handler
	// owns Handle Action Completion, auto-reporting, and response delivery.
	Finish(MakeEscortResult(Code));
}

void UConvaiEscortToTask::CompleteWithFailure(
	EConvaiEscortResultCode Code,
	const FString& ModelFeedback,
	const FString& DeveloperDiagnostic)
{
	if (bTerminal)
	{
		return;
	}

	Finish(MakeEscortResult(
		Code,
		ModelFeedback,
		DeveloperDiagnostic));
}

void UConvaiEscortToTask::CompleteCancellation()
{
	if (bTerminal)
	{
		return;
	}

	Finish(MakeEscortResult(
		EConvaiEscortResultCode::Cancelled,
		TEXT("Escort was cancelled before reaching the destination."),
		TEXT("Escort was cancelled by its caller.")));
}

void UConvaiEscortToTask::Finish(const FConvaiEscortResult& Result)
{
	if (bTerminal)
	{
		return;
	}

	bTerminal = true;
	EscortState = EEscortTaskState::Finished;
	if (!Result.DeveloperDetails.IsEmpty() && !Result.IsSuccess() &&
		Result.Code != EConvaiEscortResultCode::Cancelled)
	{
		CONVAI_LOG(
			ConvaiChatbotComponentLog,
			Warning,
			TEXT("Escort: %s"),
			*Result.DeveloperDetails);
	}

	EndTask();
	OnFinished.Broadcast(Result);
}

UConvaiEscortProxy* UConvaiEscortProxy::ConvaiEscort(
	UObject* WorldContextObject,
	AActor* EscortingActor,
	const FConvaiObjectEntry& EscortedCharacter,
	const FConvaiObjectEntry& Destination)
{
	UConvaiEscortProxy* Proxy = NewObject<UConvaiEscortProxy>();
	Proxy->EscortingActor = EscortingActor;
	Proxy->EscortedCharacterEntry = EscortedCharacter;
	Proxy->DestinationEntry = Destination;
	Proxy->RegisterWithGameInstance(WorldContextObject);
	return Proxy;
}

void UConvaiEscortProxy::Activate()
{
	if (!TryBeginActivation())
	{
		return;
	}

	AActor* Actor = EscortingActor.Get();
	UConvaiChatbotComponent* Chatbot = Actor
		? Actor->FindComponentByClass<UConvaiChatbotComponent>()
		: nullptr;
	if (!Chatbot)
	{
		FConvaiEscortResult Result;
		Result.Code = EConvaiEscortResultCode::InvalidGuideSetup;
		Result.AdditionalNote =
			TEXT("Escort could not start in the current state.");
		Result.DeveloperDetails =
			TEXT("Convai Escort requires Escorting Actor to own a live ")
			TEXT("Convai Chatbot Component.");
		CONVAI_LOG(
			ConvaiChatbotComponentLog,
			Warning,
			TEXT("Escort: %s"),
			*Result.DeveloperDetails);
		QueueProxyResult(Result);
		return;
	}

	FConvaiEscortResult ImmediateResult;
	UConvaiEscortToTask* Task = UConvaiEscortToTask::CreateEscortToTask(
		*Chatbot,
		EscortedCharacterEntry,
		DestinationEntry,
		ImmediateResult);
	if (!Task)
	{
		QueueProxyResult(ImmediateResult);
		return;
	}

	ActiveTask = Task;
	Task->OnFinished.AddUObject(
		this, &UConvaiEscortProxy::HandleTaskFinished);
	Task->Start();
}

void UConvaiEscortProxy::HandleTaskFinished(
	const FConvaiEscortResult& Result)
{
	QueueProxyResult(Result);
}

void UConvaiEscortProxy::QueueProxyResult(
	const FConvaiEscortResult& Result)
{
	if (!TryReserveNaturalCompletion())
	{
		return;
	}

	TWeakObjectPtr<UConvaiEscortProxy> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Result]
	{
		if (UConvaiEscortProxy* Proxy = WeakThis.Get())
		{
			Proxy->FinishProxy(Result);
		}
	});
}

void UConvaiEscortProxy::FinishProxy(
	const FConvaiEscortResult& Result)
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
	else if (Result.Code == EConvaiEscortResultCode::Cancelled)
	{
		// Keep the internal lifecycle code out of public Blueprint graphs.
		// Explicit proxy cancellation is detached and silent; unexpected
		// owner shutdown becomes an ordinary failure.
		Failed.Broadcast(
			EConvaiEscortResultCode::MovementFailed,
			Result.AdditionalNote);
	}
	else
	{
		Failed.Broadcast(Result.Code, Result.AdditionalNote);
	}

	SetReadyToDestroy();
}

void UConvaiEscortProxy::BeginDestroy()
{
	if (IsInGameThread())
	{
		ReleaseOwnedTask(/*bCancelTask*/ !bTerminal);
	}

	Super::BeginDestroy();
}

void UConvaiEscortProxy::ReleaseOwnedTask(bool bCancelTask)
{
	UConvaiEscortToTask* Task = ActiveTask;
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
