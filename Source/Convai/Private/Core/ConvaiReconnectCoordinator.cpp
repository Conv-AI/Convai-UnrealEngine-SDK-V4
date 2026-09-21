// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Core/ConvaiReconnectCoordinator.h"

#include "../../Convai.h"
#include "Utility/Log/ConvaiLogger.h"

namespace
{
	/** Reopen Immediately reopens at most once in this long. Placeholder until the soak
	 *  (M-stayWarm). */
	constexpr double kReopenImmediatelyMinIntervalSeconds = 120.0;

	FConvaiReconnectVerdict Make(EConvaiReconnectDecision Decision, const TCHAR* Why,
		EC_DisconnectCause Cause = EC_DisconnectCause::Unknown)
	{
		FConvaiReconnectVerdict Verdict;
		Verdict.Decision = Decision;
		Verdict.Why = Why;
		Verdict.Cause = Cause;
		return Verdict;
	}

	/** Retrying cannot fix any of these, so a retry would only spend the
	 *  player's time and the account's quota discovering that again. */
	bool IsTerminal(EC_DisconnectCause Cause)
	{
		switch (Cause)
		{
		case EC_DisconnectCause::Quota:
		case EC_DisconnectCause::Auth:
		case EC_DisconnectCause::Preflight:
		case EC_DisconnectCause::ConnectRejected:
			return true;
		default:
			return false;
		}
	}
}

FConvaiReconnectVerdict FConvaiReconnectDecider::Decide(const FConvaiReconnectContext& Context)
{
	// Nobody to reconnect for. First, because every later rule assumes there is
	// a player and a component waiting for the session.
	if (!Context.bOwnerAlive)
	{
		return Make(EConvaiReconnectDecision::Stopped, TEXT("the component that wanted this session is gone"));
	}
	if (Context.bDedicatedServer)
	{
		return Make(EConvaiReconnectDecision::Stopped, TEXT("a dedicated server has no player to reconnect for"));
	}

	// The game asked for this one, or replaced it with another character.
	if (Context.Reason == EC_DisconnectReason::Explicit)
	{
		return Make(EConvaiReconnectDecision::Stopped, TEXT("the game stopped this session"));
	}
	if (Context.Cause == EC_DisconnectCause::Superseded)
	{
		return Make(EConvaiReconnectDecision::Stopped, TEXT("a newer Start Session took the connection"));
	}

	// Terminal before the settings check: a project with reconnect off still
	// deserves to be told why its session ended, and Failed carries the cause
	// where Stopped does not.
	if (IsTerminal(Context.Cause))
	{
		return Make(EConvaiReconnectDecision::Failed, TEXT("retrying cannot fix this"), Context.Cause);
	}

	const int32 AttemptsAllowed = EffectiveAttempts(Context);
	if (AttemptsAllowed <= 0)
	{
		// Today's behaviour, and phase 2's default: one Disconnected, nothing
		// retries. Reconnect is opt-in until phase 3 flips it.
		return Make(EConvaiReconnectDecision::Stopped, TEXT("Reconnect Attempts is 0"));
	}

	// An idle close is the server saying nobody is there. Reconnecting straight
	// away would open a session for the same empty room, which is what burned
	// through a customer's concurrency all night.
	if (Context.Reason == EC_DisconnectReason::Idle)
	{
		// Unless the plugin had asked the server to keep it open and heard
		// nothing back: then the close may be the renewal that got lost, and
		// the player may still be there. Once.
		if (Context.bRenewalUnacked && !Context.bOneShotSpent
			&& Context.SecondsSinceActivity < Context.AfkBudgetSeconds)
		{
			FConvaiReconnectVerdict Verdict = Make(EConvaiReconnectDecision::Retry,
				TEXT("an idle close after a renewal the server never acknowledged"));
			Verdict.bOneShot = true;
			Verdict.bImmediate = true;
			return Verdict;
		}
		switch (Context.IdleTimeout)
		{
		case EC_IdleTimeoutBehavior::StayClosed:
			return Make(EConvaiReconnectDecision::Stopped, TEXT("Idle Timeout Behavior is Stay Closed"));
		case EC_IdleTimeoutBehavior::ReopenImmediately:
			if (!Context.bOneShotSpent && Context.SecondsSinceIdleReopen >= kReopenImmediatelyMinIntervalSeconds)
			{
				FConvaiReconnectVerdict Verdict = Make(EConvaiReconnectDecision::Retry,
					TEXT("Idle Timeout Behavior is Reopen Immediately"));
				Verdict.bOneShot = true;
				return Verdict;
			}
			return Make(EConvaiReconnectDecision::Dormant, TEXT("Reopen Immediately already reopened recently"));
		default:
			return Make(EConvaiReconnectDecision::Dormant, TEXT("the server closed an idle session"));
		}
	}

	// The same question the server asks, asked locally: has this player done
	// anything lately? If not, a reconnect is a session nobody is using.
	if (Context.SecondsSinceActivity >= Context.AfkBudgetSeconds)
	{
		return Make(EConvaiReconnectDecision::Dormant, TEXT("the player has been away longer than AFK Time"));
	}

	// Budgets last, so a give-up carries whatever cause the drop had.
	if (Context.AttemptsUsed >= AttemptsAllowed)
	{
		return Make(EConvaiReconnectDecision::Failed, TEXT("out of reconnect attempts"), Context.Cause);
	}
	if (Context.SessionsInWindow >= Context.RollingCap)
	{
		return Make(EConvaiReconnectDecision::Failed, TEXT("too many sessions in too little time"), Context.Cause);
	}

	return Make(EConvaiReconnectDecision::Retry, TEXT("retryable, inside the budget"), Context.Cause);
}

