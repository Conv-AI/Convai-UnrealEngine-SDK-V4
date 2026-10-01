// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace convai
{
	class ConvaiClient;
}

class FConvaiClientListenerShim;
class FConvaiConnectionThread;

/**
 * Where abandoned connect attempts go to die, off the game thread.
 *
 * Tearing one down means joining a connection thread that may be inside a
 * 30-second POST and then a Disconnect the transport allows five seconds for.
 * Doing that on the game thread is what froze the forum's game for up to 171
 * seconds. The attempt is handed over here instead and the caller returns
 * immediately.
 *
 * What the game thread's wait was also doing, silently, is closing the old
 * server session before the new one opened — without that order every restart
 * races its own predecessor for the account's concurrency slot. So the wait is
 * not removed, it is moved: the NEXT attempt's connection thread waits for its
 * predecessor's Disconnect before its own POST, which is a thread nobody is
 * watching.
 *
 * Process-wide on purpose: two PIE clients and two back-to-back PIE runs share
 * one transport library, and the cap has to mean something across all of them.
 */
class CONVAI_API FConvaiConnectReaper
{
public:
	/** One abandoned attempt: the thread that was connecting, the client it was
	 *  connecting, and the listener that client holds. They die together. */
	struct FTriple
	{
		TUniquePtr<FConvaiConnectionThread> Thread;
		TSharedPtr<convai::ConvaiClient> Client;
		TSharedPtr<FConvaiClientListenerShim> Shim;
		FString CharacterID;
	};

	/** Hands an attempt over. False when too many are already in flight, which
	 *  the caller must report as a failed connect rather than pile on. */
	static bool Abandon(FTriple&& Triple);

	/** True while an attempt abandoned for this character has not closed its
	 *  session yet. The successor polls this itself rather than blocking inside
	 *  the reaper: its own Stop() has to stay responsive, and a thread being
	 *  killed can be the one waiting. */
	static bool HasPendingFor(const FString& CharacterID);

	/** How many abandoned attempts are still being torn down. */
	static int32 LiveCount();

	/** At most this many abandoned attempts at once. */
	static int32 Capacity();

	/** Asks the pool to finish and waits up to `WaitSeconds`. A clean
	 *  Disconnect at exit is what frees the server slot, so this one wait is
	 *  deliberate — see ADR-worthy note in PLAN 4e. */
	static void Shutdown(double WaitSeconds);

#if WITH_TESTS
	/** Drops the bookkeeping without touching anything in flight. */
	static void ResetForTests();
	/** Holds every teardown before it starts, so a row can look at the
	 *  bookkeeping of an attempt that is still being torn down. */
	static void HoldTeardownsForTests(bool bHold);
#endif
};
