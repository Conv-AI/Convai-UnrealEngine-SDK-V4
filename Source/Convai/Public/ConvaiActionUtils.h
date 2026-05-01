// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ConvaiDefinitions.h"
#include "ConvaiActionUtils.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiActionUtilsLog, Log, All);

struct FConvaiResultAction;
struct FConvaiObjectEntry;
struct FConvaiAction;
struct FConvaiExtraParams;
struct FConvaiResultParam;
struct FConvaiEnvironmentData;


UCLASS()
class UConvaiActions : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:

	// Function to split a string based on commas, but ignoring commas within quotes
	static TArray<FString> SmartSplit(const FString& SequenceString);

	// Extract a quoted substring from an action target. Tries double quotes first, then
	// single quotes. Returns empty string if neither is present.
	// Examples: Says "I love AI"  → "I love AI"
	//           Greet 'hello world' → "hello world"
	static FString ExtractText(const FString& ActionResult);

	// Extract the first integer found in an action target as a float.
	// Example: "5 seconds" → 5.0
	static float ExtractNumber(const FString& ActionResult);

	static FString FindAction(FString ActionToBeParsed, TArray<FString> Actions);

	// Removes inner descriptions from a string e.g. (Waits for <time in seconds> becomes Waits for)
	static FString RemoveDesc(FString str);

	/** Find the FConvaiAction template whose canonical Name best matches the prefix of an
	 *  LLM-filled-in action name. Returns nullptr when nothing fits. */
	static const FConvaiAction* FindActionTemplate(const FString& FilledIn,
		const TArray<FConvaiAction>& Templates);

	/** Strip the canonical action name from the front of a filled-in string and return
	 *  the trimmed remainder. Case-insensitive prefix match.
	 *  StripActionPrefix("Parse MMA2 537", "Parse") → "MMA2 537" */
	static FString StripActionPrefix(const FString& FilledIn, const FString& CanonicalName);

	/** Split a parameter blob into ordered values. Tries double-quoted segments first
	 *  ("v1" "v2"); falls back to whitespace tokens when no quotes are present. When
	 *  ExpectedCount is 1, the whole blob is returned as a single value (so multi-word
	 *  free-form values stay intact). When ExpectedCount > 1, splits into ExpectedCount
	 *  whitespace tokens with the last one absorbing trailing tokens. */
	static TArray<FString> SplitParamValues(const FString& Blob, int32 ExpectedCount);

	/** Template-aware overload. Tries, in order:
	 *    1. brace-wrapped: `{v1} {v2}`,
	 *    2. named-key style: `paramA: v1 [conn] paramB: v2` (param names as anchors;
	 *       a connector word from the next param's `Connector` field — or a common
	 *       conjunction like "and"/"or"/"," — is trimmed off the tail of each value),
	 *    3. connector-as-separator: when every inter-param Connector is set and present
	 *       in the blob (e.g. template `Put {ball} on {table}` → response `ball on table`),
	 *       split on the connector words to carve out values,
	 *    4. quoted segments: `"v1" "v2"`,
	 *    5. whitespace tokens (last token absorbs the tail).
	 *  Always returns size == Params.Num() (slots not found are empty strings). */
	static TArray<FString> SplitParamValues(const FString& Blob, const TArray<FConvaiActionParam>& Params);

	/** Strip prompt-format mimicry from a raw param value. The LLM sometimes copies the
	 *  prompt's `{name [choices]: type}` shape into responses as e.g. `{character: User}`,
	 *  `{character: ref: User}`, or `{character [happy|sad]: happy}`. This:
	 *    1. removes any `[...]` choice block,
	 *    2. splits on ':' and drops tokens equal (case-insensitive) to ParamName or to a
	 *       known type word (`ref`/`string`/`number`/`bool`/`enum`),
	 *    3. rejoins the remaining tokens with ':' (preserves legitimate values like "3:30").
	 *  Returns Value mostly-unchanged when nothing matches. */
	static FString StripParamNameMimicry(const FString& Value, const FString& ParamName);

	/** Coerce a raw string value into a typed FConvaiResultParam. All value fields are
	 *  populated best-effort regardless of DeclaredType (StringValue always set; NumberValue
	 *  via Atof; BoolValue via true/yes/1; RefValue via Env.FindObject/FindCharacter).
	 *  Type field set to DeclaredType, except when DeclaredType == Auto in which case it's
	 *  inferred from which best-effort coercion succeeded (Reference > Number > Bool > String).
	 *
	 *  When EnumType is non-null AND DeclaredType is Enum, also resolves StringValue
	 *  against the enum's display names and populates ByteValue with the matched index. */
	static FConvaiResultParam CoerceParam(const FString& Value,
		EConvaiActionParamType DeclaredType,
		const FConvaiEnvironmentData& Env,
		const UEnum* EnumType = nullptr);

	// ── BP-friendly accessors for FConvaiResultAction.Parameters ─────

	UFUNCTION(BlueprintPure, Category = "Convai|Action API",
		meta = (DisplayName = "Get First Param"))
	static FConvaiResultParam GetFirstParam(const FConvaiResultAction& Action);

	UFUNCTION(BlueprintPure, Category = "Convai|Action API",
		meta = (DisplayName = "Get Param"))
	static FConvaiResultParam GetParam(const FConvaiResultAction& Action, const FString& Name);

	UFUNCTION(BlueprintPure, Category = "Convai|Action API",
		meta = (DisplayName = "Get Param Type"))
	static EConvaiActionParamType GetParamType(const FConvaiResultAction& Action, const FString& Name);

	UFUNCTION(BlueprintPure, Category = "Convai|Action API",
		meta = (DisplayName = "Get Param As String"))
	static FString GetParamAsString(const FConvaiResultAction& Action, const FString& Name);

	UFUNCTION(BlueprintPure, Category = "Convai|Action API",
		meta = (DisplayName = "Get Param As Number"))
	static float GetParamAsNumber(const FConvaiResultAction& Action, const FString& Name);

	UFUNCTION(BlueprintPure, Category = "Convai|Action API",
		meta = (DisplayName = "Get Param As Bool"))
	static bool GetParamAsBool(const FConvaiResultAction& Action, const FString& Name);

	UFUNCTION(BlueprintPure, Category = "Convai|Action API",
		meta = (DisplayName = "Get Param As Ref"))
	static FConvaiObjectEntry GetParamAsRef(const FConvaiResultAction& Action, const FString& Name);

	/** Returns the param's ByteValue (matched enum index when the template's Type=Enum +
	 *  EnumType was set; 0 otherwise). Convert with Byte-to-Enum<YourType> in BP. */
	UFUNCTION(BlueprintPure, Category = "Convai|Action API",
		meta = (DisplayName = "Get Param As Byte"))
	static uint8 GetParamAsByte(const FConvaiResultAction& Action, const FString& Name);

	UFUNCTION(BlueprintPure, Category = "Convai|Action API",
		meta = (DisplayName = "Has Param"))
	static bool HasParam(const FConvaiResultAction& Action, const FString& Name);

	// ── DEPRECATED — kept for back-compat with prior-iteration BP graphs ─────

	UFUNCTION(BlueprintPure, Category = "Convai|Action API|DEPRECATED",
		meta = (DisplayName = "Get Action Param", DeprecatedFunction,
				DeprecationMessage = "Use Get Param / Get Param As String on the FConvaiResultAction directly."))
	static FString GetActionParam(const FConvaiExtraParams& Params, const FString& Name);

	UFUNCTION(BlueprintPure, Category = "Convai|Action API|DEPRECATED",
		meta = (DisplayName = "Get Action Param As Number", DeprecatedFunction,
				DeprecationMessage = "Use Get Param As Number on the FConvaiResultAction directly."))
	static float GetActionParamAsNumber(const FConvaiExtraParams& Params, const FString& Name);

	UFUNCTION(BlueprintPure, Category = "Convai|Action API|DEPRECATED",
		meta = (DisplayName = "Has Action Param", DeprecatedFunction,
				DeprecationMessage = "Use Has Param on the FConvaiResultAction directly."))
	static bool HasActionParam(const FConvaiExtraParams& Params, const FString& Name);
};
