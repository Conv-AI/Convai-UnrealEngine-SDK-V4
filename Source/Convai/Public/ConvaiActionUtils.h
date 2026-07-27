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
class USceneComponent;


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
	 *  against the enum's display names (via FuzzyFindLabel) and populates ByteValue with
	 *  the matched value.
	 *
	 *  When Choices is non-null and non-empty AND DeclaredType is not Enum, validates
	 *  StringValue against Choices via FuzzyFindLabel (no canonicalization).
	 *
	 *  bOutConstraintMatched (optional) reports whether the declared Enum/Choices constraint
	 *  accepted the value — true when no constraint was provided, true on exact or fuzzy
	 *  match, false when the value was outside the Levenshtein threshold. Use it to log a
	 *  warning at the call site without re-running the matching. */
	static FConvaiResultParam CoerceParam(const FString& Value,
		EConvaiActionParamType DeclaredType,
		const FConvaiEnvironmentData& Env,
		const UEnum* EnumType = nullptr,
		const TArray<FString>* Choices = nullptr,
		bool* bOutConstraintMatched = nullptr);

	/** Find the index of the closest matching label for Query in Labels. Three passes:
	 *  1) exact case-insensitive equality;
	 *  2) closest by Levenshtein distance within `Clamp(Query.Len()/2, 2, 4)` (mirrors
	 *     the threshold used by FindBestPhraseMatch);
	 *  3) whole-word substring of any label of length >= 4 inside Query — catches LLM
	 *     paraphrases that glue extra tokens onto a valid choice ("wave:User"). Tie-break:
	 *     longest label, then earliest position, then first-declared.
	 *  Returns INDEX_NONE when no pass produces a candidate or when inputs are empty.
	 *  OutDistance receives 0 on exact or substring match, the Levenshtein distance on fuzzy. */
	static int32 FuzzyFindLabel(const FString& Query, const TArray<FString>& Labels, int32& OutDistance);

	/** Collect display-name labels for a UEnum (falls back to GetNameStringByIndex when
	 *  the display name is empty). Excludes the trailing _MAX entry when present. */
	static TArray<FString> GetEnumLabels(const UEnum* EnumType);

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

	// ── FConvaiObjectEntry Blueprint helper ──────────────────────────

	/** Resolves an FConvaiObjectEntry into the concrete inputs an AI Move To
	 *  node needs PLUS optional navmesh reachability data. Canonical bridge
	 *  between Convai's object model and UE's movement nodes.
	 *
	 *  **Wire Target Actor + Destination straight into one AI Move To — no
	 *  branch needed.** On successful resolution, Target Actor is null exactly
	 *  when the goal is a fixed location (a Movement Point won, or the entry
	 *  references a component), so AI Move To falls through to the Destination
	 *  pin automatically; when Target Actor is set, following the actor is the
	 *  correct behavior (the goal tracks a moving object). A failed resolution
	 *  (bOut Success false) also returns null. Uses Destination (advanced)
	 *  exposes the location-vs-actor fact as a bool for graphs that want an
	 *  explicit branch.
	 *
	 *   - Source Actor:          optional. When provided, the function also
	 *                            runs a navmesh path query from Source Actor
	 *                            to this entry and populates the reachability
	 *                            outputs below. Leave null when you only want
	 *                            the movement-goal fields.
	 *   - Target Actor:          wire to AI Move To · Target Actor. Null when
	 *                            the goal is a location (see above) — exactly
	 *                            when the Destination pin should win — and
	 *                            also null when resolution failed.
	 *   - Object Actor (advanced): Entry.Ref, always populated while alive —
	 *                            for non-movement BP (attaching effects,
	 *                            queries), regardless of what won the goal.
	 *   - Out Goal Location ("Destination"): resolved world position — the
	 *                            winning Movement Point when any are enabled,
	 *                            else the component/socket location (Specific
	 *                            Component reference) or Ref's origin. Wire to
	 *                            AI Move To · Destination.
	 *   - Out Goal Component:    the resolved sub-component on Ref when the
	 *                            object reference is a component and Component
	 *                            Name matched. Useful for non-movement BP
	 *                            (attaching effects, queries) — AIMoveTo
	 *                            itself can't take components.
	 *   - Out Acceptance Radius: the entry's Acceptance Radius, forwarded for
	 *                            the AI Move To call.
	 *   - bOut Move To Location ("Uses Destination", advanced): true when the
	 *                            goal is a location (Movement Point won, or
	 *                            the object reference is a specific component)
	 *                            — i.e. exactly when Target Actor is null.
	 *                            Only needed for explicit-branch graphs.
	 *   - bOut Success:          true when the entry resolved to a live target
	 *                            you can hand to AI Move To. False means Ref
	 *                            is null/destroyed and Out Goal Location is
	 *                            just the snapshotted Optional Position
	 *                            Vector (deprecated marker-only entry) — the
	 *                            other outputs are still populated for
	 *                            inspection but AIMoveTo would no-op on an
	 *                            actor goal and walk to a stale point on a
	 *                            location goal. ALWAYS branch on this before
	 *                            invoking AI Move To. Reachability outputs
	 *                            are also meaningless when bOut Success is
	 *                            false (the nav query is skipped because we
	 *                            have nothing to path TO).
	 *   - bOut Already There:    true when Source Actor was provided AND it
	 *                            is already at the goal. With Movement Points:
	 *                            within max(Acceptance Radius, 150 uu) of any
	 *                            enabled point. Object fallback:
	 *                            distance to the nearest point on the entry's
	 *                            footprint within max(Acceptance Radius × 2,
	 *                            150 uu). Either way ANDed with a vertical
	 *                            check against Source Actor's own height.
	 *                            Branch on this before calling AI Move To to
	 *                            short-circuit a no-op move when the actor is
	 *                            already arrived.
	 *   - bOut Reachable:        true when Source Actor was provided AND the
	 *                            navmesh path landed within tolerance of the
	 *                            goal — a Movement Point's acceptance radius,
	 *                            or (object fallback) within max(Acceptance
	 *                            Radius × 2, 150 uu) of the entry's footprint —
	 *                            plus Source Actor's own vertical span. False
	 *                            when no Source Actor, no nav system, no nav
	 *                            data, path failure, or out of tolerance.
	 *                            When Movement Points are authored, reachable
	 *                            means AT LEAST ONE point is; the shortest-path
	 *                            point is the selected goal.
	 *   - Out Path End Point:    final navpoint reached by the path query
	 *                            (may be a partial-path endpoint when the
	 *                            goal isn't fully reachable).
	 *   - Out Path Points:       full nav path from Source Actor to Out Path
	 *                            End Point, in world space. Useful for debug
	 *                            visualisation.
	 *   - Out Goal Travel Distance: nav path length (uu) to the selected goal;
	 *                            0 when no path was computed / already there.
	 *   - Out Movement Point Index: which Movement Point won (-1 when the
	 *                            object reference resolved the goal). */
	UFUNCTION(BlueprintCallable, Category = "Convai|Action API",
		meta = (DisplayName = "Resolve Goal Location",
			AdvancedDisplay = "OutGoalActor,bOutMoveToLocation",
			ToolTip = "Resolves an object entry into ready-to-wire AI Move To inputs and (when Source Actor is set) the source-relative outputs: already-there arrival check, navmesh reachability, and full path points. Movement Points (when authored) take over the goal: the reachable point with the shortest walking path wins. Wire Target Actor + Destination straight into AI Move To — Target Actor is null exactly when the goal is a fixed location, so no branch is needed."))
	static void ResolveGoalLocation(UPARAM(ref) FConvaiObjectEntry& Entry,
		AActor* SourceActor,
		UPARAM(DisplayName = "Target Actor") AActor*& OutMoveTargetActor,
		UPARAM(DisplayName = "Object Actor") AActor*& OutGoalActor,
		USceneComponent*& OutGoalComponent,
		UPARAM(DisplayName = "Destination") FVector& OutGoalLocation,
		float& OutAcceptanceRadius,
		UPARAM(DisplayName = "Uses Destination") bool& bOutMoveToLocation,
		bool& bOutSuccess,
		bool& bOutAlreadyThere,
		bool& bOutReachable,
		FVector& OutPathEndPoint,
		TArray<FVector>& OutPathPoints,
		float& OutGoalTravelDistance,
		int32& OutMovementPointIndex);

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
