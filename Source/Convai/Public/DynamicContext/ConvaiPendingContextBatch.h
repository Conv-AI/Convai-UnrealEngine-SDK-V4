// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiDefinitions.h"
#include "Environment/ConvaiEnvironment.h"

struct FConvaiDynamicContextTracker;

/**
 * Aggregates state updates and events arriving within a debounce window
 * before they are sent to the remote context.  Owned by
 * UConvaiChatbotComponent as a plain C++ member.
 *
 * StageState writes the new value through to the tracker immediately
 * (tracker is always current), and also snapshots the pre-batch value so
 * the flush can produce a "Key is now Y (was X)" delta line.
 *
 * Rules:
 *   - Staging the same state key multiple times overwrites the tracker value
 *     but normally keeps the original pre-batch snapshot. Internal transition
 *     producers may supply an observed previous value (last edge wins) so a
 *     coalesced round trip still describes the real final transition.
 *   - Staging the same event string multiple times deduplicates (first
 *     occurrence wins for ordering).
 *   - AggregateRunLLM is upgraded to the maximum rank seen: Always > Auto > Never.
 */
struct CONVAI_API FConvaiPendingContextBatch
{
	// ── Staged state (insertion-ordered, deduped via Contains) ────────
	TArray<FString> StagedStateOrder;

	// Prior value rendered in the delta for each staged key; normally the
	// pre-batch snapshot. Internal transition producers may replace it with the
	// immediately preceding observed state when several edges coalesce.
	TMap<FString, FString> OldValues;

	// Immutable pre-batch baseline. Kept separate from OldValues so display-only
	// transition history cannot corrupt net-no-op or first-appearance decisions.
	// A missing key means the state did not exist before this batch.
	TMap<FString, FString> InitialValues;

	// Sticky until flush/drop: once an edge is declared significant, a later
	// write in the same batch cannot collapse the excursion away.
	TSet<FString> PreserveTransitionKeys;

	// Internal producer hint: delta lines for these keys say only "is now Y",
	// even when a short prior value exists. Not exposed through Blueprint APIs.
	TSet<FString> OmitPreviousValueKeys;

	// Per-item response ranks let key removal recompute the aggregate instead of
	// leaving behind a response requested by a staged value that was cancelled.
	TMap<FString, EC_RunLLMOption> StateRespond;
	// Sticky only for an explicit one-shot Watch Property promotion. If a newer
	// same-key value withdraws this pending item, its held/replacement value must
	// inherit Always so the already-consumed watch still fulfils its promise.
	TSet<FString> WatchPromotedStateKeys;

	// ── Staged declarative facts (insertion-ordered, deduped) ────────
	// Staged keys mark the batch as having work; the sentences themselves
	// always render from the tracker's canonical slot (no delta lines).
	TArray<FString> StagedDeclarativeOrder;
	// Immutable pre-batch declarative baseline. A missing key means the fact
	// first appeared in this batch.
	TMap<FString, FString> InitialDeclarativeValues;
	TMap<FString, EC_RunLLMOption> DeclarativeRespond;

	// ── Staged events (deduped by string) ─────────────────────────────
	TArray<FString> EventsOrdered;
	TSet<FString> EventsSet;
	TMap<FString, EC_RunLLMOption> EventRespond;

	// ── Staged EPHEMERAL (one-flush) events (deduped by string) ───────
	// Emitted in the next context update EXACTLY ONCE, then dropped on
	// ClearStaged. Unlike normal events these are NEVER committed to the
	// canonical tracker, so they never reappear on later updates — as if the
	// event was never added. Used for transient cues (e.g. "X is paying
	// attention to Y") that should nudge the AI once without piling up.
	TArray<FString> EphemeralEventsOrdered;
	TSet<FString> EphemeralEventsSet;
	TMap<FString, EC_RunLLMOption> EphemeralEventRespond;

	// ── Aggregate ShouldRespond across all staged items ───────────────
	EC_RunLLMOption AggregateRunLLM = EC_RunLLMOption::Never;

	// Force a canonical Replace on next flush even if no state/event is staged.
	// Set by RemoveContextState so the flush sees the updated tracker.
	bool bForceReplace = false;

	// Send a Reset command on next flush after draining the staged batch.
	// Set by ResetDynamicContext.
	bool bPendingReset = false;

