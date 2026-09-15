// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Actions/ConvaiCancellableTaskProxy.h"

bool UConvaiCancellableTaskProxy::Cancel()
{
	if (!ensureMsgf(
			IsInGameThread(),
			TEXT("Convai task proxies must be cancelled on the game thread.")))
	{
		return false;
	}

	if (bTerminal || bNaturalCompletionPending)
	{
		return false;
	}

	// Latch before stopping the owned task. Engine teardown may synchronously
	// report completion, which must not escape as a second acknowledgement.
	bTerminal = true;
	ReleaseOwnedTask(/*bCancelTask*/ true);
	SetReadyToDestroy();
	return true;
}

void UConvaiCancellableTaskProxy::BeginDestroy()
{
	// Never dispatch to ReleaseOwnedTask here. By the time this base
	// BeginDestroy runs, the concrete proxy may already have left its derived
	// destruction phase, making a pure-virtual call fatal. Concrete proxies
	// detach their typed tasks in their own BeginDestroy overrides.
	bTerminal = true;
	Super::BeginDestroy();
}

bool UConvaiCancellableTaskProxy::TryBeginActivation()
{
	if (bActivated || bTerminal)
	{
		return false;
	}

	bActivated = true;
	return true;
}

bool UConvaiCancellableTaskProxy::TryReserveNaturalCompletion()
{
	if (bTerminal || bNaturalCompletionPending)
	{
		return false;
	}

	bNaturalCompletionPending = true;
	return true;
}

bool UConvaiCancellableTaskProxy::TryClaimNaturalCompletion()
{
	if (bTerminal || !bNaturalCompletionPending)
	{
		return false;
	}

	bNaturalCompletionPending = false;
	bTerminal = true;
	return true;
}
