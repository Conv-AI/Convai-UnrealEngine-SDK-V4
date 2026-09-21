// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiDefinitions.h"

/**
 * What made the coordinator look at a session.
 *
 * Separate from the disconnect reason because two of these — a bot-ready
 * timeout and a microphone the server never heard — happen while the transport
 * is still up, and a decision that could not tell them from a drop would leave
 * a live client behind.
 */
enum class EConvaiReconnectTrigger : uint8
{
	/** The transport reported the session ended. */
	Drop,
	/** A connect attempt never reached a session. */
	AttemptFailed,
	/** Connected, but the character never became ready. */
	BotReadyTimeout,
	/** The server reported an error it marked fatal. */
	FatalError,
	/** The player has been speaking and the server has heard nothing. */
	MicUnheard,
	/** The character's own participant left a transport that is still up. */
	BotLeft,
};

/** What the coordinator does next. */
enum class EConvaiReconnectDecision : uint8
{
	/** Schedule another attempt. */
	Retry,
	/** Stop, but come back on Player Activity. */
	Dormant,
	/** Stop, and say why. Only a new Visit starts this session again. */
	Failed,
	/** Stop. Nothing is owed to anybody. */
	Stopped,
};

/**
 * Everything the decision needs, snapshotted on the game thread.
 *
 * A struct rather than a pile of arguments because the decision is a pure
 * function of it: given this, the answer is always the same, which is what
 * makes one unit row per matrix line possible without a world or a network.
 */
struct CONVAI_API FConvaiReconnectContext
{
	EConvaiReconnectTrigger Trigger = EConvaiReconnectTrigger::Drop;

	/** The verdict on the session that ended, already final (slice 06). */
	EC_DisconnectReason Reason = EC_DisconnectReason::Unexpected;

	/** Why, when the transport can say. Unknown until phase 3 classifies the
	 *  DLL's reason string and the /connect status. */
	EC_DisconnectCause Cause = EC_DisconnectCause::Unknown;

	/** Project Settings ▸ Reconnect Attempts. 0 disables reconnect entirely. */
	int32 AttemptsAllowed = 0;

	/** Attempts already spent on THIS drop chain. Reset at bot-ready. */
	int32 AttemptsUsed = 0;

	/** Sessions opened in the rolling window, against its cap. The per-chain
	 *  budget bounds one bad drop; this bounds a game that keeps dropping. */
	int32 SessionsInWindow = 0;
	int32 RollingCap = 10;

	/** Seconds since the player last did anything, against the AFK budget.
	 *  Both belong to the Visit, not to a connection (slice 07). */
	double SecondsSinceActivity = 0.0;
	double AfkBudgetSeconds = 600.0;


	/** False once the component that wanted this session is gone: a level
	 *  unloaded, an actor destroyed, the game shutting down. */
	bool bOwnerAlive = true;

	/** A dedicated server has no player to reconnect for. */
	bool bDedicatedServer = false;

	/** Project Settings ▸ Idle Timeout Behavior. */
	EC_IdleTimeoutBehavior IdleTimeout = EC_IdleTimeoutBehavior::ReopenWhenPlayerReturns;

	/** The last renewal of the server's idle timer went out and was never
	 *  acknowledged, so an idle close may be one the plugin tried to prevent. */
	bool bRenewalUnacked = false;

	/** A one-off reopen (Reopen Immediately, or the unacked renewal) was already spent
	 *  on this idle close. */
	bool bOneShotSpent = false;

	/** Since the last Reopen Immediately reopen, so a kiosk that idles out every few
	 *  minutes cannot hold a slot open by reopening each time. */
	double SecondsSinceIdleReopen = TNumericLimits<double>::Max();

	/** How long the character's participant may be gone from a transport that
	 *  is still up before that counts as the character having left. */
	double BotAbsenceWindowSeconds = 10.0;
};

/** The answer, and what to tell the game. */
struct CONVAI_API FConvaiReconnectVerdict
{
	EConvaiReconnectDecision Decision = EConvaiReconnectDecision::Stopped;

