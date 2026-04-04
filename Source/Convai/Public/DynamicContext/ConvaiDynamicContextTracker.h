// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Tracks dynamic context state properties and events, and builds a canonical
 * context string from them.  Owned by UConvaiChatbotComponent as a plain
 * C++ member — no UObject overhead needed.
 *
 * State properties: key-value pairs, latest value only, insertion-ordered.
 * Events: chronological list of free-form strings.
 *
 * Canonical format (newline-separated):
 *   <all states, in insertion order>    "Name is Value"
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

	// ── Events ────────────────────────────────────────────────────────

	void AddEvent(const FString& Text);

	// ── Canonical context ─────────────────────────────────────────────

	/** Builds the full canonical context string from current state + events. */
	FString BuildCanonicalContext() const;

	// ── Reset ─────────────────────────────────────────────────────────

	void Reset();

private:
	/** Ordered list of state keys (insertion order). */
	TArray<FString> StateKeyOrder;

	/** Key → Value lookup. */
	TMap<FString, FString> StateValues;

	/** Events in chronological order. */
	TArray<FString> Events;
};