	// ── Attention slot ───────────────────────────────────────────────
	// Last write wins within the debounce window. Multiple SetObjectInAttention
	// calls collapse to one attention update on flush; the optional Text becomes
	// a single canonical-context event (also last-wins).

	bool bHasPendingAttention = false;
	FConvaiObjectEntry PendingAttentionObject;
	FString PendingAttentionText;
	EC_RunLLMOption PendingAttentionRunLLM = EC_RunLLMOption::Auto;

	bool IsEmpty() const;
	bool HasWork() const;
	bool HasNonNever() const;
	bool HasStagedWork() const;

	/** Clears staged state/events/attention and control flags (but NOT bPendingReset). */
	void ClearStaged();

	/** Clears everything including bPendingReset. */
	void Clear();

	/** Returns true when the key remains staged; false for a net no-op. */
	bool StageState(FConvaiDynamicContextTracker& Tracker, const FString& Name,
		const FString& Value, EC_RunLLMOption ShouldRespond,
		bool bOmitPreviousValue = false,
		const FString* PreviousValueOverride = nullptr,
		bool bPreserveTransition = false);
	void StageDeclarative(FConvaiDynamicContextTracker& Tracker, const FString& Key, const FString& Sentence, EC_RunLLMOption ShouldRespond);
	void StageEvent(const FString& Text, EC_RunLLMOption ShouldRespond, bool bEphemeral = false);
	void StageAttention(const FConvaiObjectEntry& Object, const FString& Text, EC_RunLLMOption ShouldRespond);
	void DropStateKey(const FString& Name);
	void DropDeclarativeKey(const FString& Name);
	void DropEvent(const FString& Text, bool bEphemeral);

	/**
	 * Withdraws an older staged value before a newer same-key call is held for
	 * idle delivery. Restores the canonical tracker to the pre-batch baseline,
	 * or removes the key when the staged value was its first appearance, then
	 * drops every piece of per-key batch metadata and response rank.
	 *
	 * Returns true only when the key was staged and withdrawn.
	 */
	bool WithdrawStateForHold(FConvaiDynamicContextTracker& Tracker,
		const FString& Name, bool* bOutWatchPromoted = nullptr);
	bool WithdrawDeclarativeForHold(FConvaiDynamicContextTracker& Tracker, const FString& Name);

	/** True when this state key itself (not merely another batch item) may run the LLM. */
	bool StateRequestsResponse(const FString& Name) const;
	void MergeRunLLM(EC_RunLLMOption In);

private:
	void RecomputeAggregateRunLLM();
};

/**
 * Context calls whose Delivery is "Wait Until Conversation Is Idle", parked
 * until the owning chatbot's conversation goes idle (or the max-wait deadline
 * passes). Owned by UConvaiChatbotComponent as a plain C++ member.
 *
 * Unlike FConvaiPendingContextBatch this NEVER touches the canonical tracker —
 * a held call has not happened yet. On release the chatbot re-issues each held
 * call through the normal public API (with Delivery = Send Normally), so all
 * staging, dedup and debug side-effects fire at the moment the update actually
 * lands.
 *
 * Rules (mirroring FConvaiPendingContextBatch semantics):
 *   - States and facts are last-wins per key on VALUE, max-rank on ShouldRespond
 *     (Always > Auto > Never — matching the ordinary batch's aggregate). A
 *     NORMAL (non-held) write or a remove for the same key drops the held entry
 *     — the held call is older and must not roll the value backwards when
 *     released.
 *   - Events dedup by (text, ephemerality) — a persistent "X" and an ephemeral
 *     "X" are distinct, exactly as in the ordinary batch — and a duplicate
 *     rank-merges ShouldRespond. A NORMAL event drops an identical older held
 *     event so the tracker can't receive the same persistent event twice.
 *   - Attention is a last-wins slot on object/text, max-rank on ShouldRespond;
 *     any newer attention call (held or normal) supersedes it.
 *   - Dropping the last payload resets IdleSinceTime / bReleaseAsSoonAsIdle,
 *     so a stale idle clock or urgency flag can't leak onto the next
 *     unrelated held item.
 */
