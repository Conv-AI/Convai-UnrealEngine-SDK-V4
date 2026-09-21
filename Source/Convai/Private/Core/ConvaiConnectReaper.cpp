// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Core/ConvaiConnectReaper.h"

#include "../../Convai.h"
#include "ConvaiSubsystem.h"
#include "Utility/Log/ConvaiLogger.h"

#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

#include <convai/convai_client.h>

namespace
{
	/** Four is a headroom number, not a measured one: two PIE clients that each
	 *  restart once, plus one. Past it something is looping, and refusing the
	 *  connect says so instead of spawning threads until the process dies. */
	constexpr int32 kMaxLiveTriples = 4;

	/** Today's parked-destroy rule: the transport's own threads are still
	 *  winding down when Disconnect returns, and the destructor cannot run into
	 *  them. */
	constexpr float kDestroyDelaySeconds = 2.0f;
#if WITH_TESTS
	std::atomic<bool> bHoldTeardownsForTests{false};
#endif

	struct FReaperState
	{
		FCriticalSection Lock;
		int32 LiveTriples = 0;
		/** Attempts per character that have not closed their session yet. */
		TMap<FString, int32> PendingByCharacter;
		/** Pulsed whenever one closes, so waiters re-check. */
		FEvent* Progress = nullptr;
		bool bShuttingDown = false;

		FEvent* GetProgressEvent()
		{
			if (!Progress)
			{
				Progress = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false);
			}
			return Progress;
		}
	};

	FReaperState& State()
	{
		static FReaperState Singleton;
		return Singleton;
	}
}

int32 FConvaiConnectReaper::Capacity()
{
	return kMaxLiveTriples;
}

int32 FConvaiConnectReaper::LiveCount()
{
	FScopeLock Lock(&State().Lock);
	return State().LiveTriples;
}

bool FConvaiConnectReaper::Abandon(FTriple&& Triple)
{
	FReaperState& Reaper = State();
	bool bOverCapacity = false;
	{
		FScopeLock Lock(&Reaper.Lock);
		// Always taken, even past the cap. Refusing would leave the caller
		// destroying the triple inline - joining a connect thread on the game
		// thread, which is the freeze this whole class exists to avoid. The cap
		// stops the next CONNECT (see ConnectSession), not the teardown.
		bOverCapacity = Reaper.LiveTriples >= kMaxLiveTriples;
		++Reaper.LiveTriples;
		if (!Triple.CharacterID.IsEmpty())
		{
			++Reaper.PendingByCharacter.FindOrAdd(Triple.CharacterID);
		}
	}

	if (bOverCapacity)
	{
		CONVAI_LOG(LogConvai, Error,
			TEXT("%d connect attempts are already being torn down; something is restarting the session in a loop."),
			FConvaiConnectReaper::LiveCount());
	}

	// TSharedPtr rather than a moved unique: the lambda has to be copyable.
	TSharedPtr<FTriple> Owned = MakeShared<FTriple>(MoveTemp(Triple));
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Owned]()
	{
#if WITH_TESTS
		while (bHoldTeardownsForTests.load())
		{
			FPlatformProcess::Sleep(0.005f);
		}
#endif
		// Joins the connection thread, which may be inside its POST. This is
		// exactly the wait that used to be on the game thread.
		if (Owned->Thread.IsValid())
		{
			Owned->Thread.Reset();
		}

		if (Owned->Client.IsValid())
		{
			// Closes the server session. The successor's POST waits for this.
			Owned->Client->Disconnect();
		}

		{
			FReaperState& Reaper = State();
			FScopeLock Lock(&Reaper.Lock);
			if (!Owned->CharacterID.IsEmpty())
			{
				if (int32* Pending = Reaper.PendingByCharacter.Find(Owned->CharacterID))
				{
					if (--(*Pending) <= 0)
					{
						Reaper.PendingByCharacter.Remove(Owned->CharacterID);
					}
				}
			}
			if (FEvent* Progress = Reaper.GetProgressEvent())
			{
				Progress->Trigger();
			}
		}

		// The delay is for the transport's own threads to finish winding down.
		// With no client there are none, and holding a slot for two seconds
		// would cap out on nothing.
		if (Owned->Client.IsValid())
		{
			FPlatformProcess::Sleep(kDestroyDelaySeconds);
		}

		{
			FReaperState& Reaper = State();
			FScopeLock Lock(&Reaper.Lock);
			Reaper.LiveTriples = FMath::Max(0, Reaper.LiveTriples - 1);
			if (FEvent* Progress = Reaper.GetProgressEvent())
			{
				Progress->Trigger();
			}
		}
		// Owned dies here: client, shim and thread with it.
	});

	return true;
}

bool FConvaiConnectReaper::HasPendingFor(const FString& CharacterID)
{
	if (CharacterID.IsEmpty())
	{
		return false;
	}
	FScopeLock Lock(&State().Lock);
	return State().PendingByCharacter.Contains(CharacterID);
}

void FConvaiConnectReaper::Shutdown(double WaitSeconds)
{
	FReaperState& Reaper = State();
	{
		FScopeLock Lock(&Reaper.Lock);
		Reaper.bShuttingDown = true;
	}

	const double Deadline = FPlatformTime::Seconds() + WaitSeconds;
	for (;;)
	{
		int32 Live = 0;
		FEvent* Progress = nullptr;
		{
			FScopeLock Lock(&Reaper.Lock);
			Live = Reaper.LiveTriples;
			Progress = Reaper.GetProgressEvent();
		}
		if (Live <= 0)
		{
			break;
		}
		if (FPlatformTime::Seconds() >= Deadline)
		{
			// The transport library must not be unloaded under a live thread,
			// so the handle leaks rather than crashing the process on exit.
			CONVAI_LOG(LogConvai, Warning,
				TEXT("%d connect attempts were still being torn down at shutdown; leaving them alone."), Live);
			break;
		}
		if (Progress)
		{
			Progress->Wait(100u);
		}
	}

	// The event is deliberately NOT returned to the pool: a waiter reads the
	// pointer outside the lock (it has to, or it would hold the lock while
	// waiting), and returning it here would hand a live pointer back for reuse.
	// One event for the process is the cheaper end of that trade.
}

#if WITH_TESTS
void FConvaiConnectReaper::ResetForTests()
{
	FReaperState& Reaper = State();
	FScopeLock Lock(&Reaper.Lock);
	Reaper.LiveTriples = 0;
	Reaper.PendingByCharacter.Empty();
	Reaper.bShuttingDown = false;
}

void FConvaiConnectReaper::HoldTeardownsForTests(bool bHold)
{
	bHoldTeardownsForTests = bHold;
}
#endif