	/** Set when Decision is Failed. */
	EC_DisconnectCause Cause = EC_DisconnectCause::Unknown;

	/** Why this verdict, in words, for the one Display line per attempt. */
	const TCHAR* Why = TEXT("");

	/** A single reopen, not the start of a retry chain: a second close of the
	 *  same kind goes Dormant. */
	bool bOneShot = false;

	/** Retry without waiting out a backoff. */
	bool bImmediate = false;
};

/**
 * The whole reconnect decision, as one pure function.
 *
 * It owns no state and touches no engine subsystem on purpose: every row of the
 * plan's decision matrix is a unit test over this, with no world, no network
 * and no timing. The coordinator around it owns the clock, the timers and the
 * side effects.
 */
struct CONVAI_API FConvaiReconnectDecider
{
	static FConvaiReconnectVerdict Decide(const FConvaiReconnectContext& Context);

	/** How many attempts this character gets per drop once its policy is
	 *  applied. 0 means reconnect is off for it, which is also what decides
	 *  whether a live session is torn down to be reconnected at all. */
	static int32 EffectiveAttempts(const FConvaiReconnectContext& Context);
};

/** Where a session is, as far as reconnect is concerned. */
enum class EConvaiReconnectState : uint8
{
	/** Nothing is owed: no session, nothing scheduled. */
	Stopped,
	/** An attempt is in flight. */
	Connecting,
	/** A session is up and the character is ready. */
	Ready,
	/** Waiting out a backoff before the next attempt. */
	Backoff,
	/** Not connected on purpose. Player Activity brings it back. */
	Dormant,
	/** Given up. Only a new Visit starts this session again. */
	Failed,
	/** Ready, but the character's participant has left a transport that is
	 *  still up. A full transport restart does exactly this and comes back;
	 *  past the window it is treated as the character having left. */
	BotAbsent,
};

/**
 * The one thing that schedules reconnect attempts.
 *
 * It owns the state, the budgets and the timers; the decision itself is the
 * pure function above. Everything it touches outside itself goes through hooks,
 * so a unit row can drive a whole drop chain with no world, no network and no
 * real clock — and so the production wiring (FTSTicker, ConnectSession, the
 * Blueprint events) has exactly one place it can be wrong.
 *
 * Game thread only.
 */
class CONVAI_API FConvaiReconnectCoordinator
{
public:
	struct FHooks
	{
		/** Wall clock in seconds. Wall clock on purpose: a paused game is not
		 *  a player who went away, and a retry must still happen. */
		TFunction<double()> Now;

		/** Everything the decision needs, read on the game thread. */
		TFunction<FConvaiReconnectContext()> Snapshot;

		/** Opens a session again. The coordinator never connects anything
		 *  itself; this is the subsystem's ConnectSession with the character
		 *  and proxy the drop chain started with. */
		TFunction<void()> StartAttempt;

		/** Runs `Work` after `DelaySeconds`. Returns a handle the coordinator
		 *  can cancel. In production this is an FTSTicker; in a unit row it is
		 *  a queue the test drains itself. */
		TFunction<int32(double DelaySeconds, TFunction<void()> Work)> Schedule;
		TFunction<void(int32 Handle)> Cancel;

		/** Tells the game what happened: Reconnecting once per drop chain,
		 *  Disconnected once per session end. */
		TFunction<void(EC_ConnectionState)> EmitServerState;

		/** The Bot Absence Window ran out. The subsystem ends the live session
		 *  the way a drop does, then triggers with BotLeft. */
		TFunction<void()> BotAbsenceExpired;

		/** Where the chain is, for a game that shows it. */
		TFunction<void(const FConvaiReconnectStatus&)> EmitStatus;
	};

	void Initialize(FHooks&& InHooks);

	/** Cancels everything pending. Safe to call twice. */
	void Shutdown();

	/**
	 * Something happened that might need a reconnect.
	 * @return true when a retry is scheduled, so the caller reports
	 *         Reconnecting rather than Disconnected.
	 */
	bool Trigger(EConvaiReconnectTrigger Trigger);

