// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actions/ConvaiCancellableTaskProxy.h"
#include "ConvaiDefinitions.h"
#include "GameplayTask.h"
#include "Tasks/AITask_MoveTo.h"
#include "ConvaiMoveToTask.generated.h"

class AAIController;
class APawn;
class UGameplayTask;

/** Why a Convai Move To request ended. */
UENUM(BlueprintType)
enum class EConvaiMoveToResultCode : uint8
{
	/** The character reached the resolved destination. */
	Reached UMETA(DisplayName = "Reached"),
	/** No move was necessary because the character was already there. */
	AlreadyAtDestination UMETA(DisplayName = "Already At Destination"),
	/** The destination actor disappeared or could not be resolved. */
	UnknownDestination UMETA(DisplayName = "Unknown Destination"),
	/** The destination is known, but no usable route reaches it from here. */
	Unreachable UMETA(DisplayName = "Unreachable"),
	/** Moving Actor is not a usable pawn. */
	InvalidCharacter UMETA(DisplayName = "Invalid Character"),
	/** The pawn is not controlled by an AI Controller. */
	MissingController UMETA(DisplayName = "Missing AI Controller"),
	/** The pawn has no movement component. */
	MissingMovementComponent UMETA(DisplayName = "Missing Movement Component"),
	/** The AI Controller has no path-following component. */
	MissingPathFollowingComponent UMETA(DisplayName = "Missing Path Following Component"),
	/** The world has no compatible navigation data for this pawn. */
	MissingNavigationData UMETA(DisplayName = "Missing Navigation Data"),
	/** Unreal stopped the move even though the destination still resolved as reachable. */
	MoveFailed UMETA(DisplayName = "Move Failed"),
	/**
	 * Native lifecycle marker used when a composed C++ task is cancelled.
	 * Explicit proxy cancellation detaches first and stays silent; unexpected
	 * owner termination reaches the public Failed pin so the caller cannot hang.
	 */
	Cancelled UMETA(Hidden)
};

/**
 * Terminal result shared by the C++ Gameplay Task and the Blueprint async node.
 *
 * Additional Note is safe to show to a player or feed back to a Convai
 * character. Developer Details is diagnostic-only: log or display it to the
 * developer, but never send it to character context.
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiMoveToResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Movement")
	EConvaiMoveToResultCode Code = EConvaiMoveToResultCode::MoveFailed;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Movement")
	FString AdditionalNote;

	UPROPERTY()
	FString DeveloperDetails;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Movement", AdvancedDisplay)
	FVector ResolvedLocation = FVector::ZeroVector;

	bool IsSuccess() const
	{
		return Code == EConvaiMoveToResultCode::Reached ||
			Code == EConvaiMoveToResultCode::AlreadyAtDestination;
	}

	bool ShouldReportToCharacter() const
	{
		return Code == EConvaiMoveToResultCode::UnknownDestination ||
			Code == EConvaiMoveToResultCode::Unreachable ||
			Code == EConvaiMoveToResultCode::MoveFailed;
	}
};

DECLARE_MULTICAST_DELEGATE_OneParam(
	FConvaiMoveToTaskFinishedSignature,
	const FConvaiMoveToResult&);

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FConvaiOwnedMoveFinishedSignature,
	bool,
	int32);

/**
 * Small compatibility adapter around Unreal's Move To AI task.
 *
 * It owns the engine request and exposes the pause, refresh, path, and native
 * completion hooks required by UConvaiMoveToTask across supported Unreal
 * versions. It has no Blueprint-facing API or Convai movement policy.
 */
UCLASS()
class UConvaiMoveOwnedAITask : public UAITask_MoveTo
{
	GENERATED_BODY()

public:
	bool SuspendOwnedRequest();
	bool RefreshOwnedRequest(const FAIMoveRequest& InMoveRequest);
	bool CopyRemainingPathPoints(TArray<FVector>& OutPathPoints) const;

	FConvaiOwnedMoveFinishedSignature OnOwnedMoveFinished;

protected:
	virtual void OnRequestFinished(
		FAIRequestID RequestID,
		const FPathFollowingResult& Result) override;
	virtual void OnDestroy(bool bInOwnerFinished) override;
};

/**
 * Reusable native movement primitive for Convai actions.
 *
 * Resolution, reachability, arrival, moving-target tracking, bounded retry, and
 * failure wording live here so compound and custom movement actions can share
 * one policy. Escort uses this task directly. The task never calls Handle
 * Action Completion; its caller decides how the movement result maps into that
 * action's lifecycle.
 */
UCLASS()
class CONVAI_API UConvaiMoveToTask : public UGameplayTask
{
	GENERATED_BODY()

public:
	UConvaiMoveToTask(const FObjectInitializer& ObjectInitializer);

	/**
	 * Resolves Destination and creates a dormant task when movement is needed.
	 *
	 * Returns null for an immediate outcome (already there or a validation /
	 * reachability failure); Out Immediate Result always explains that outcome.
	 * Parent Task is optional and lets a compound action such as Escort own the
	 * move. Bind On Finished before calling Start.
	 *
	 * Lock AI Logic should normally remain false so an existing Behavior Tree
	 * can keep running. Compound actions that fully own the character's
	 * movement, such as Escort, may opt into the lock.
	 */
	static UConvaiMoveToTask* CreateMoveToTask(
		AActor* MovingCharacter,
		const FConvaiObjectEntry& Destination,
		FConvaiMoveToResult& OutImmediateResult,
		UGameplayTask* ParentTask = nullptr,
		bool bLockAILogic = false);