int32 FConvaiReconnectDecider::EffectiveAttempts(const FConvaiReconnectContext& Context)
{
	return FMath::Max(0, Context.AttemptsAllowed);
}

// ---------------------------------------------------------------------------
// The coordinator: state, budgets and timers around the decision above.
// ---------------------------------------------------------------------------

namespace
{
	/** Backoff base and ceiling, from PLAN 1c. Placeholders until the soak. */
	constexpr double kBackoffBaseSeconds = 1.0;
	constexpr double kBackoffCapSeconds = 30.0;

	/** The rolling window the session cap is counted over. */
	constexpr double kRollingWindowSeconds = 600.0;
}

double FConvaiReconnectCoordinator::BackoffSeconds(int32 AttemptIndex, double Random01)
{
	// Doubling, capped, with FULL jitter rather than a fixed delay: twenty
	// clients that dropped together must not all come back in the same second,
	// which is how a server that just recovered gets knocked over again.
	const double Ceiling = FMath::Min(kBackoffCapSeconds,
		kBackoffBaseSeconds * FMath::Pow(2.0, static_cast<double>(FMath::Max(0, AttemptIndex))));
	return FMath::Clamp(Random01, 0.0, 1.0) * Ceiling;
}

void FConvaiReconnectCoordinator::Initialize(FHooks&& InHooks)
{
	Hooks = MoveTemp(InHooks);
	CurrentState = EConvaiReconnectState::Stopped;
	AttemptsThisChain = 0;
	ScheduledHandle = INDEX_NONE;
	bAnnouncedReconnecting = false;
	bFiredNowThisChain = false;
	RecentSessionStarts.Reset();
}

void FConvaiReconnectCoordinator::Shutdown()
{
	if (ScheduledHandle != INDEX_NONE && Hooks.Cancel)
	{
		Hooks.Cancel(ScheduledHandle);
	}
	ScheduledHandle = INDEX_NONE;
	CurrentState = EConvaiReconnectState::Stopped;
}

FConvaiReconnectVerdict FConvaiReconnectCoordinator::Predict(FConvaiReconnectContext Context, EConvaiReconnectTrigger Trigger) const
{
	Context.Trigger = Trigger;
	Context.AttemptsUsed = AttemptsThisChain;
	Context.SessionsInWindow = RecentSessionStarts.Num();
	Context.bOneShotSpent = bOneShotSpent;
	Context.SecondsSinceIdleReopen = (Hooks.Now ? Hooks.Now() : 0.0) - LastIdleReopenAt;
	return FConvaiReconnectDecider::Decide(Context);
}

