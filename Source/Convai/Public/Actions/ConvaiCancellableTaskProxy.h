// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "ConvaiCancellableTaskProxy.generated.h"

/**
 * Shared lifecycle contract for cancellable Convai Blueprint task proxies.
 *
 * Concrete proxies keep their own typed inputs, results, and Gameplay Tasks.
 * This base owns only the cancellation-versus-completion race so every
 * long-running Blueprint request follows the same acknowledgement contract.
 */
UCLASS(Abstract, BlueprintType)
class CONVAI_API UConvaiCancellableTaskProxy
	: public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/**
	 * Attempts to synchronously and silently stop this request.
	 *
	 * True means cancellation won the terminal race and the caller may
	 * acknowledge its cancellation. False means a natural completion already
	 * owns acknowledgement, or this request was already cancelled.
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "Convai|Movement",
		meta = (ReturnDisplayName = "Cancellation Succeeded"))
	bool Cancel();

protected:
	virtual void BeginDestroy() override;

	/** Claims this proxy's one allowed activation. */
	bool TryBeginActivation();

	/** Reserves the terminal path for a deferred natural completion. */
	bool TryReserveNaturalCompletion();

	/** Converts a reserved natural completion into the terminal state. */
	bool TryClaimNaturalCompletion();

	/** Detaches from the concrete task and optionally cancels it. */
	virtual void ReleaseOwnedTask(bool bCancelTask) PURE_VIRTUAL(
		UConvaiCancellableTaskProxy::ReleaseOwnedTask, );

	// Protected so focused automation accessors on concrete proxies can verify
	// the shared race without exposing lifecycle state to Blueprint.
	bool bActivated = false;
	bool bNaturalCompletionPending = false;
	bool bTerminal = false;
};