	/** Activates a task returned by Create Move To Task. Safe to call once. */
	void Start();

	/**
	 * Idempotently stops this task. Native C++ observers receive the internal
	 * Cancelled terminal marker; Blueprint proxies detach before calling this
	 * and therefore expose no cancellation output.
	 */
	virtual void ExternalCancel() override;

	/**
	 * Pauses the owned AI request without releasing its movement resources.
	 * Used by compound behaviors such as Escort while they wait.
	 */
	bool SuspendMovement();

	/**
	 * Fully re-resolves the destination and resumes this task's owned request.
	 * A terminal result may be emitted synchronously.
	 */
	bool ResumeMovement();

	/**
	 * Copies the live remaining path when available, otherwise the path selected
	 * by the latest full Convai resolve before the low-level move starts. Once a
	 * move is active, unsuitable live routes fail closed instead of exposing a
	 * stale path for directional progress checks.
	 */
	bool CopyRemainingPathPoints(TArray<FVector>& OutPathPoints) const;

	FConvaiMoveToTaskFinishedSignature OnFinished;

protected:
	virtual void Activate() override;
	virtual void TickTask(float DeltaTime) override;
	virtual void OnDestroy(bool bInOwnerFinished) override;

private:
	void StartOwnedMove();
	void StopOwnedMove();
	void HandleOwnedMoveFinished(bool bEngineReportedSuccess, int32 EngineResult);
	bool ResolveForMovement(FConvaiMoveToResult& OutImmediateResult);
	bool RefreshTrackedLocation();
	void Finish(const FConvaiMoveToResult& Result);

	UPROPERTY(Transient)
	TObjectPtr<UGameplayTask> ActiveMoveTask = nullptr;

	TWeakObjectPtr<APawn> MovingPawn;
	TWeakObjectPtr<AAIController> AIController;
	TWeakObjectPtr<AActor> ResolvedGoalActor;
	FConvaiObjectEntry DestinationEntry;
	FVector ResolvedGoalLocation = FVector::ZeroVector;
	FVector LastIssuedGoalLocation = FVector::ZeroVector;
	TArray<FVector> ResolvedPathPoints;
	float ResolvedAcceptanceRadius = 0.0f;
	float LocationRefreshElapsed = 0.0f;
	int32 ResolvedMovementPointIndex = INDEX_NONE;
	int32 ReachableMoveRetryCount = 0;
	bool bMoveToLocation = false;
	bool bStartRequested = false;
	bool bMoveSuspended = false;
	bool bShouldLockAILogic = false;
	bool bStoppingOwnedMove = false;
	bool bTerminal = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FConvaiMoveToProxyResultSignature,
	EConvaiMoveToResultCode,
	ResultCode,
	FString,
	AdditionalNote);

/**
 * Blueprint proxy for UConvaiMoveToTask.
 *
 * This object is not a Convai action implementation. It only turns the native
 * task's natural completion into Succeeded / Failed Blueprint pins. The
 * surrounding Blueprint action handler remains responsible for calling Handle
 * Action Completion.
 *
 * Advanced handlers may save Move Request, call Cancel from a separate
 * "Cancel <Action>" event, and then call Handle Action Cancellation. Cancel is
 * synchronous on the game thread, emits no normal output, and is idempotent.
 */
UCLASS(BlueprintType, meta = (ExposedAsyncProxy = "MoveRequest"))
class CONVAI_API UConvaiMoveToProxy : public UConvaiCancellableTaskProxy
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Convai|Movement")
	FConvaiMoveToProxyResultSignature Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Convai|Movement")
	FConvaiMoveToProxyResultSignature Failed;

	/**
	 * Move Moving Actor to Destination.
	 *
	 * Destination may reference a whole actor, a component/socket, or authored
	 * movement points. Setup is validated before the request can report arrival.
	 * Developer diagnostics are logged internally; Additional Note remains safe
	 * for character-facing action completion.
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "Convai|Movement",
		meta = (
			BlueprintInternalUseOnly = "true",
			DisplayName = "Convai Move To",
			WorldContext = "WorldContextObject",
			AdvancedDisplay = "bLockAILogic",
			Keywords = "AI move navigate walk character destination"
		))
	static UConvaiMoveToProxy* ConvaiMoveTo(
		UObject* WorldContextObject,
		UPARAM(DisplayName = "Moving Actor") AActor* MovingCharacter,
		const FConvaiObjectEntry& Destination,
		bool bLockAILogic = false);

	virtual void Activate() override;

protected:
	virtual void BeginDestroy() override;

private:
	void HandleTaskFinished(const FConvaiMoveToResult& Result);
	void QueueProxyResult(const FConvaiMoveToResult& Result);
	void FinishProxy(const FConvaiMoveToResult& Result);
	virtual void ReleaseOwnedTask(bool bCancelTask) override;

	UPROPERTY(Transient)
	TObjectPtr<UConvaiMoveToTask> ActiveTask = nullptr;

	TWeakObjectPtr<AActor> MovingCharacter;
	FConvaiObjectEntry DestinationEntry;
	bool bShouldLockAILogic = false;

#if WITH_DEV_AUTOMATION_TESTS
	friend struct FConvaiMoveToProxyTestAccessor;
#endif
};