bool FConvaiReconnectCoordinator::Trigger(EConvaiReconnectTrigger Trigger)
{
	if (!Hooks.Snapshot || !Hooks.StartAttempt || !Hooks.Schedule)
	{
		return false;  // never initialized: behave as if reconnect did not exist
	}

	if (IsEngineExitRequested())
	{
		// Nothing is worth reconnecting into a process that is leaving, and a
		// ticker that fires during teardown outlives the world it needed.
		End(EConvaiReconnectState::Stopped);
		return false;
	}

	FConvaiReconnectContext Context = Hooks.Snapshot();
	Context.Trigger = Trigger;
	Context.AttemptsUsed = AttemptsThisChain;
	Context.SessionsInWindow = RecentSessionStarts.Num();
	Context.bOneShotSpent = bOneShotSpent;
	const double Now = Hooks.Now ? Hooks.Now() : 0.0;
	Context.SecondsSinceIdleReopen = Now - LastIdleReopenAt;

	const FConvaiReconnectVerdict Verdict = FConvaiReconnectDecider::Decide(Context);
	ChainReason = Context.Reason;
	ChainCause = Verdict.Decision == EConvaiReconnectDecision::Failed ? Verdict.Cause : Context.Cause;
	ChainMaxAttempts = FConvaiReconnectDecider::EffectiveAttempts(Context);

	switch (Verdict.Decision)
	{
	case EConvaiReconnectDecision::Retry:
		bChainIsIdleOneShot = Verdict.bOneShot;
		if (Verdict.bOneShot)
		{
			bOneShotSpent = true;
			if (Context.IdleTimeout == EC_IdleTimeoutBehavior::ReopenImmediately && !Verdict.bImmediate)
			{
				LastIdleReopenAt = Now;
				CONVAI_LOG(LogConvai, Display,
					TEXT("Reconnect: Reopen Immediately reopens the idle session once; it holds a concurrency slot while nobody talks"));
			}
		}
		EnterBackoff(Verdict.bImmediate);
		// A handler of the announcement may already have stopped the chain.
		return CurrentState == EConvaiReconnectState::Backoff || CurrentState == EConvaiReconnectState::Connecting;

	case EConvaiReconnectDecision::Dormant:
		End(EConvaiReconnectState::Dormant);
		return false;

	case EConvaiReconnectDecision::Failed:
		FailedCause = Verdict.Cause;
		End(EConvaiReconnectState::Failed);
		return false;

	default:
		End(EConvaiReconnectState::Stopped);
		return false;
	}
}

void FConvaiReconnectCoordinator::EnterBackoff(bool bImmediate)
{
	if (ScheduledHandle != INDEX_NONE && Hooks.Cancel)
	{
		Hooks.Cancel(ScheduledHandle);
		ScheduledHandle = INDEX_NONE;
	}

	CurrentState = EConvaiReconnectState::Backoff;

	// Scheduled before anything is announced: a handler of either event may
	// fire, stop or supersede this backoff, and must find it already there.
	const double Delay = bImmediate ? 0.0 : BackoffSeconds(AttemptsThisChain, FMath::FRand());
	ScheduledHandle = Hooks.Schedule(Delay, [this]() { OnBackoffElapsed(); });
	EmitStatus(EC_ReconnectStatus::Scheduled, AttemptsThisChain + 1, static_cast<float>(Delay));

	// One Reconnecting for the whole chain: a game that shows "reconnecting"
	// does not want it re-announced per attempt, and a game that restarts its
	// session on the event would restart once per attempt.
	if (!bAnnouncedReconnecting && CurrentState == EConvaiReconnectState::Backoff)
	{
		bAnnouncedReconnecting = true;
		if (Hooks.EmitServerState)
		{
			Hooks.EmitServerState(EC_ConnectionState::Reconnecting);
		}
	}
}

void FConvaiReconnectCoordinator::OnBackoffElapsed()
{
	ScheduledHandle = INDEX_NONE;
	if (CurrentState != EConvaiReconnectState::Backoff || !Hooks.Snapshot)
	{
		return;
	}

	// The player can walk away during a long outage. Reopening then is a
	// session nobody uses: the chain ends Dormant, and the game is told the
	// session ended, once.
	//
	// Not for the one-off reopen of an idle close: the decider judged that one
	// without this rule on purpose. ShouldRenewIdleTimer only lets a session
	// idle out once Elapsed + remaining >= AFK Time, so the budget is always
	// spent when the close lands - re-asking here means it can never reopen.
	const FConvaiReconnectContext Context = Hooks.Snapshot();
	if (!bChainIsIdleOneShot && Context.SecondsSinceActivity >= Context.AfkBudgetSeconds)
	{
		// The ending first, stamped with this session's epoch: a Dormant
		// handler that starts a new session must not have that session
		// announced as ended.
		if (Hooks.EmitServerState)
		{
			Hooks.EmitServerState(EC_ConnectionState::Disconnected);
		}
		End(EConvaiReconnectState::Dormant);
		return;
	}

	BeginAttempt();
}

