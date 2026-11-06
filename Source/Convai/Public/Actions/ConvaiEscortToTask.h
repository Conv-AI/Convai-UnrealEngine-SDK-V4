// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actions/ConvaiCancellableTaskProxy.h"
#include "ConvaiDefinitions.h"
#include "GameplayTask.h"
#include "Tasks/AITask.h"
#include "ConvaiEscortToTask.generated.h"

class AAIController;
class AActor;
class APawn;
class UConvaiChatbotComponent;
class UConvaiMoveToTask;
struct FConvaiMoveToResult;

/** Why a Convai Escort request ended. */
UENUM(BlueprintType)
enum class EConvaiEscortResultCode : uint8
{
	/** The escorting actor and escorted character reached the destination. */
	Reached = 0 UMETA(DisplayName = "Reached"),
	/** The escort began with both actors already at the destination. */
	AlreadyAtDestination = 6 UMETA(DisplayName = "Already At Destination"),
	/** The character to escort disappeared or is not a valid escort target. */
	EscorteeUnavailable = 1 UMETA(DisplayName = "Character Unavailable"),
	/** The destination disappeared or could not be reached from here. */
	DestinationUnavailable = 2 UMETA(DisplayName = "Destination Unavailable"),
	/** The escorting actor is missing setup required by Escort. */
	InvalidGuideSetup = 3 UMETA(DisplayName = "Invalid Escort Setup"),
	/** Unreal stopped the owned movement before Escort reached its destination. */
	MovementFailed = 4 UMETA(DisplayName = "Movement Failed"),
	/** Native lifecycle marker reported as a failed public request. */
	Cancelled = 5 UMETA(Hidden)
};

/**
 * Terminal result shared by the native Escort task and its Blueprint async node.
 *
 * Additional Note is safe to pass to Handle Action Completion. Developer
 * Details is diagnostic-only, logged by native code, and omitted from the
 * public async Blueprint node.
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiEscortResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Movement")
	EConvaiEscortResultCode Code = EConvaiEscortResultCode::MovementFailed;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Movement")
	FString AdditionalNote;

	UPROPERTY()
	FString DeveloperDetails;

	bool IsSuccess() const
	{
		return Code == EConvaiEscortResultCode::Reached ||
			Code == EConvaiEscortResultCode::AlreadyAtDestination;
	}

	bool ShouldReportToCharacter() const
	{
		return Code == EConvaiEscortResultCode::EscorteeUnavailable ||
			Code == EConvaiEscortResultCode::DestinationUnavailable ||
			Code == EConvaiEscortResultCode::MovementFailed;
	}
};

DECLARE_MULTICAST_DELEGATE_OneParam(
	FConvaiEscortTaskFinishedSignature,
	const FConvaiEscortResult&);

/** Internal resource owner that keeps the guide still while Escort waits. */
UCLASS()
class UConvaiEscortHoldTask : public UAITask
{
	GENERATED_BODY()

public:
	UConvaiEscortHoldTask(const FObjectInitializer& ObjectInitializer);
	virtual void ExternalCancel() override;
};

/**
 * Reusable long-running implementation behind Convai Escort.
 *
 * Escort composes the shared UConvaiMoveToTask with its wait-for-escortee
 * behavior. It pauses only the movement request it owns, asks a lagging
 * character to follow, and resumes once that character catches up or is clearly
 * ahead on the remaining route. The task never calls Handle Action Completion;
 * its caller owns the surrounding Convai action lifecycle.
 */
UCLASS()
class CONVAI_API UConvaiEscortToTask : public UGameplayTask
{
	GENERATED_BODY()

public:
	UConvaiEscortToTask(const FObjectInitializer& ObjectInitializer);

	/**
	 * Creates, but does not activate, an Escort task owned by the chatbot's AI
	 * controller. Returns null for an immediate setup failure and always
	 * describes that outcome in Out Immediate Result.
	 */
	static UConvaiEscortToTask* CreateEscortToTask(
		UConvaiChatbotComponent& Chatbot,
		const FConvaiObjectEntry& EscortedCharacter,
		const FConvaiObjectEntry& Destination,
		FConvaiEscortResult& OutImmediateResult);

	/** Activates a task returned by CreateEscortToTask. Safe to call once. */
	void Start();

	/**
	 * Cooperatively stops Escort. Native C++ observers receive an internal
	 * Cancelled marker; Blueprint proxies detach first and expose no cancel pin.
	 */
	virtual void ExternalCancel() override;

	/** Fires once after the task has stopped all owned work. */
	FConvaiEscortTaskFinishedSignature OnFinished;

protected:
	virtual void Activate() override;
	virtual void TickTask(float DeltaTime) override;
	virtual void OnGameplayTaskDeactivated(UGameplayTask& Task) override;
	virtual void OnDestroy(bool bInOwnerFinished) override;

private:
	enum class EEscortTaskState : uint8
	{
		Created,
		Moving,
		WaitingForEscortee,
		Finished
	};

