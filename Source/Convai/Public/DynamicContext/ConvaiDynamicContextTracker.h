// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Tracks dynamic context state properties and events, and builds a canonical
 * context string from them.  Owned by UConvaiChatbotComponent as a plain
 * C++ member — no UObject overhead needed.
 *
 * State properties: key-value pairs, latest value only, insertion-ordered.
 * Declarative facts: like state properties (keyed, latest-value-only,
 *   insertion-ordered, updatable + removable) but rendered as the bare value —
 *   a complete sentence — instead of "Name is Value". Used for spatial/world
 *   facts the context subsystem composes ("cube1 is in front of you and on top
 *   of the plate"), where a key prefix would be noise.
 * Events: chronological list of free-form strings. Past a byte budget the
 *   oldest are dropped, and an event longer than the budget keeps only its
 *   start; states and facts are never trimmed.
 *
 * Canonical format (newline-separated):
 *   <all states, in insertion order>          "Name is Value"
 *   <all declarative facts, in insertion order>  "<sentence>"
 *   <all events, in chronological order>
 */
struct CONVAI_API FConvaiDynamicContextTracker
{
	// ── State properties ──────────────────────────────────────────────

	/** Returns true if the key already existed (i.e. this is an update, not a new entry). */
	bool SetState(const FString& Name, const FString& Value, FString* OutOldValue = nullptr);

	/** Returns true if the key existed and was removed. */
	bool RemoveState(const FString& Name);

	/** Returns true if found. */
	bool GetStateValue(const FString& Name, FString& OutValue) const;

	/** All state rows in insertion order — the debug overlay's snapshot read. */
	void GetStatesSnapshot(TArray<TPair<FString, FString>>& Out) const;

	// ── Declarative facts ─────────────────────────────────────────────

	/** Returns true if the key already existed (i.e. this is an update, not a new entry). */
	bool SetDeclarative(const FString& Key, const FString& Sentence, FString* OutOldValue = nullptr);

	/** Returns true if the key existed and was removed. */
	bool RemoveDeclarative(const FString& Key);

	/** Returns true if found. */
	bool GetDeclarativeValue(const FString& Key, FString& OutValue) const;

	/** All declarative facts in insertion order — the debug overlay's snapshot read. */
	void GetDeclarativesSnapshot(TArray<TPair<FString, FString>>& Out) const;

	// ── Events ────────────────────────────────────────────────────────

	void AddEvent(const FString& Text);

	/** All committed events, chronological — the debug overlay's snapshot read. */
	void GetEventsSnapshot(TArray<FString>& Out) const { Out = Events; }

	// ── Canonical context ─────────────────────────────────────────────

	/**
	 * Builds the full canonical context string from current state + declaratives
	 * + events. Declarative facts always render in their canonical slot (they
	 * never appear as delta lines); every event still held is emitted, and
	 * AddEvent keeps them within a byte budget by dropping the oldest.
	 *
	 * @param ExcludeStateKeys  Optional set of state keys to omit from the
	 *   canonical output. Used by the flush path to defer first-appearance
	 *   keys: a brand-new state is shown only as a delta line at the prompt
	 *   tail for emphasis on this flush, and then enters canonical naturally
	 *   on the next flush.
	 */
	FString BuildCanonicalContext(const TSet<FString>& ExcludeStateKeys = {}) const;

	// ── Reset ─────────────────────────────────────────────────────────

	void Reset();

private:
	// Leaves the oldest events out of a Replace that would pass the transport limit.
	friend class UConvaiChatbotComponent;

	/** BuildCanonicalContext with the events from FirstEvent on. */
	FString BuildCanonicalContextFrom(const TSet<FString>& ExcludeStateKeys, int32 FirstEvent) const;

	/** Ordered list of state keys (insertion order). */
	TArray<FString> StateKeyOrder;

	/** Key → Value lookup. */
	TMap<FString, FString> StateValues;

	/** Ordered list of declarative-fact keys (insertion order). */
	TArray<FString> DeclarativeKeyOrder;

	/** Key → Sentence lookup for declarative facts. */
	TMap<FString, FString> DeclarativeValues;

	/** Events in chronological order. */
	TArray<FString> Events;

	/** UTF-8 bytes of Events, counting one newline per event. */
	int32 EventBytes = 0;
	bool bWarnedEventTrim = false;
};
