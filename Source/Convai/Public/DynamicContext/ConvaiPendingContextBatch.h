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
 * the flush can produce a "Key changed from X to Y" delta line.
 *
 * Rules:
 *   - Staging the same state key multiple times overwrites the tracker value
 *     but keeps the original pre-batch snapshot (first write wins for delta).
 *   - Staging the same event string multiple times deduplicates (first
 *     occurrence wins for ordering).
 *   - AggregateRunLLM is upgraded to the maximum rank seen: Always > Auto > Never.
 */
struct CONVAI_API FConvaiPendingContextBatch
{
	// ── Staged state (insertion-ordered, deduped via Contains) ────────
	TArray<FString> StagedStateOrder;

	// Pre-batch value for each staged key; key absent means the key is new.
	TMap<FString, FString> OldValues;

	// ── Staged declarative facts (insertion-ordered, deduped via Contains) ──
	TArray<FString> StagedDeclarativeOrder;

	// Pre-batch sentence for each staged declarative key; key absent means new.
	TMap<FString, FString> DeclarativeOldValues;

	// ── Staged events (deduped by string) ─────────────────────────────
	TArray<FString> EventsOrdered;
	TSet<FString> EventsSet;

	// ── Staged EPHEMERAL (one-flush) events (deduped by string) ───────
	// Emitted in the next context update EXACTLY ONCE, then dropped on
	// ClearStaged. Unlike normal events these are NEVER committed to the
	// canonical tracker, so they never reappear on later updates — as if the
	// event was never added. Used for transient cues (e.g. "X is paying
	// attention to Y") that should nudge the AI once without piling up.
	TArray<FString> EphemeralEventsOrdered;
	TSet<FString> EphemeralEventsSet;

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

	void StageState(FConvaiDynamicContextTracker& Tracker, const FString& Name, const FString& Value, EC_RunLLMOption ShouldRespond);
	void StageDeclarative(FConvaiDynamicContextTracker& Tracker, const FString& Key, const FString& Sentence, EC_RunLLMOption ShouldRespond);
	void StageEvent(const FString& Text, EC_RunLLMOption ShouldRespond, bool bEphemeral = false);
	void StageAttention(const FConvaiObjectEntry& Object, const FString& Text, EC_RunLLMOption ShouldRespond);
	void DropStateKey(const FString& Name);
	void DropDeclarativeKey(const FString& Name);
	void MergeRunLLM(EC_RunLLMOption In);
};