	/** What Trigger would decide now, without deciding it: the chain's own
	 *  inputs (budget spent, one-shot, Reopen Immediately clock) folded into the
	 *  caller's snapshot. */
	FConvaiReconnectVerdict Predict(FConvaiReconnectContext Context, EConvaiReconnectTrigger Trigger) const;

	/** The player did something. Wakes a Dormant session, or one that gave up
	 *  for a reason that can pass, and fires a pending backoff now rather than
	 *  waiting it out.
	 *  @return true when this starts a new chain - a new Visit. */
	bool Wake();

	/** Something wants the pending attempt now: a Start Session adopted into
	 *  it, or a player waking it. Fires a backoff at most once per drop chain, so
	 *  a game calling it every frame cannot turn the backoff into a hammer.
	 *  @return true when an attempt is running or was just started. */
	bool FireNow();

	/** Whether the game has already been told this chain is Reconnecting. A
	 *  wake from Dormant is a new Visit and announces Connecting instead. */
	bool IsChainAnnounced() const { return bAnnouncedReconnecting; }

	/** The game stopped this session, or its owner went away. The caller that
	 *  announces the ending itself passes false. */
	void Stop(bool bAnnounceEnding = true);

	/** A session reached bot-ready. The budget for this drop chain is spent
	 *  only until something works. */
	void NotifyReady();

	/** An attempt the coordinator started never reached a session. */
	void OnAttemptFailed();

	/** The character's participant left while the transport stayed up. Opens
	 *  the Bot Absence Window when this session would be reconnected; does
	 *  nothing otherwise, which is what happened before reconnect existed. */
	void OnBotLeft();

	/** The participant is back inside the window. */
	void OnBotReturned();

	EConvaiReconnectState State() const { return CurrentState; }
	/** Why the chain gave up. Meaningful only in Failed. */
	EC_DisconnectCause GetFailedCause() const { return FailedCause; }
	/** Failed on something another try would only find again: quota, a
	 *  missing key. */
	bool HasFailedTerminally() const;
	int32 AttemptsUsed() const { return AttemptsThisChain; }

	/** Backoff for the n-th attempt, in seconds: one second doubling to a cap,
	 *  with full jitter. Exposed so a test can assert the shape without
	 *  waiting. `Random01` is the jitter draw. */
	static double BackoffSeconds(int32 AttemptIndex, double Random01);

private:
	void EnterBackoff(bool bImmediate = false);
	void OnBackoffElapsed();
	void BeginAttemptFromWake();
	void BeginAttempt();
	void End(EConvaiReconnectState FinalState);
	void EmitStatus(EC_ReconnectStatus Status, int32 Attempt, float DelaySeconds = 0.0f) const;
	/** A chain the game has been told about: a retry pending or in flight. */
	bool IsChainActive() const;

	FHooks Hooks;
	EConvaiReconnectState CurrentState = EConvaiReconnectState::Stopped;
	int32 AttemptsThisChain = 0;
	int32 ScheduledHandle = INDEX_NONE;
	/** One Reconnecting per drop chain, however many attempts it takes. */
	bool bAnnouncedReconnecting = false;
	/** The one-off reopen of an idle close is spent until a session works. */
	bool bOneShotSpent = false;
	/** This chain is the one-off reopen of an idle close, so the backoff does
	 *  not re-ask the AFK question the decider already answered for it. */
	bool bChainIsIdleOneShot = false;
	/** A backoff fired early once this chain; later requests join it. */
	bool bFiredNowThisChain = false;
	double LastIdleReopenAt = -TNumericLimits<double>::Max();
	/** What this chain is about, for its status. */
	EC_DisconnectReason ChainReason = EC_DisconnectReason::Unexpected;
	EC_DisconnectCause ChainCause = EC_DisconnectCause::Unknown;
	int32 ChainMaxAttempts = 0;
	/** Why the chain gave up: a wake retries only what can pass. */
	EC_DisconnectCause FailedCause = EC_DisconnectCause::Unknown;
	/** Session starts inside the rolling window, oldest first. */
	TArray<double> RecentSessionStarts;
};
