// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Dynamic-context prompt formatting. Keys are sanitized once at the
 * UConvaiChatbotComponent state API choke points (set / remove / get agree);
 * values stay stored raw and are quoted at render time only.
 */
namespace ConvaiContextFormat
{
	/** Strips whitespace from a (possibly dotted) state key by Pascal-casing
	 *  each segment that contains whitespace ("Front Door.door state" →
	 *  "FrontDoor.DoorState"). Whitespace-free input returns unchanged. */
	FString SanitizeKey(const FString& Key);

	/** Returns the value ready for prompt rendering: unchanged when a single
	 *  non-empty word; otherwise quoted, with `"` `\` and newlines/tabs
	 *  minimally escaped. */
	FString FormatValueForPrompt(const FString& Value);

	/** Case-insensitive match against either a raw state value or the exact
	 *  quoted/escaped literal that FormatValueForPrompt exposes to the model. */
	bool ValueMatchesPromptLiteral(const FString& Candidate, const FString& RawValue);

	/** Pure one-shot watch helpers. Empty TargetValue means any canonical change.
	 *  A targeted watch may also match a return to the canonical value after a
	 *  non-target departure was observed in an idle-delivery lane. */
	bool WouldWatchMatch(const FString& TargetValue, const FString& ExistingValue,
		const FString& NewValue, bool bObservedTargetDeparture);
	bool IsTargetWatchDeparture(const FString& TargetValue,
		const FString& ExistingValue, const FString& NewValue);
}