void FConvaiReconnectCoordinator::BeginAttempt()
{
	ScheduledHandle = INDEX_NONE;
	if (IsEngineExitRequested())
	{
		CurrentState = EConvaiReconnectState::Stopped;
		return;
	}
	if (CurrentState != EConvaiReconnectState::Backoff)
	{
		// Stopped, woken or superseded while the backoff was running.
		return;
	}

	++AttemptsThisChain;
	CurrentState = EConvaiReconnectState::Connecting;

	if (Hooks.Now)
	{
		const double Now = Hooks.Now();
		RecentSessionStarts.RemoveAll([Now](double At) { return Now - At > kRollingWindowSeconds; });
		RecentSessionStarts.Add(Now);
	}

	EmitStatus(EC_ReconnectStatus::Attempting, AttemptsThisChain);
	// A handler of that status may have stopped the chain.
	if (CurrentState != EConvaiReconnectState::Connecting)
	{
		return;
	}
	Hooks.StartAttempt();
}

bool FConvaiReconnectCoordinator::Wake()
{
	switch (CurrentState)
	{
	case EConvaiReconnectState::Backoff:
		// The player is back and waiting; whatever is left of the backoff is
		// time they spend looking at a character that does not answer.
		FireNow();
		return false;

	case EConvaiReconnectState::Failed:
	{
		// A chain that gave up on something that can pass - the network, the
		// budget - gets a fresh one when the player is back. One that gave up
		// on quota or a missing key would only find the same thing again.
		if (IsTerminal(FailedCause) || !Hooks.Snapshot)
		{
			return false;
		}
		const double Now = Hooks.Now ? Hooks.Now() : 0.0;
		RecentSessionStarts.RemoveAll([Now](double At) { return Now - At > kRollingWindowSeconds; });
		if (RecentSessionStarts.Num() >= Hooks.Snapshot().RollingCap)
		{
			return false;
		}
		AttemptsThisChain = 0;
		bAnnouncedReconnecting = false;
		BeginAttemptFromWake();
		return true;
	}

	case EConvaiReconnectState::Dormant:
		bAnnouncedReconnecting = false;
		BeginAttemptFromWake();
		return true;

	default:
		// Ready, Connecting and Stopped ignore activity: there is either
		// nothing to do or nothing that activity can fix.
		return false;
	}
}

bool FConvaiReconnectCoordinator::HasFailedTerminally() const
{
	return CurrentState == EConvaiReconnectState::Failed && IsTerminal(FailedCause);
}

bool FConvaiReconnectCoordinator::FireNow()
{
	switch (CurrentState)
	{
	case EConvaiReconnectState::Backoff:
		if (!bFiredNowThisChain)
		{
			bFiredNowThisChain = true;
			if (ScheduledHandle != INDEX_NONE && Hooks.Cancel)
			{
				Hooks.Cancel(ScheduledHandle);
				ScheduledHandle = INDEX_NONE;
			}
			BeginAttemptFromWake();
		}
		return true;

	case EConvaiReconnectState::Connecting:
		return true;

	default:
		return false;
	}
}

void FConvaiReconnectCoordinator::BeginAttemptFromWake()
{
	// A wake opens straight away: Attempting with no Scheduled before it.
	if (Hooks.Snapshot && CurrentState != EConvaiReconnectState::Backoff)
	{
		ChainMaxAttempts = FConvaiReconnectDecider::EffectiveAttempts(Hooks.Snapshot());
	}
	CurrentState = EConvaiReconnectState::Backoff;
	BeginAttempt();
}

void FConvaiReconnectCoordinator::Stop(bool bAnnounceEnding)
{
	if (ScheduledHandle != INDEX_NONE && Hooks.Cancel)
	{
		Hooks.Cancel(ScheduledHandle);
		ScheduledHandle = INDEX_NONE;
	}

	// Only a backoff owes the game its ending: nothing else is left to report
	// it. An attempt in flight is torn down by the same Stop Session, and that
	// teardown announces the session's end itself. Dormant and Failed already
	// ended their session and said so.
	const bool bOwedAnEnding = bAnnounceEnding && CurrentState == EConvaiReconnectState::Backoff;
	const bool bWasActive = IsChainActive();
	FConvaiReconnectStatus Status;
	Status.Status = EC_ReconnectStatus::GaveUp;
	Status.Attempt = AttemptsThisChain;
	Status.MaxAttempts = ChainMaxAttempts;
	Status.Reason = EC_DisconnectReason::Explicit;
	Status.Cause = ChainCause;

	CurrentState = EConvaiReconnectState::Stopped;
	AttemptsThisChain = 0;
	bAnnouncedReconnecting = false;
	bOneShotSpent = false;
	bChainIsIdleOneShot = false;
	bFiredNowThisChain = false;

	if (bWasActive && Hooks.EmitStatus)
	{
		Hooks.EmitStatus(Status);
	}
	if (bOwedAnEnding && Hooks.EmitServerState)
	{
		Hooks.EmitServerState(EC_ConnectionState::Disconnected);
	}
}

