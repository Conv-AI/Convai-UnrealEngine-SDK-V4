// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiDefinitions.h"

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

	// ── Staged events (deduped by string) ─────────────────────────────
	TArray<FString> EventsOrdered;
	TSet<FString> EventsSet;

	// ── Aggregate ShouldRespond across all staged items ───────────────
	EC_RunLLMOption AggregateRunLLM = EC_RunLLMOption::Never;

	// Force a canonical Replace on next flush even if no state/event is staged.
	// Set by RemoveContextState so the flush sees the updated tracker.
	bool bForceReplace = false;

	// Send a Reset command on next flush after draining the staged batch.
	// Set by ResetDynamicContext.
	bool bPendingReset = false;

	bool IsEmpty() const;
	bool HasWork() const;
	bool HasNonNever() const;
	bool HasStagedWork() const;

	/** Clears staged state/events and control flags (but NOT bPendingReset). */
	void ClearStaged();

	/** Clears everything including bPendingReset. */
	void Clear();

	void StageState(FConvaiDynamicContextTracker& Tracker, const FString& Name, const FString& Value, EC_RunLLMOption ShouldRespond);
	void StageEvent(const FString& Text, EC_RunLLMOption ShouldRespond);
	void DropStateKey(const FString& Name);
	void MergeRunLLM(EC_RunLLMOption In);
};