struct CONVAI_API FConvaiHeldContextLane
{
	// ── Held states (last-wins per key, insertion-ordered) ────────────
	TArray<FString> StateOrder;
	TMap<FString, FString> StateValues;
	TMap<FString, EC_RunLLMOption> StateRespond;
	TSet<FString> StateOmitPreviousValue;
	// Last transition wins, matching StateValues. Used only by internal
	// producers that know the actual state immediately before a held edge.
	TMap<FString, FString> StatePreviousValueOverrides;
	// Opt-in for state excursions whose intermediate edge matters even when the
	// held value later returns to the unchanged canonical value.
	TSet<FString> StatePreserveTransitions;
	// Explicit one-shot Watch Property ownership, kept separate from the
	// strongest response rank so authoritative replacements can carry the
	// consumed watch across multiple pending/held edges.
	TSet<FString> WatchPromotedStateKeys;

	// ── Held declarative facts (last-wins per key, insertion-ordered) ─
	TArray<FString> DeclarativeOrder;
	TMap<FString, FString> DeclarativeSentences;
	TMap<FString, EC_RunLLMOption> DeclarativeRespond;

	// ── Held events (deduped by text + ephemerality) ──────────────────
	struct FHeldEvent
	{
		FString Text;
		EC_RunLLMOption ShouldRespond = EC_RunLLMOption::Auto;
		bool bEphemeral = false;
	};
	TArray<FHeldEvent> Events;

	// ── Held attention slot (last write wins) ─────────────────────────
	bool bHasAttention = false;
	FConvaiObjectEntry AttentionObject;
	FString AttentionText;
	EC_RunLLMOption AttentionRespond = EC_RunLLMOption::Auto;
	bool bAttentionAddEvent = true;
	// Gaze-held attention re-issues through the gaze-gated setter on release so
	// an Explicit owner that appeared during the hold still wins.
	bool bAttentionFromGaze = false;

	// ── Release-time controls ─────────────────────────────────────────
	// OR of every held call's bFlushImmediately: urgent work releases the
	// moment idle is DETECTED, skipping the quiet period. Batch-level on
	// purpose — one urgent call makes the whole coalesced lane urgent.
	bool bReleaseAsSoonAsIdle = false;
	// When the current stretch of conversation quiet began, or -1 while the
	// conversation is active / disconnected / the lane is empty. Held work
	// releases only after the quiet stretch reaches the chatbot's
	// "Quiet Time Before Delivery" — there is deliberately NO upper bound on
	// the total wait: an update asked to wait for a pause waits for a pause.
	double IdleSinceTime = -1.0;

	bool HasWork() const;
	void Clear();

	/** Advances the idle clock and decides whether held work releases NOW.
	 *  Quiet time counts only while connected AND the conversation is idle;
	 *  activity, disconnects, and an empty lane all reset the clock (a pause
	 *  that started before a disconnect must not count as observed quiet).
	 *  Urgent lanes (bReleaseAsSoonAsIdle) release at the FIRST idle instant.
	 *  Pure decision logic — kept free of component state so it unit-tests
	 *  without a connected session. */
	bool UpdateIdleAndCheckRelease(bool bConversationIdle, bool bConnected, double Now, double QuietSeconds);

	void HoldState(const FString& Key, const FString& Value,
		EC_RunLLMOption ShouldRespond, bool bOmitPreviousValue = false,
		const FString* PreviousValueOverride = nullptr,
		bool bPreserveTransition = false,
		bool bWatchPromoted = false);
	void HoldDeclarative(const FString& Key, const FString& Sentence, EC_RunLLMOption ShouldRespond);
	void HoldEvent(const FString& Text, EC_RunLLMOption ShouldRespond, bool bEphemeral);
	void HoldAttention(const FConvaiObjectEntry& Object, const FString& Text, EC_RunLLMOption ShouldRespond, bool bAddAttentionEvent, bool bFromGaze);

	void DropStateKey(const FString& Key, bool* bOutWatchPromoted = nullptr);
	void DropDeclarativeKey(const FString& Key);
	void DropEvent(const FString& Text, bool bEphemeral);
	void DropAttention();

private:
	int32 IndexOfHeldEvent(const FString& Text, bool bEphemeral) const;

	/** A lane with no payload must not keep release controls armed. */
	void ResetReleaseControlsIfEmpty()
	{
		if (!HasWork())
		{
			IdleSinceTime = -1.0;
			bReleaseAsSoonAsIdle = false;
		}
	}
};