void FConvaiReconnectCoordinator::NotifyReady()
{
	if (ScheduledHandle != INDEX_NONE && Hooks.Cancel)
	{
		Hooks.Cancel(ScheduledHandle);
	}
	ScheduledHandle = INDEX_NONE;
	const int32 Attempts = AttemptsThisChain;
	CurrentState = EConvaiReconnectState::Ready;
	// Something worked, so the budget for this chain is spent: a session that
	// runs for hours and drops twice must not be measured against one budget.
	AttemptsThisChain = 0;
	bAnnouncedReconnecting = false;
	bOneShotSpent = false;
	bChainIsIdleOneShot = false;
	bFiredNowThisChain = false;
	// Last, with the chain already over: a handler that stops the session
	// ends a Ready session, not a second chain. A first session says nothing.
	if (Attempts > 0)
	{
		EmitStatus(EC_ReconnectStatus::Reconnected, Attempts);
	}
}

void FConvaiReconnectCoordinator::OnAttemptFailed()
{
	Trigger(EConvaiReconnectTrigger::AttemptFailed);
}

void FConvaiReconnectCoordinator::OnBotLeft()
{
	if (CurrentState != EConvaiReconnectState::Ready || !Hooks.Snapshot || !Hooks.Schedule)
	{
		return;
	}
	const FConvaiReconnectContext Context = Hooks.Snapshot();
	if (!Context.bOwnerAlive || FConvaiReconnectDecider::EffectiveAttempts(Context) <= 0)
	{
		return;
	}

	// A full transport restart makes every participant leave and rejoin, and
	// looks exactly like the character leaving. Waiting out the window is what
	// tells the two apart.
	CurrentState = EConvaiReconnectState::BotAbsent;
	ScheduledHandle = Hooks.Schedule(Context.BotAbsenceWindowSeconds, [this]()
	{
		ScheduledHandle = INDEX_NONE;
		if (CurrentState == EConvaiReconnectState::BotAbsent && Hooks.BotAbsenceExpired)
		{
			Hooks.BotAbsenceExpired();
		}
	});
}

void FConvaiReconnectCoordinator::OnBotReturned()
{
	if (CurrentState != EConvaiReconnectState::BotAbsent)
	{
		return;
	}
	if (ScheduledHandle != INDEX_NONE && Hooks.Cancel)
	{
		Hooks.Cancel(ScheduledHandle);
	}
	ScheduledHandle = INDEX_NONE;
	CurrentState = EConvaiReconnectState::Ready;
}

bool FConvaiReconnectCoordinator::IsChainActive() const
{
	return CurrentState == EConvaiReconnectState::Backoff || CurrentState == EConvaiReconnectState::Connecting
		|| AttemptsThisChain > 0;
}

void FConvaiReconnectCoordinator::EmitStatus(EC_ReconnectStatus Status, int32 Attempt, float DelaySeconds) const
{
	if (!Hooks.EmitStatus)
	{
		return;
	}
	FConvaiReconnectStatus Out;
	Out.Status = Status;
	Out.Attempt = Attempt;
	Out.MaxAttempts = ChainMaxAttempts;
	Out.DelaySeconds = DelaySeconds;
	Out.Reason = ChainReason;
	Out.Cause = ChainCause;
	Hooks.EmitStatus(Out);
}

void FConvaiReconnectCoordinator::End(EConvaiReconnectState FinalState)
{
	if (ScheduledHandle != INDEX_NONE && Hooks.Cancel)
	{
		Hooks.Cancel(ScheduledHandle);
		ScheduledHandle = INDEX_NONE;
	}
	// Exactly one ending per chain. A Stopped with no chain behind it - a
	// drop with reconnect off - is not a reconnect giving up.
	const int32 Attempts = AttemptsThisChain;
	const bool bGaveUp = FinalState == EConvaiReconnectState::Failed
		|| (FinalState == EConvaiReconnectState::Stopped && IsChainActive());
	CurrentState = FinalState;
	AttemptsThisChain = 0;
	bAnnouncedReconnecting = false;
	bOneShotSpent = false;
	bChainIsIdleOneShot = false;
	bFiredNowThisChain = false;
	// After the state is final, so a handler that starts or ensures a session
	// finds the chain already ended.
	if (FinalState == EConvaiReconnectState::Dormant)
	{
		EmitStatus(EC_ReconnectStatus::Dormant, Attempts);
	}
	else if (bGaveUp)
	{
		EmitStatus(EC_ReconnectStatus::GaveUp, Attempts);
	}
}