	void StartOrFinishMovement();
	void HandleMoveFinished(const FConvaiMoveToResult& Result);
	void HandleImmediateMoveResult(const FConvaiMoveToResult& Result);
	EConvaiEscortResultCode MapMoveSuccessCode(
		const FConvaiMoveToResult& Result) const;
	void EnterWaitingForEscortee();
	bool StartOwnedHold();
	void StopOwnedHold();
	void StopOwnedMovement();
	void ClearEscortFocus();
	void WithdrawFollowPrompt();
	static bool IsEscorteeNear(float Distance);
	bool IsEscorteeLagging(float Distance);
	bool IsEscorteeAheadOnRemainingPath();
	static bool IsLocationClearlyAheadOnPath(
		const TArray<FVector>& PathPoints,
		const FVector& GuideLocation,
		const FVector& EscorteeLocation);
	float GetEscorteeDistance() const;
	void CompleteSuccessfully(EConvaiEscortResultCode Code);
	void CompleteWithFailure(
		EConvaiEscortResultCode Code,
		const FString& ModelFeedback,
		const FString& DeveloperDiagnostic);
	void CompleteCancellation();
	void Finish(const FConvaiEscortResult& Result);

	UPROPERTY(Transient)
	TObjectPtr<UConvaiMoveToTask> ActiveMoveTask = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UGameplayTask> ActiveHoldTask = nullptr;

	TWeakObjectPtr<UConvaiChatbotComponent> ChatbotComponent;
	TWeakObjectPtr<AAIController> AIController;
	TWeakObjectPtr<APawn> MovingPawn;
	TWeakObjectPtr<AActor> PreviousGameplayFocusActor;
	FVector PreviousGameplayFocalPoint = FVector::ZeroVector;

	FConvaiObjectEntry EscortedCharacterEntry;
	FConvaiObjectEntry DestinationEntry;
	TArray<FVector> ResolvedPathPoints;

	EEscortTaskState EscortState = EEscortTaskState::Created;
	float LaggingSeconds = 0.0f;
	bool bStartRequested = false;
	bool bFollowPromptSent = false;
	bool bStoppingOwnedMove = false;
	bool bStoppingOwnedHold = false;
	bool bOwnsEscortFocus = false;
	bool bHadPreviousGameplayFocalPoint = false;
	bool bReachedDestinationAfterTravel = false;
	bool bTerminal = false;

#if WITH_DEV_AUTOMATION_TESTS
	friend struct FConvaiEscortToTaskTestAccessor;
#endif
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FConvaiEscortProxyResultSignature,
	EConvaiEscortResultCode,
	ResultCode,
	FString,
	AdditionalNote);

/**
 * Blueprint proxy for UConvaiEscortToTask.
 *
 * This proxy exposes only natural success and failure. It does not implement
 * or acknowledge a Convai action; the surrounding Blueprint handler owns
 * Handle Action Completion.
 *
 * Advanced handlers may save Escort Request, call Cancel from a separate
 * "Cancel <Action>" event, and then call Handle Action Cancellation. Cancel is
 * synchronous on the game thread, silent, and safe to call repeatedly.
 */
UCLASS(BlueprintType, meta = (ExposedAsyncProxy = "EscortRequest"))
class CONVAI_API UConvaiEscortProxy : public UConvaiCancellableTaskProxy
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Convai|Movement")
	FConvaiEscortProxyResultSignature Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Convai|Movement")
	FConvaiEscortProxyResultSignature Failed;

	/**
	 * Escort Escorted Character to Destination.
	 *
	 * Escorting Actor owns the Convai chatbot used for the temporary follow cue
	 * when Escorted Character falls behind. Both object entries retain their
	 * complete Convai object-entry semantics.
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "Convai|Movement",
		meta = (
			BlueprintInternalUseOnly = "true",
			DisplayName = "Convai Escort",
			WorldContext = "WorldContextObject",
			Keywords = "guide accompany follow character destination"
		))
	static UConvaiEscortProxy* ConvaiEscort(
		UObject* WorldContextObject,
		UPARAM(DisplayName = "Escorting Actor") AActor* EscortingActor,
		UPARAM(DisplayName = "Escorted Character")
			const FConvaiObjectEntry& EscortedCharacter,
		const FConvaiObjectEntry& Destination);

	virtual void Activate() override;

protected:
	virtual void BeginDestroy() override;

private:
	void HandleTaskFinished(const FConvaiEscortResult& Result);
	void QueueProxyResult(const FConvaiEscortResult& Result);
	void FinishProxy(const FConvaiEscortResult& Result);
	virtual void ReleaseOwnedTask(bool bCancelTask) override;

	UPROPERTY(Transient)
	TObjectPtr<UConvaiEscortToTask> ActiveTask = nullptr;

	TWeakObjectPtr<AActor> EscortingActor;
	FConvaiObjectEntry EscortedCharacterEntry;
	FConvaiObjectEntry DestinationEntry;

#if WITH_DEV_AUTOMATION_TESTS
	friend struct FConvaiEscortProxyTestAccessor;
#endif
};
