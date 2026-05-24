// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Engine/GameEngine.h"
#include "Runtime/Launch/Resources/Version.h"
#include "RestAPI/ConvaiURL.h"
#include "ConvaiDefinitions.generated.h"

class USceneComponent;

DECLARE_LOG_CATEGORY_EXTERN(ConvaiDefinitionsLog, Log, All);

/**
 * Picks what AI Move To should head toward when this entry is the target of a
 * movement action.
 *
 *  - Actor:  use Ref as the actor goal. AI Move To stops when it reaches the
 *            actor's bounds — it can't step INTO the actor's volume. Best for
 *            "reach the car", "follow the player".
 *  - Vector: compute a world point and feed it as the goal. The point comes
 *            from Ref's origin by default; if Component Name is set, the
 *            target sub-component's location (or socket) is used instead.
 *            Step Onto Bounds and Offset further refine the point. Best for
 *            "step onto the platform", "stand at the door handle", "stand
 *            at the floor marker".
 *
 * Acceptance Radius applies in every mode — none of these is "exact".
 */
UENUM(BlueprintType)
enum class EConvaiMoveTarget : uint8
{
	/** Walk toward the whole Actor — the AI stops when it touches the actor's bounds. Pick this for "go to the car", "follow the player". */
	Actor  UMETA(DisplayName = "Actor as goal"),
	/** Walk toward a specific point on the Actor — a named sub-component, a socket, optionally stepping onto its bounds, plus an offset. Pick this for "stand at the door handle", "step onto the platform". */
	Vector UMETA(DisplayName = "Component as goal"),
};

USTRUCT(BlueprintType)
struct CONVAI_API FNarrativeDecision
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString criteria;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString next_section_id;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	int32 priority;

    FNarrativeDecision()
        : criteria(TEXT("")), // Initialize with default empty string
		next_section_id(TEXT("")), // Initialize with default empty string
		priority(0) // Initialize with default priority
    {
    }
};

USTRUCT(BlueprintType)
struct CONVAI_API FNarrativeTrigger
{
	GENERATED_BODY()

	UPROPERTY()
	FString character_id;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString destination_section;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString trigger_id;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString trigger_message;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString trigger_name;

	FNarrativeTrigger()
		: destination_section(TEXT("")),
		trigger_id(TEXT("")),
		trigger_message(TEXT("")),
		trigger_name(TEXT(""))
	{
	}
};

USTRUCT(BlueprintType)
struct CONVAI_API FNarrativeSection
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString behavior_tree_code;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString bt_constants;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString character_id;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	TArray<FNarrativeDecision> decisions;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString objective;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	TArray<FString> parents;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString section_id;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString section_name;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	TMap<FString, FString> updated_character_data;

    FNarrativeSection()
        : behavior_tree_code(TEXT("")),
		bt_constants(TEXT("")),
		character_id(TEXT("")),
		objective(TEXT("")),
		section_id(TEXT("")),
		section_name(TEXT(""))
    {
    }
};

UENUM(BlueprintType)
enum class ETTS_Voice_Type : uint8
{
	MALE,
	FEMALE,
	WUKMale_1,
	WUKFemale_1,
	SUKMale_1,
	WAFemale_1,
	WAMale_1,
	SIFemale_1,
	SIMale_1,
	SUFemale_1,
	SUMale_1,
	WUFemale_1,
	WUMale_1,
	Trixie,
	Twilight_Sparkle,
	Celestia,
	Spike,
	Applejack,
};

UENUM(BlueprintType)
enum class EEmotionIntensity : uint8
{
	Basic        UMETA(DisplayName = "Basic"),
	LessIntense  UMETA(DisplayName = "Less Intense"),
	MoreIntense  UMETA(DisplayName = "More Intense"),
	None         UMETA(DisplayName = "None", BlueprintHidden) // To handle cases when the emotion is not found
};

UENUM(BlueprintType)
enum class EBasicEmotions : uint8
{
	Joy          UMETA(DisplayName = "Happy"),
	Trust        UMETA(DisplayName = "Calm"),
	Fear         UMETA(DisplayName = "Afraid"),
	Surprise     UMETA(DisplayName = "Surprise"),
	Sadness      UMETA(DisplayName = "Sad"),
	Disgust      UMETA(DisplayName = "Bored"),
	Anger        UMETA(DisplayName = "Angry"),
	Anticipation UMETA(DisplayName = "Anticipation", Hidden), // No longer used
	None         UMETA(DisplayName = "None", Hidden) // To handle cases when the emotion is not found
};

USTRUCT(BlueprintType)
struct CONVAI_API FConvaiObjectEntry
{
	GENERATED_USTRUCT_BODY()

public:
	/** A refrence of a character or object*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		TWeakObjectPtr<AActor> Ref;

	/** DEPRECATED — auto-written snapshot of the resolved goal position. Use
	 *  the "Out Goal Location" output from Resolve Goal Location instead, which
	 *  is always live and doesn't rely on this field being up-to-date. Kept for
	 *  back-compat with BP graphs that read it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API",
		meta = (DeprecatedProperty,
				DeprecationMessage = "Use Resolve Goal Location's Out Goal Location output. This field is now an auto-written snapshot and should not be set manually."))
		FVector OptionalPositionVector;

	/** The Name of the character or object*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FString Name;


	/** The bio/description for the chracter/object*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FString Description;

	// ── Movement / targeting ─────────────────────────────────────────
	// Pick Move Target Mode first. In Actor mode, AI Move To uses Ref directly
	// (stops at bounds). In Vector mode, Convai computes a world point — Ref's
	// origin by default, or a sub-component on Ref if Component Name is set,
	// with optional socket / Step Onto Bounds on top.

	/** What the AI should head toward when this entry is a movement target.
	 *  See the enum's description for what each value does. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API")
		EConvaiMoveTarget MoveTargetMode = EConvaiMoveTarget::Actor;

	/** How close (in cm) the AI must get before the move is considered done.
	 *  Forwarded to AI Move To. Smaller for buttons/handles, larger for
	 *  vehicles or wide objects. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API",
		meta = (ClampMin = "0.0"))
		float AcceptanceRadius = 150.f;

	/** Optional — refine Vector mode to target a specific sub-component on Ref
	 *  by case-insensitive substring match on the component name. Example:
	 *  "muzzle" matches both "MuzzleSocket_L" and "muzzle_flash_attach". If
	 *  multiple components match, the first is used and a warning is logged —
	 *  tighten the text to disambiguate. Leave empty to target Ref's origin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API",
		meta = (EditCondition = "MoveTargetMode == EConvaiMoveTarget::Vector"))
		FString ComponentName;

	/** Optional socket or bone to target on the matched component. Sockets are
	 *  supported on Static Mesh / Skeletal Mesh components; bones additionally
	 *  on Skeletal Mesh. If the socket/bone isn't found, the goal falls back
	 *  to the component's origin (a warning is logged). Leave as None to use
	 *  the component's origin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API",
		meta = (EditCondition = "MoveTargetMode == EConvaiMoveTarget::Vector"))
		FName SocketOrBoneName;

	/** Tick when the AI should walk to the TOP of the resolved target — i.e. step ONTO
	 *  it — instead of stopping at its bounds. Convai projects the goal to the top of
	 *  the bounding box so the AI walks up onto a platform, table, or crate.
	 *
	 *  Works in BOTH targeting modes:
	 *   - "Actor as goal" + Step Onto Bounds: use the whole actor's bounds. The AI
	 *     walks ONTO the actor instead of stopping at its edge. (Internally this
	 *     resolves to a top-of-bounds vector goal.)
	 *   - "Component as goal" + Step Onto Bounds: use the resolved component's
	 *     bounds. (Or the actor's bounds if no Component Name is set or none match.)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API",
		meta = (DisplayName = "Step Onto Bounds"))
		bool bStepOntoBounds = false;

	/** Output only — the component Convai resolved using Component Name.
	 *  Convai fills this in when the action arrives. Read it from Blueprint
	 *  when you need to do something component-specific (attach an effect,
	 *  change a property). Clears itself if the component is destroyed. */
	UPROPERTY(Transient, BlueprintReadOnly,
		category = "Convai|Action API")
		TWeakObjectPtr<USceneComponent> ResolvedComponent;

	/** Returns true when Move Target Mode is Component AND Component Name is set. */
	bool HasComponentFilters() const;

	/** Resolves Component Name against Ref's component tree and caches the
	 *  match in Resolved Component. Returns null when Move Target Mode is not
	 *  Vector, when Ref or Component Name is missing, or when nothing matched.
	 *  Pass bForceRefresh = true to ignore the cache. */
	USceneComponent* ResolveComponent(bool bForceRefresh = false);

	/** Resolves this entry into the full set of inputs an AI Move To node needs,
	 *  PLUS optional navmesh reachability data when Source Actor is provided.
	 *
	 *  Position resolution (always produced, in Out Goal Location):
	 *   - Actor mode:           Ref's origin (informational; AI Move To uses
	 *                           the actor pin in this mode). Step-Onto-Bounds
	 *                           on Actor mode promotes to a top-of-actor-bounds
	 *                           vector — Out Mode flips to Vector so the BP
	 *                           graph picks the location pin.
	 *   - Vector mode + no
	 *     Component Name:       Ref's origin + Step-Onto-Bounds (actor bounds).
	 *   - Vector mode + Component
	 *     Name matched:         component / socket + Step-Onto-Bounds
	 *                           (component bounds).
	 *   - No Ref:               falls back to Optional Position Vector as-is
	 *                           (deprecated marker-only entries); bOut Success
	 *                           is false.
	 *  Also snapshots the resolved location into Optional Position Vector so
	 *  legacy reads stay consistent.
	 *
	 *  bOut Success — branch on this before consuming the other outputs:
	 *    - true: Ref was alive and resolution succeeded. Out Goal Actor, Out
	 *            Goal Component (Vector mode + Component Name match), Out Goal
	 *            Location, Out Acceptance Radius, and Out Mode are all valid;
	 *            reachability outputs are valid when Source Actor was passed.
	 *    - false: Ref is null/destroyed. Out Goal Location holds the
	 *             snapshotted Optional Position Vector (deprecated marker-
	 *             only path), Out Goal Actor is null, Out Goal Component is
	 *             null, Out Mode mirrors Move Target Mode but is informational
	 *             only. Reachability outputs are zero-initialised — the nav
	 *             query is skipped because there's no live target to path TO.
	 *  AI Move To consumers MUST gate on this; passing a destroyed Ref will
	 *  silently no-op (Actor mode) or send the pawn to a stale point
	 *  (Vector mode).
	 *
	 *  Source-relative outputs (only populated when Source Actor is non-null):
	 *   - bOut Already There: true when Source Actor is effectively at the
	 *                       goal. Horizontal test depends on Step Onto Bounds:
	 *                       OFF — distance from Source Actor to the nearest
	 *                       point on the entry's bounding-box footprint within
	 *                       max(Acceptance Radius × 2, 150 uu), a conservative
	 *                       test wider than AIMoveTo's own trigger; ON —
	 *                       distance to the goal (the centre of the target's
	 *                       top face) within Acceptance Radius. Either way
	 *                       ANDed with |Z delta to the nearest face of the
	 *                       entry's bounding box| within Source Actor's own
	 *                       height. Cheap, no nav query — check it BEFORE
	 *                       issuing AI Move To to avoid no-op move commands.
	 *   - bOut Reachable:   true when a navmesh path from Source Actor lands
	 *                       on this entry's bounds. Horizontal test matches
	 *                       Step Onto Bounds: ON — the path's final point must
	 *                       land ON the footprint (containment); OFF — within
	 *                       max(Acceptance Radius × 2, 150 uu) of the nearest
	 *                       footprint face. Plus Source Actor's own vertical
	 *                       span. False on no nav system, no nav data for Source Actor's
	 *                       agent props, path failure, or out-of-tolerance.
	 *   - Out Path End:     the path's final navpoint (may be a partial-path
	 *                       endpoint if the goal isn't fully reachable).
	 *   - Out Path Points:  the full nav path (world space) from start to
	 *                       Out Path End. Useful for debug visualisation.
	 *  When Source Actor is null, the source-relative outputs are zero-
	 *  initialised.
	 *
	 *  Must be called on the game thread — it reads live actor/component
	 *  transforms and bounds, and may run a synchronous nav query. */
	void ResolveGoalLocation(
		AActor* SourceActor,
		bool bForceRefresh,
		AActor*& OutGoalActor,
		USceneComponent*& OutGoalComponent,
		FVector& OutGoalLocation,
		float& OutAcceptanceRadius,
		EConvaiMoveTarget& OutMode,
		bool& bOutSuccess,
		bool& bOutAlreadyThere,
		bool& bOutReachable,
		FVector& OutPathEndPoint,
		TArray<FVector>& OutPathPoints);

	/** Refreshes the Optional Position Vector + Resolved Component snapshots
	 *  without computing nav reachability. Use this from non-movement code
	 *  paths that just need the cached values to be current (e.g. param
	 *  resolution at receive time). Skips when Ref is destroyed. */
	void RefreshSnapshot(bool bForceRefresh = false);

	friend bool operator==(const FConvaiObjectEntry& lhs, const FConvaiObjectEntry& rhs)
	{
		return lhs.Name == rhs.Name;
	}

	FConvaiObjectEntry()
		: Ref(nullptr)
		, OptionalPositionVector(FVector(0, 0, 0))
		, Name(FString(""))
		, Description(FString(""))
	{
	}
};

/**
 * Declared / inferred type for an action parameter. Drives both the prompt hint
 * rendered into the wire format AND how the parser interprets the response.
 *
 * Note: regardless of declared type, FConvaiResultParam always populates ALL
 * value fields best-effort, so handlers can read whichever interpretation suits.
 * Type just signals which one was the *intended* slot.
 */
UENUM(BlueprintType)
enum class EConvaiActionParamType : uint8
{
	/** Infer at parse time: try Reference, then Number, then Bool; fall back to String. */
	Auto      UMETA(DisplayName = "Auto"),
	/** Resolve the value against Environment.Objects/Characters. */
	Reference UMETA(DisplayName = "Actor Reference"),
	String    UMETA(DisplayName = "String"),
	Number    UMETA(DisplayName = "Number"),
	Bool      UMETA(DisplayName = "Bool"),
	/** Constrain to the values of a UEnum picked in EnumType. Auto-fills Choices. */
	Enum      UMETA(DisplayName = "Enum"),
};

/**
 * Legacy parameter container — kept as a deprecated UPROPERTY type on
 * FConvaiResultAction so existing handler graphs that read .ConvaiExtraParams.X
 * still resolve. New code: read FConvaiResultAction.Parameters directly.
 */
USTRUCT(BlueprintType)
struct FConvaiExtraParams
{
	GENERATED_BODY()

public:
	/** Legacy: first numeric value found in the action target/name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		float Number;

	/** Legacy: first quoted text found in the action target/name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FString Text;

	/** Legacy: per-placeholder string values, projected from the new Parameters map. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		TMap<FString, FString> NamedParams;

	FConvaiExtraParams()
		:Number(0),
		Text("")
	{

	}
};

/**
 * One placeholder parameter on an FConvaiAction template. Order matters — when the LLM
 * fills in the template, returned values are mapped back to these names by position.
 */
USTRUCT(BlueprintType)
struct FConvaiActionParam
{
	GENERATED_BODY()

	/** Placeholder name as it appears in the template, e.g. "email", "time in seconds". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	FString Name;

	/** Optional human-language description of what the parameter means. Surfaced to the
	 *  LLM in the action_config so it picks better values. Empty descriptions are skipped
	 *  in the rendered string — the param name is always rendered as a placeholder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	FString Description;

	/** What kind of value the LLM should fill in. Drives both prompt hint AND parse
	 *  interpretation. Default Auto — most flexible, infers at parse time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	EConvaiActionParamType Type = EConvaiActionParamType::Auto;

	/** Optional joining text rendered before this param in the wire format. Use for
	 *  compound actions: "Put ball on table" → second param has Connector="on".
	 *  Not limited to prepositions — anything that links the param to what came before. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	FString Connector;

	/** Optional fixed-choice constraint. When non-empty, rendered into the wire format
	 *  as `[choice1|choice2|...]` so the LLM picks from the list. The parser
	 *  validates against this set on receipt — values not in the list still flow
	 *  through to the result but with a warning logged.
	 *  Grayed out in the Details panel when Type == Enum (auto-derived from EnumType).
	 *  We use plain EditCondition (not EditConditionHides) because the latter has a
	 *  known UE bug that breaks right-click → Delete on the parent array. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API",
		meta = (EditCondition = "Type != EConvaiActionParamType::Enum"))
	TArray<FString> Choices;

	/** UEnum to draw choices from when Type == Enum. The wire format uses each enum
	 *  value's display names. Required when Type == Enum — the rendered preview surfaces
	 *  an `[ERROR: …]` token if unset and the parser logs a warning. Grayed out when
	 *  Type != Enum (see Choices comment for why we don't use EditConditionHides). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API",
		meta = (EditCondition = "Type == EConvaiActionParamType::Enum"))
	TObjectPtr<UEnum> EnumType = nullptr;

	FConvaiActionParam() = default;
	FConvaiActionParam(const FString& InName,
					   const FString& InDescription = FString(),
					   EConvaiActionParamType InType = EConvaiActionParamType::Auto)
		: Name(InName), Description(InDescription), Type(InType) {}
};

/**
 * Structured action template sent to the server in action_config.actions[]. Replaces
 * the old free-form "Action <placeholder>" string with proper Name / Description /
 * Parameters fields, so designers get a typed BP UX and the LLM gets clearer guidance.
 *
 * The template is rendered to a single string at /connect time via ToActionConfigString.
 * The owning chatbot keeps RenderedString in sync via PostEditChangeProperty so designers
 * see a live preview in the Details panel — the preview is also editable, parsing back
 * into the structured fields via ParseFromActionConfigString().
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiAction
{
	GENERATED_BODY()

	/** Canonical action name without placeholders, e.g. "Parse", "Wait For", "Move To". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	FString Name;

	/** Optional human-language description of what the action does. Skipped in the
	 *  rendered string when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	FString Description;

	/** Ordered parameters. The wire format renders each as {Param.Name}; the order is
	 *  used to map response values back to placeholder names. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	TArray<FConvaiActionParam> Parameters;

	/** Wire-format string kept in sync with the structured fields above. Persisted (not
	 *  Transient) because Transient on a nested struct UPROPERTY interacted badly with
	 *  UE's array-element context menu (right-click → Delete was blocked).
	 *  PostEditChangeProperty / PostLoad keep it consistent, so the duplicated storage
	 *  is harmless. Hidden from the editor and BP — designers should only ever interact
	 *  with the structured fields above. */
	UPROPERTY()
	FString RenderedString;

	/** When true, defer firing this action (only when it lands as the first action of a
	 *  freshly-arrived sequence — i.e. the queue was empty) until the bot signals it has
	 *  begun or finished speaking, OR until the per-character timeout (see
	 *  UConvaiChatbotComponent::ActionWaitForBotSpeechTimeoutSec) elapses. Has no effect
	 *  on subsequent actions in a sequence — those always fire immediately. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API",
		meta = (ToolTip = "Wait until the character speaks before running this action."))
	bool bWaitForBotSpeech = false;

	/** Additional delay (seconds) applied AFTER the wait-for-speech condition resolves,
	 *  before the action actually fires. Treated as 0 at runtime when bWaitForBotSpeech
	 *  is false, regardless of the persisted value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API",
		meta = (EditCondition = "bWaitForBotSpeech", ClampMin = "0.0",
				ToolTip = "Extra delay (seconds) after the character speaks before this action runs."))
	float DelayAfterBotSpeechSec = 0.0f;

	FConvaiAction() = default;
	FConvaiAction(const FString& InName,
				  const FString& InDescription = FString(),
				  const TArray<FConvaiActionParam>& InParameters = {})
		: Name(InName), Description(InDescription), Parameters(InParameters) {}

	/** Renders to the string sent in action_config.actions[]. */
	FString ToActionConfigString() const;

	/** Reverse of ToActionConfigString. Returns true on successful parse, populating Out
	 *  with Name / Description / Parameters. Returns false on malformed input. */
	static bool ParseFromActionConfigString(const FString& Source, FConvaiAction& Out);
};

/**
 * One typed parameter value in an FConvaiResultAction. All value fields are populated
 * best-effort regardless of the declared Type, so handlers can read whichever
 * interpretation is convenient. Type just signals which slot was the LLM's intended target.
 */
USTRUCT(BlueprintType)
struct FConvaiResultParam
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	EConvaiActionParamType Type = EConvaiActionParamType::String;

	/** Raw value as a string, always populated. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	FString StringValue;

	/** Atof(StringValue). 0 if not numeric. Always attempted. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	float NumberValue = 0.f;

	/** "true"/"yes"/"1" → true; otherwise false. Always attempted. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	bool BoolValue = false;

	/** Looked up against Environment.Objects then .Characters. Always attempted. Empty
	 *  entry when no match. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	FConvaiObjectEntry RefValue;

	/** Byte value of the matched enum entry when the declared param Type was Enum and
	 *  the template's EnumType was set — handlers read this and use Byte-to-Enum to
	 *  convert to their specific UENUM. Populated only for Enum-typed params; stays
	 *  0 otherwise. The string-side StringValue still carries the raw label. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	uint8 ByteValue = 0;
};

USTRUCT(BlueprintType)
struct FConvaiResultAction
{
	GENERATED_BODY()

	/** The action to be made (canonical name post template-match). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	FString Action;

	/** The actual string of the action without any preprocessing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	FString ActionString;

	/** Insertion-ordered map of placeholder name → typed value. Populated by the
	 *  subsystem from the matched template + LLM's filled-in name/target. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	TMap<FString, FConvaiResultParam> Parameters;

	/** Mirror of FConvaiAction::bWaitForBotSpeech, copied at parse time so the chatbot
	 *  doesn't need a template lookup at dispatch. Only honored when this action lands
	 *  as the first action of a freshly-arrived sequence. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	bool bWaitForBotSpeech = false;

	/** Mirror of FConvaiAction::DelayAfterBotSpeechSec. At runtime treated as 0 when
	 *  bWaitForBotSpeech is false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	float DelayAfterBotSpeechSec = 0.0f;

	/** DEPRECATED — mirrors the first Reference param. Kept for back-compat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API",
		meta = (DeprecatedProperty,
				DeprecationMessage = "Use the Parameters map. RelatedObjectOrCharacter mirrors the first Reference param."))
	FConvaiObjectEntry RelatedObjectOrCharacter;

	/** DEPRECATED — mirrors first numeric / first text / map of strings from Parameters.
	 *  Kept for back-compat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API",
		meta = (DeprecatedProperty,
				DeprecationMessage = "Use the Parameters map (Get First Param / Get Param As Number / String)."))
	FConvaiExtraParams ConvaiExtraParams;
};

USTRUCT(BlueprintType)
struct FConvaiBlendshapeParameters
{
	GENERATED_BODY()

		UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		TArray<FName> TargetNames;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		float Multiplyer = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		float Offset = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		bool UseOverrideValue = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		bool IgnoreGlobalModifiers = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		float OverrideValue = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		float ClampMinValue = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		float ClampMaxValue = 1;
};

USTRUCT()
struct FAnimationFrame
{
	GENERATED_BODY()

	UPROPERTY()
	int32 FrameIndex = 0;

	UPROPERTY()
	TMap<FName, float> BlendShapes;

	FString ToString()
	{
		FString Result;

		// iterate over all elements in the TMap
		for (const auto& Elem : BlendShapes)
		{
			// Append the key-value pair to the result string
			Result += Elem.Key.ToString() + TEXT(": ") + FString::SanitizeFloat(Elem.Value) + TEXT(", ");
		}

		// Remove the trailing comma and space for cleanliness, if present
		if (Result.Len() > 0)
		{
			Result.RemoveAt(Result.Len() - 2);
		}

		return Result;
	}
};

USTRUCT()
struct FAnimationSequence
{
	GENERATED_BODY()
public:
	UPROPERTY()
	TArray<FAnimationFrame> AnimationFrames;

	UPROPERTY()
	double Duration = 0;

	UPROPERTY()
	int32 FrameRate = 0;

	// Serialize this struct to a JSON string
	FString ToJson() const
	{
		TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);

		// Convert AnimationFrames to a JSON array
		TArray<TSharedPtr<FJsonValue>> JsonFrames;
		for (const FAnimationFrame& Frame : AnimationFrames)
		{
			TSharedPtr<FJsonObject> JsonFrameObject = MakeShareable(new FJsonObject);
			JsonFrameObject->SetNumberField(TEXT("FrameIndex"), Frame.FrameIndex);

			// Convert BlendShapes to a JSON object
			TSharedPtr<FJsonObject> JsonBlendShapes = MakeShareable(new FJsonObject);
			for (const auto& Elem : Frame.BlendShapes)
			{
				JsonBlendShapes->SetNumberField(Elem.Key.ToString(), Elem.Value);
			}
			JsonFrameObject->SetObjectField(TEXT("BlendShapes"), JsonBlendShapes);

			JsonFrames.Add(MakeShareable(new FJsonValueObject(JsonFrameObject)));
		}
		JsonObject->SetArrayField(TEXT("AnimationFrames"), JsonFrames);

		// Set the rest of the properties
		JsonObject->SetNumberField(TEXT("Duration"), Duration);
		JsonObject->SetNumberField(TEXT("FrameRate"), FrameRate);

		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

		return OutputString;
	}

	// Deserialize this struct from a JSON string
	bool FromJson(const FString& JsonString)
	{
		AnimationFrames.Empty();
		Duration = 0;
		FrameRate = 0;
		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
		if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonFrames;
			if (JsonObject->TryGetArrayField(TEXT("AnimationFrames"), JsonFrames))
			{
				for (const TSharedPtr<FJsonValue>& JsonValue : *JsonFrames)
				{
					TSharedPtr<FJsonObject> JsonFrameObject = JsonValue->AsObject();
					if (JsonFrameObject.IsValid())
					{
						FAnimationFrame Frame;
						Frame.FrameIndex = JsonFrameObject->GetIntegerField(TEXT("FrameIndex"));

						TSharedPtr<FJsonObject> JsonBlendShapes = JsonFrameObject->GetObjectField(TEXT("BlendShapes"));
						for (const auto& Elem : JsonBlendShapes->Values)
						{
							Frame.BlendShapes.Add(FName(*Elem.Key), Elem.Value->AsNumber());
						}

						AnimationFrames.Add(Frame);
					}
				}
			}

#if ENGINE_MAJOR_VERSION < 5
			double tempDuration;
			JsonObject->TryGetNumberField(TEXT("Duration"), tempDuration);
			Duration = (float)tempDuration;
#else
			JsonObject->TryGetNumberField(TEXT("Duration"), Duration);
#endif
			JsonObject->TryGetNumberField(TEXT("FrameRate"), FrameRate);

			return true;
		}
		return false;
	}
};

USTRUCT()
struct FAnimationSequenceBP
{
	GENERATED_BODY()
public:
	UPROPERTY()
	FAnimationSequence AnimationSequence = FAnimationSequence();
};

/** Result of frame selection calculation for interpolation */
struct FFrameSelectionResult
{
	TMap<FName, float> StartFrame;
	TMap<FName, float> EndFrame;
	float Alpha = 0.0f;
	int32 FrameIndex = 0;
	int32 BufferIndex = 0;
	bool bValid = false;
};

USTRUCT()
struct FConvaiEmotionState
{
	GENERATED_BODY()

public:
	
	// Deprecated
	void GetEmotionDetails(const FString& Emotion, EEmotionIntensity& Intensity, EBasicEmotions& BasicEmotion)
	{
		// Static dictionaries of emotions
		static const TMap<FString, EBasicEmotions> BasicEmotions = {
			{"Joy", EBasicEmotions::Joy},
			{"Trust", EBasicEmotions::Trust},
			{"Fear", EBasicEmotions::Fear},
			{"Surprise", EBasicEmotions::Surprise},
			{"Sadness", EBasicEmotions::Sadness},
			{"Disgust", EBasicEmotions::Disgust},
			{"Anger", EBasicEmotions::Anger},
			{"Anticipation", EBasicEmotions::Anticipation}
		};

		static const TMap<FString, EBasicEmotions> LessIntenseEmotions = {
			{"Serenity", EBasicEmotions::Joy},
			{"Acceptance", EBasicEmotions::Trust},
			{"Apprehension", EBasicEmotions::Fear},
			{"Distraction", EBasicEmotions::Surprise},
			{"Pensiveness", EBasicEmotions::Sadness},
			{"Boredom", EBasicEmotions::Disgust},
			{"Annoyance", EBasicEmotions::Anger},
			{"Interest", EBasicEmotions::Anticipation}
		};

		static const TMap<FString, EBasicEmotions> MoreIntenseEmotions = {
			{"Ecstasy", EBasicEmotions::Joy},
			{"Admiration", EBasicEmotions::Trust},
			{"Terror", EBasicEmotions::Fear},
			{"Amazement", EBasicEmotions::Surprise},
			{"Grief", EBasicEmotions::Sadness},
			{"Loathing", EBasicEmotions::Disgust},
			{"Rage", EBasicEmotions::Anger},
			{"Vigilance", EBasicEmotions::Anticipation}
		};

		// Initialize the output parameters
		Intensity = EEmotionIntensity::None;
		BasicEmotion = EBasicEmotions::None;

		// Look up the emotion
		if (BasicEmotions.Contains(Emotion))
		{
			Intensity = EEmotionIntensity::Basic;
			BasicEmotion = BasicEmotions[Emotion];
		}
		else if (LessIntenseEmotions.Contains(Emotion))
		{
			Intensity = EEmotionIntensity::LessIntense;
			BasicEmotion = LessIntenseEmotions[Emotion];
		}
		else if (MoreIntenseEmotions.Contains(Emotion))
		{
			Intensity = EEmotionIntensity::MoreIntense;
			BasicEmotion = MoreIntenseEmotions[Emotion];
		}
	}

	// Deprecated
	void SetEmotionData(const FString& EmotionRespponse, float EmotionOffset)
	{
		TArray<FString> OutputEmotionsArray;
		// Separate the string into an array based on the space delimiter
		EmotionRespponse.ParseIntoArray(OutputEmotionsArray, TEXT(" "), true);
		SetEmotionData(OutputEmotionsArray, EmotionOffset);
	}

	// Deprecated
	void SetEmotionData(const TArray<FString>& EmotionArray, float EmotionOffset)
	{
		ResetEmotionScores();
		EEmotionIntensity Intensity = EEmotionIntensity::None;
		EBasicEmotions BasicEmotion = EBasicEmotions::None;
		float Score = 0;

		int i = 0;
		for (FString Emotion : EmotionArray)
		{
			GetEmotionDetails(Emotion, Intensity, BasicEmotion);
			if (Intensity == EEmotionIntensity::None || BasicEmotion == EBasicEmotions::None)
				continue;

			if (const float* ScoreMultiplier = ScoreMultipliers.Find(Intensity))
			{
				Score = *ScoreMultiplier * (FMath::Exp(float(-i) / float(EmotionArray.Num())) + EmotionOffset);
				Score = Score > 1 ? 1 : Score;
				Score = Score < 0 ? 0 : Score;
			}
			else
			{
				Score = 0;
			}

			EmotionsScore.Add(BasicEmotion, Score);
			i++;
		}

	}


	void ForceSetEmotion(const EBasicEmotions& BasicEmotion, const EEmotionIntensity& Intensity, const bool& ResetOtherEmotions)
	{
		if (ResetOtherEmotions)
		{
			ResetEmotionScores();
		}

		float Score = 0;
		if (const float* ScoreMultiplier = ScoreMultipliers.Find(Intensity))
		{
			Score = *ScoreMultiplier;
		}
		else
		{
			Score = 0;
		}

		EmotionsScore.Add(BasicEmotion, Score);
	}

	void GetTTSEmotion(const FString& Emotion, EBasicEmotions& BasicEmotion)
	{
		// Static dictionaries of emotions
		static const TMap<FString, EBasicEmotions> BasicEmotions = {
			{"Joy", EBasicEmotions::Joy},
			{"Calm", EBasicEmotions::Trust},
			{"Fear", EBasicEmotions::Fear},
			{"Surprise", EBasicEmotions::Surprise},
			{"Sadness", EBasicEmotions::Sadness},
			{"Bored", EBasicEmotions::Disgust},
			{"Anger", EBasicEmotions::Anger},
			{"Neutral", EBasicEmotions::None}
		};

		// Initialize the output parameters
		BasicEmotion = EBasicEmotions::None;

		// Look up the emotion
		if (BasicEmotions.Contains(Emotion))
		{
			BasicEmotion = BasicEmotions[Emotion];
		}
	}


	void SetEmotionDataSingleEmotion(const FString& EmotionRespponse, float EmotionOffset)
	{
		FString EmotionString;
		float Scale;
		float MaxScale = 3;
		ParseStringToStringAndFloat(EmotionRespponse, EmotionString, Scale);


		Scale /= MaxScale;

		// Increase the scale a bit
		Scale += EmotionOffset;
		Scale = Scale > 1? 1 : Scale;
		Scale = Scale < 0? 0 : Scale;

		EBasicEmotions Emotion;
		GetTTSEmotion(EmotionString, Emotion);

		ResetEmotionScores();
		EmotionsScore.Add(Emotion, Scale);
	}
	float GetEmotionScore(const EBasicEmotions& Emotion)
	{
		float Score = 0;
		if (const float* ScorePointer = EmotionsScore.Find(Emotion))
		{
			Score = *ScorePointer;
		}
		return Score;
	}

	void ResetEmotionScores()
	{
		EmotionsScore.Empty();

		for (int32 i = 0; i <= static_cast<int32>(EBasicEmotions::Anticipation); ++i)
		{
			EBasicEmotions EnumValue = static_cast<EBasicEmotions>(i);
			EmotionsScore.Add(EnumValue, 0);
		}
	}

private:
	TMap<EBasicEmotions, float> EmotionsScore;

	static const TMap<EEmotionIntensity, float> ScoreMultipliers;

	bool ParseStringToStringAndFloat(const FString& Input, FString& OutString, float& OutFloat)
	{
		// Split the input string into two parts based on the space character
		TArray<FString> Parsed;
		Input.ParseIntoArray(Parsed, TEXT(" "), true);

		// Check if the parsing was successful
		if (Parsed.Num() == 2)
		{
			// Assign the first part to OutString
			OutString = Parsed[0];

			// Convert the second part to a float and assign it to OutFloat
			OutFloat = FCString::Atof(*Parsed[1]);
			return true;
		}
		else
		{
			// Handle the error case
			OutString = TEXT("");
			OutFloat = 0.0f;
			return false;
		}
	}
};


namespace ConvaiConstants
{
	enum
	{
		// Buffer sizes
		VoiceCaptureRingBufferCapacity = 1024 * 1024,
		VoiceCaptureBufferSize = 1024 * 1024,
		LipSyncBufferSize = 1024 * 100, // aproximately 1024 seconds for OVR
		VoiceCaptureSampleRate = 48000,
		VoiceCaptureChunk = 2084,
		VoiceStreamMaxChunk = 4096,
		PlayerTimeOut = 2500 /* 2500 ms*/,
		ChatbotTimeOut = 6000 /* 6000 ms*/,
		WebRTCAudioSampleRate = 48000 /* 16 kHz */
	};
	
	// OVR Viseme names (15 visemes) - used for VisemeBased mode
	const TArray<FString> VisemeNames = { "sil", "PP", "FF", "TH", "DD", "kk", "CH", "SS", "nn", "RR", "aa", "E", "ih", "oh", "ou" };
	
	// ARKit / Apple blendshape names (52 blendshapes) - used for BS_ArKIT mode
	const TArray<FString> ARKitBlendShapesNames = { "EyeBlinkLeft", "EyeLookDownLeft", "EyeLookInLeft", "EyeLookOutLeft", "EyeLookUpLeft", "EyeSquintLeft", "EyeWideLeft", "EyeBlinkRight", "EyeLookDownRight", "EyeLookInRight", "EyeLookOutRight", "EyeLookUpRight", "EyeSquintRight", "EyeWideRight", "JawForward", "JawRight", "JawLeft", "JawOpen", "MouthClose", "MouthFunnel", "MouthPucker", "MouthRight", "MouthLeft", "MouthSmileLeft", "MouthSmileRight", "MouthFrownLeft", "MouthFrownRight", "MouthDimpleLeft", "MouthDimpleRight", "MouthStretchLeft", "MouthStretchRight", "MouthRollLower", "MouthRollUpper", "MouthShrugLower", "MouthShrugUpper", "MouthPressLeft", "MouthPressRight", "MouthLowerDownLeft", "MouthLowerDownRight", "MouthUpperUpLeft", "MouthUpperUpRight", "BrowDownLeft", "BrowDownRight", "BrowInnerUp", "BrowOuterUpLeft", "BrowOuterUpRight", "CheekPuff", "CheekSquintLeft", "CheekSquintRight", "NoseSneerLeft", "NoseSneerRight", "TongueOut", "HeadYaw", "HeadPitch", "HeadRoll", "LeftEyeYaw", "LeftEyePitch", "LeftEyeRoll", "RightEyeYaw", "RightEyePitch", "RightEyeRoll" };
	
	// MetaHuman CTRL curve names - used for BS_MHA mode
	const TArray<FString> MetaHumanCtrlNames = { TEXT("CTRL_expressions_browDownL"), TEXT("CTRL_expressions_browDownR"), TEXT("CTRL_expressions_browLateralL"), TEXT("CTRL_expressions_browLateralR"), TEXT("CTRL_expressions_browRaiseInL"), TEXT("CTRL_expressions_browRaiseInR"), TEXT("CTRL_expressions_browRaiseOuterL"), TEXT("CTRL_expressions_browRaiseOuterR"), TEXT("CTRL_expressions_earUpL"), TEXT("CTRL_expressions_earUpR"), TEXT("CTRL_expressions_eyeBlinkL"), TEXT("CTRL_expressions_eyeBlinkR"), TEXT("CTRL_expressions_eyeCheekRaiseL"), TEXT("CTRL_expressions_eyeCheekRaiseR"), TEXT("CTRL_expressions_eyeFaceScrunchL"), TEXT("CTRL_expressions_eyeFaceScrunchR"), TEXT("CTRL_expressions_eyeLidPressL"), TEXT("CTRL_expressions_eyeLidPressR"), TEXT("CTRL_expressions_eyeLookDownL"), TEXT("CTRL_expressions_eyeLookDownR"), TEXT("CTRL_expressions_eyeLookLeftL"), TEXT("CTRL_expressions_eyeLookLeftR"), TEXT("CTRL_expressions_eyeLookRightL"), TEXT("CTRL_expressions_eyeLookRightR"), TEXT("CTRL_expressions_eyeLookUpL"), TEXT("CTRL_expressions_eyeLookUpR"), TEXT("CTRL_expressions_eyeLowerLidDownL"), TEXT("CTRL_expressions_eyeLowerLidDownR"), TEXT("CTRL_expressions_eyeLowerLidUpL"), TEXT("CTRL_expressions_eyeLowerLidUpR"), TEXT("CTRL_expressions_eyeParallelLookDirection"), TEXT("CTRL_expressions_eyePupilNarrowL"), TEXT("CTRL_expressions_eyePupilNarrowR"), TEXT("CTRL_expressions_eyePupilWideL"), TEXT("CTRL_expressions_eyePupilWideR"), TEXT("CTRL_expressions_eyeRelaxL"), TEXT("CTRL_expressions_eyeRelaxR"), TEXT("CTRL_expressions_eyeSquintInnerL"), TEXT("CTRL_expressions_eyeSquintInnerR"), TEXT("CTRL_expressions_eyeUpperLidUpL"), TEXT("CTRL_expressions_eyeUpperLidUpR"), TEXT("CTRL_expressions_eyeWidenL"), TEXT("CTRL_expressions_eyeWidenR"), TEXT("CTRL_expressions_eyelashesDownINL"), TEXT("CTRL_expressions_eyelashesDownINR"), TEXT("CTRL_expressions_eyelashesDownOUTL"), TEXT("CTRL_expressions_eyelashesDownOUTR"), TEXT("CTRL_expressions_eyelashesUpINL"), TEXT("CTRL_expressions_eyelashesUpINR"), TEXT("CTRL_expressions_eyelashesUpOUTL"), TEXT("CTRL_expressions_eyelashesUpOUTR"), TEXT("CTRL_expressions_jawBack"), TEXT("CTRL_expressions_jawChinCompressL"), TEXT("CTRL_expressions_jawChinCompressR"), TEXT("CTRL_expressions_jawChinRaiseDL"), TEXT("CTRL_expressions_jawChinRaiseDR"), TEXT("CTRL_expressions_jawChinRaiseUL"), TEXT("CTRL_expressions_jawChinRaiseUR"), TEXT("CTRL_expressions_jawClenchL"), TEXT("CTRL_expressions_jawClenchR"), TEXT("CTRL_expressions_jawFwd"), TEXT("CTRL_expressions_jawLeft"), TEXT("CTRL_expressions_jawOpen"), TEXT("CTRL_expressions_jawOpenExtreme"), TEXT("CTRL_expressions_jawRight"), TEXT("CTRL_expressions_mouthCheekBlowL"), TEXT("CTRL_expressions_mouthCheekBlowR"), TEXT("CTRL_expressions_mouthCheekSuckL"), TEXT("CTRL_expressions_mouthCheekSuckR"), TEXT("CTRL_expressions_mouthCornerDepressL"), TEXT("CTRL_expressions_mouthCornerDepressR"), TEXT("CTRL_expressions_mouthCornerDownL"), TEXT("CTRL_expressions_mouthCornerDownR"), TEXT("CTRL_expressions_mouthCornerNarrowL"), TEXT("CTRL_expressions_mouthCornerNarrowR"), TEXT("CTRL_expressions_mouthCornerPullL"), TEXT("CTRL_expressions_mouthCornerPullR"), TEXT("CTRL_expressions_mouthCornerRounderDL"), TEXT("CTRL_expressions_mouthCornerRounderDR"), TEXT("CTRL_expressions_mouthCornerRounderUL"), TEXT("CTRL_expressions_mouthCornerRounderUR"), TEXT("CTRL_expressions_mouthCornerSharpenDL"), TEXT("CTRL_expressions_mouthCornerSharpenDR"), TEXT("CTRL_expressions_mouthCornerSharpenUL"), TEXT("CTRL_expressions_mouthCornerSharpenUR"), TEXT("CTRL_expressions_mouthCornerUpL"), TEXT("CTRL_expressions_mouthCornerUpR"), TEXT("CTRL_expressions_mouthCornerWideL"), TEXT("CTRL_expressions_mouthCornerWideR"), TEXT("CTRL_expressions_mouthDimpleL"), TEXT("CTRL_expressions_mouthDimpleR"), TEXT("CTRL_expressions_mouthDown"), TEXT("CTRL_expressions_mouthFunnelDL"), TEXT("CTRL_expressions_mouthFunnelDR"), TEXT("CTRL_expressions_mouthFunnelUL"), TEXT("CTRL_expressions_mouthFunnelUR"), TEXT("CTRL_expressions_mouthLeft"), TEXT("CTRL_expressions_mouthLipsBlowL"), TEXT("CTRL_expressions_mouthLipsBlowR"), TEXT("CTRL_expressions_mouthLipsPressL"), TEXT("CTRL_expressions_mouthLipsPressR"), TEXT("CTRL_expressions_mouthLipsPullDL"), TEXT("CTRL_expressions_mouthLipsPullDR"), TEXT("CTRL_expressions_mouthLipsPullUL"), TEXT("CTRL_expressions_mouthLipsPullUR"), TEXT("CTRL_expressions_mouthLipsPurseDL"), TEXT("CTRL_expressions_mouthLipsPurseDR"), TEXT("CTRL_expressions_mouthLipsPurseUL"), TEXT("CTRL_expressions_mouthLipsPurseUR"), TEXT("CTRL_expressions_mouthLipsPushDL"), TEXT("CTRL_expressions_mouthLipsPushDR"), TEXT("CTRL_expressions_mouthLipsPushUL"), TEXT("CTRL_expressions_mouthLipsPushUR"), TEXT("CTRL_expressions_mouthLipsStickyLPh1"), TEXT("CTRL_expressions_mouthLipsStickyLPh2"), TEXT("CTRL_expressions_mouthLipsStickyLPh3"), TEXT("CTRL_expressions_mouthLipsStickyRPh1"), TEXT("CTRL_expressions_mouthLipsStickyRPh2"), TEXT("CTRL_expressions_mouthLipsStickyRPh3"), TEXT("CTRL_expressions_mouthLipsThickDL"), TEXT("CTRL_expressions_mouthLipsThickDR"), TEXT("CTRL_expressions_mouthLipsThickInwardDL"), TEXT("CTRL_expressions_mouthLipsThickInwardDR"), TEXT("CTRL_expressions_mouthLipsThickInwardUL"), TEXT("CTRL_expressions_mouthLipsThickInwardUR"), TEXT("CTRL_expressions_mouthLipsThickUL"), TEXT("CTRL_expressions_mouthLipsThickUR"), TEXT("CTRL_expressions_mouthLipsThinDL"), TEXT("CTRL_expressions_mouthLipsThinDR"), TEXT("CTRL_expressions_mouthLipsThinInwardDL"), TEXT("CTRL_expressions_mouthLipsThinInwardDR"), TEXT("CTRL_expressions_mouthLipsThinInwardUL"), TEXT("CTRL_expressions_mouthLipsThinInwardUR"), TEXT("CTRL_expressions_mouthLipsThinUL"), TEXT("CTRL_expressions_mouthLipsThinUR"), TEXT("CTRL_expressions_mouthLipsTightenDL"), TEXT("CTRL_expressions_mouthLipsTightenDR"), TEXT("CTRL_expressions_mouthLipsTightenUL"), TEXT("CTRL_expressions_mouthLipsTightenUR"), TEXT("CTRL_expressions_mouthLipsTogetherDL"), TEXT("CTRL_expressions_mouthLipsTogetherDR"), TEXT("CTRL_expressions_mouthLipsTogetherUL"), TEXT("CTRL_expressions_mouthLipsTogetherUR"), TEXT("CTRL_expressions_mouthLipsTowardsDL"), TEXT("CTRL_expressions_mouthLipsTowardsDR"), TEXT("CTRL_expressions_mouthLipsTowardsUL"), TEXT("CTRL_expressions_mouthLipsTowardsUR"), TEXT("CTRL_expressions_mouthLowerLipBiteL"), TEXT("CTRL_expressions_mouthLowerLipBiteR"), TEXT("CTRL_expressions_mouthLowerLipDepressL"), TEXT("CTRL_expressions_mouthLowerLipDepressR"), TEXT("CTRL_expressions_mouthLowerLipRollInL"), TEXT("CTRL_expressions_mouthLowerLipRollInR"), TEXT("CTRL_expressions_mouthLowerLipRollOutL"), TEXT("CTRL_expressions_mouthLowerLipRollOutR"), TEXT("CTRL_expressions_mouthLowerLipShiftLeft"), TEXT("CTRL_expressions_mouthLowerLipShiftRight"), TEXT("CTRL_expressions_mouthLowerLipTowardsTeethL"), TEXT("CTRL_expressions_mouthLowerLipTowardsTeethR"), TEXT("CTRL_expressions_mouthPressDL"), TEXT("CTRL_expressions_mouthPressDR"), TEXT("CTRL_expressions_mouthPressUL"), TEXT("CTRL_expressions_mouthPressUR"), TEXT("CTRL_expressions_mouthRight"), TEXT("CTRL_expressions_mouthSharpCornerPullL"), TEXT("CTRL_expressions_mouthSharpCornerPullR"), TEXT("CTRL_expressions_mouthStickyDC"), TEXT("CTRL_expressions_mouthStickyDINL"), TEXT("CTRL_expressions_mouthStickyDINR"), TEXT("CTRL_expressions_mouthStickyDOUTL"), TEXT("CTRL_expressions_mouthStickyDOUTR"), TEXT("CTRL_expressions_mouthStickyUC"), TEXT("CTRL_expressions_mouthStickyUINL"), TEXT("CTRL_expressions_mouthStickyUINR"), TEXT("CTRL_expressions_mouthStickyUOUTL"), TEXT("CTRL_expressions_mouthStickyUOUTR"), TEXT("CTRL_expressions_mouthStretchL"), TEXT("CTRL_expressions_mouthStretchLipsCloseL"), TEXT("CTRL_expressions_mouthStretchLipsCloseR"), TEXT("CTRL_expressions_mouthStretchR"), TEXT("CTRL_expressions_mouthUp"), TEXT("CTRL_expressions_mouthUpperLipBiteL"), TEXT("CTRL_expressions_mouthUpperLipBiteR"), TEXT("CTRL_expressions_mouthUpperLipRaiseL"), TEXT("CTRL_expressions_mouthUpperLipRaiseR"), TEXT("CTRL_expressions_mouthUpperLipRollInL"), TEXT("CTRL_expressions_mouthUpperLipRollInR"), TEXT("CTRL_expressions_mouthUpperLipRollOutL"), TEXT("CTRL_expressions_mouthUpperLipRollOutR"), TEXT("CTRL_expressions_mouthUpperLipShiftLeft"), TEXT("CTRL_expressions_mouthUpperLipShiftRight"), TEXT("CTRL_expressions_mouthUpperLipTowardsTeethL"), TEXT("CTRL_expressions_mouthUpperLipTowardsTeethR"), TEXT("CTRL_expressions_neckDigastricDown"), TEXT("CTRL_expressions_neckDigastricUp"), TEXT("CTRL_expressions_neckMastoidContractL"), TEXT("CTRL_expressions_neckMastoidContractR"), TEXT("CTRL_expressions_neckStretchL"), TEXT("CTRL_expressions_neckStretchR"), TEXT("CTRL_expressions_neckSwallowPh1"), TEXT("CTRL_expressions_neckSwallowPh2"), TEXT("CTRL_expressions_neckSwallowPh3"), TEXT("CTRL_expressions_neckSwallowPh4"), TEXT("CTRL_expressions_neckThroatDown"), TEXT("CTRL_expressions_neckThroatExhale"), TEXT("CTRL_expressions_neckThroatInhale"), TEXT("CTRL_expressions_neckThroatUp"), TEXT("CTRL_expressions_noseNasolabialDeepenL"), TEXT("CTRL_expressions_noseNasolabialDeepenR"), TEXT("CTRL_expressions_noseNostrilCompressL"), TEXT("CTRL_expressions_noseNostrilCompressR"), TEXT("CTRL_expressions_noseNostrilDepressL"), TEXT("CTRL_expressions_noseNostrilDepressR"), TEXT("CTRL_expressions_noseNostrilDilateL"), TEXT("CTRL_expressions_noseNostrilDilateR"), TEXT("CTRL_expressions_noseWrinkleL"), TEXT("CTRL_expressions_noseWrinkleR"), TEXT("CTRL_expressions_noseWrinkleUpperL"), TEXT("CTRL_expressions_noseWrinkleUpperR"), TEXT("CTRL_expressions_teethBackD"), TEXT("CTRL_expressions_teethBackU"), TEXT("CTRL_expressions_teethDownD"), TEXT("CTRL_expressions_teethDownU"), TEXT("CTRL_expressions_teethFwdD"), TEXT("CTRL_expressions_teethFwdU"), TEXT("CTRL_expressions_teethLeftD"), TEXT("CTRL_expressions_teethLeftU"), TEXT("CTRL_expressions_teethRightD"), TEXT("CTRL_expressions_teethRightU"), TEXT("CTRL_expressions_teethUpD"), TEXT("CTRL_expressions_teethUpU"), TEXT("CTRL_expressions_tongueBendDown"), TEXT("CTRL_expressions_tongueBendUp"), TEXT("CTRL_expressions_tongueDown"), TEXT("CTRL_expressions_tongueIn"), TEXT("CTRL_expressions_tongueLeft"), TEXT("CTRL_expressions_tongueNarrow"), TEXT("CTRL_expressions_tongueOut"), TEXT("CTRL_expressions_tonguePress"), TEXT("CTRL_expressions_tongueRight"), TEXT("CTRL_expressions_tongueRoll"), TEXT("CTRL_expressions_tongueThick"), TEXT("CTRL_expressions_tongueThin"), TEXT("CTRL_expressions_tongueTipDown"), TEXT("CTRL_expressions_tongueTipLeft"), TEXT("CTRL_expressions_tongueTipRight"), TEXT("CTRL_expressions_tongueTipUp"), TEXT("CTRL_expressions_tongueTwistLeft"), TEXT("CTRL_expressions_tongueTwistRight"), TEXT("CTRL_expressions_tongueUp"), TEXT("CTRL_expressions_tongueWide") };
	
	// CC4 Extended curve names - used for BS_CC4_Extended mode
	const TArray<FString> CC4ExtendedNames = { "Mouth_Drop_Lower", "Mouth_Up_Upper_L", "Mouth_Up_Upper_R", "Mouth_Contract", "Tongue_Out", "Tongue_In", "Tongue_Up", "Tongue_Down", "Tongue_Mid_Up", "Tongue_Tip_Up", "Tongue_Tip_Down", "Tongue_Narrow", "Tongue_Wide", "Tongue_Roll", "Tongue_L", "Tongue_R", "Tongue_Tip_L", "Tongue_Tip_R", "Tongue_Twist_L", "Tongue_Twist_R", "Tongue_Bulge_L", "Tongue_Bulge_R", "Tongue_Extend", "Tongue_Enlarge", "Jaw_Forward", "Jaw_L", "Jaw_R", "Jaw_Up", "Jaw_Down", "Head_Turn_Up", "Head_Turn_Down", "Head_Tilt_L", "Head_L", "Head_Backward", "Brow_Raise_Inner_L", "Brow_Raise_Inner_R", "Brow_Raise_Outer_L", "Brow_Raise_Outer_R", "Brow_Drop_L", "Brow_Drop_R", "Brow_Compress_L", "Brow_Compress_R", "Eye_Blink_L", "Eye_Blink_R", "Eye_Squint_L", "Eye_Squint_R", "Eye_Wide_L", "Eye_Wide_R", "Eye_L_Look_L", "Eye_R_Look_L", "Eye_L_Look_R", "Eye_R_Look_R", "Eye_L_Look_Up", "Eye_R_Look_Up", "Eye_L_Look_Down", "Eye_R_Look_Down", "Eyelash_Upper_Up_L", "Eyelash_Upper_Down_L", "Eyelash_Upper_Up_R", "Eyelash_Upper_Down_R", "Eyelash_Lower_Up_L", "Eyelash_Lower_Down_L", "Eyelash_Lower_Up_R", "Eyelash_Lower_Down_R", "Ear_Up_L", "Ear_Up_R", "Ear_Down_L", "Ear_Down_R", "Ear_Out_L", "Ear_Out_R", "Nose_Sneer_L", "Nose_Sneer_R", "Nose_Nostril_Raise_L", "Nose_Nostril_Raise_R", "Nose_Nostril_Dilate_L", "Nose_Nostril_Dilate_R", "Nose_Crease_L", "Nose_Crease_R", "Nose_Nostril_Down_L", "Nose_Nostril_Down_R", "Nose_Nostril_In_L", "Nose_Nostril_In_R", "Nose_Tip_L", "Nose_Tip_R", "Nose_Tip_Up", "Nose_Tip_Down", "Cheek_Raise_L", "Cheek_Raise_R", "Cheek_Suck_L", "Cheek_Suck_R", "Cheek_Puff_L", "Cheek_Puff_R", "Mouth_Smile_L", "Mouth_Smile_R", "Mouth_Smile_Sharp_L", "Mouth_Smile_Sharp_R", "Mouth_Frown_L", "Mouth_Frown_R", "Mouth_Stretch_L", "Mouth_Stretch_R", "Mouth_Dimple_L", "Mouth_Dimple_R", "Mouth_Press_L", "Mouth_Press_R", "Mouth_Tighten_L", "Mouth_Tighten_R", "Mouth_Blow_L", "Mouth_Blow_R", "Mouth_Pucker_Up_L", "Mouth_Pucker_Up_R", "Mouth_Pucker_Down_L", "Mouth_Pucker_Down_R", "Mouth_Funnel_Up_L", "Mouth_Funnel_Up_R", "Mouth_Funnel_Down_L", "Mouth_Funnel_Down_R", "Mouth_Roll_In_Upper_L", "Mouth_Roll_In_Upper_R", "Mouth_Roll_In_Lower_L", "Mouth_Roll_In_Lower_R", "Mouth_Roll_Out_Upper_L", "Mouth_Roll_Out_Upper_R", "Mouth_Roll_Out_Lower_L", "Mouth_Roll_Out_Lower_R", "Mouth_Push_Upper_L", "Mouth_Push_Upper_R", "Mouth_Push_Lower_L", "Mouth_Push_Lower_R", "Mouth_Pull_Upper_L", "Mouth_Pull_Upper_R", "Mouth_Pull_Lower_L", "Mouth_Pull_Lower_R", "Mouth_Up", "Mouth_Down", "Mouth_L", "Mouth_R", "Mouth_Upper_L", "Mouth_Upper_R", "Mouth_Lower_L", "Mouth_Lower_R", "Mouth_Shrug_Upper", "Mouth_Shrug_Lower", "Mouth_Drop_Upper", "Mouth_Down_Lower_L", "Mouth_Down_Lower_R", "Mouth_Chin_Up", "Mouth_Close", "Jaw_Open", "Jaw_Backward", "Neck_Swallow_Up", "Neck_Swallow_Down", "Neck_Tighten_L", "Neck_Tighten_R", "Head_Turn_L", "Head_Turn_R", "Head_Tilt_R", "Head_R", "Head_Forward", "eye_shape_L", "eye_shape_R", "eye_shape_angry_L", "eye_shape_angry_R", "double_eyelid_up_L", "double_eyelid_up_R", "lips_smooth_lower", "lips_curve_shape_lower", "eyes_smile_shape_L", "eyes_smile_shape_R", "smile_coner_shape_L", "smile_coner_shape_R" };
	
	const FString API_Key_Header = "CONVAI-API-KEY";
	const FString Auth_Token_Header = "API-AUTH-TOKEN";
	const FString X_API_KEY_HEADER = "X-API-KEY";
	
	namespace WebRTC
	{
		namespace label
		{
			constexpr const char* Default       = "rtvi-ai";
		}
		
		namespace MessageType
		{
			constexpr const char* UserTextMessage    = "user_text_message";
			constexpr const char* TriggerMessage     = "trigger-message";
			constexpr const char* UpdateTemplateKeys = "update-template-keys";
			constexpr const char* UpdateDynamicInfo  = "update-dynamic-info";
			constexpr const char* STTToggle          = "stt-toggle";
			constexpr const char* ContextUpdate      = "context-update";
			constexpr const char* ResetIdleTimer      = "reset-idle-timer";
			constexpr const char* UpdateSceneMetadata = "update-scene-metadata";
		}
		
	}
};


template<typename DelegateType>
class FThreadSafeDelegateWrapper
{
public:
	// Bind a delegate
	void Bind(const DelegateType& InDelegate)
	{
		FScopeLock Lock(&Mutex);
		MyDelegate = InDelegate;
	}

	// Mirror of BindUObject function for non-const UserClass
	template <typename UserClass, typename... VarTypes>
	void BindUObject(UserClass* InUserObject, void(UserClass::* InFunc)(VarTypes...))
	{
		FScopeLock Lock(&Mutex);
		MyDelegate.BindUObject(InUserObject, InFunc);
	}

	// Unbind the delegate
	void Unbind()
	{
		FScopeLock Lock(&Mutex);
		MyDelegate.Unbind();
	}

	// Check if the delegate is bound
	bool IsBound() const
	{
		return MyDelegate.IsBound();
	}

	// Execute the delegate if it is bound
	// Use perfect forwarding to forward arguments to the delegate
	template<typename... ArgTypes>
	void ExecuteIfBound(ArgTypes&&... Args) const
	{
		FScopeLock Lock(&Mutex);
		if (MyDelegate.IsBound() && !IsEngineExitRequested())
		{
			MyDelegate.ExecuteIfBound(Forward<ArgTypes>(Args)...);
		}
	}

private:
	mutable FCriticalSection Mutex;
	DelegateType MyDelegate;
};

// Enum for Voice Types
UENUM(BlueprintType)
enum class EVoiceType : uint8
{
	AzureVoices           UMETA(DisplayName = "Azure Voices"),
	ElevenLabsVoices      UMETA(DisplayName = "ElevenLabs Voices"),
	GCPVoices             UMETA(DisplayName = "GCP Voices"),
	ConvaiVoices          UMETA(DisplayName = "Convai Voices"),
	OpenAIVoices          UMETA(DisplayName = "OpenAI Voices"),
	ConvaiVoicesNew       UMETA(DisplayName = "Convai Voices (New)"),
	ConvaiVoicesExperimental UMETA(DisplayName = "Convai Voices (Experimental)")
};

// Enum for Languages
UENUM(BlueprintType)
enum class ELanguageType : uint8
{
	Arabic               UMETA(DisplayName = "Arabic"),
	ChineseCantonese     UMETA(DisplayName = "Chinese (Cantonese)"),
	ChineseMandarin      UMETA(DisplayName = "Chinese (Mandarin)"),
	Dutch                UMETA(DisplayName = "Dutch"),
	DutchBelgium         UMETA(DisplayName = "Dutch (Belgium)"),
	English              UMETA(DisplayName = "English"),
	Finnish              UMETA(DisplayName = "Finnish"),
	French               UMETA(DisplayName = "French"),
	German               UMETA(DisplayName = "German"),
	Hindi                UMETA(DisplayName = "Hindi"),
	Italian              UMETA(DisplayName = "Italian"),
	Japanese             UMETA(DisplayName = "Japanese"),
	Korean               UMETA(DisplayName = "Korean"),
	Polish               UMETA(DisplayName = "Polish"),
	PortugueseBrazil     UMETA(DisplayName = "Portuguese (Brazil)"),
	PortuguesePortugal   UMETA(DisplayName = "Portuguese (Portugal)"),
	Russian              UMETA(DisplayName = "Russian"),
	Spanish              UMETA(DisplayName = "Spanish"),
	SpanishMexico        UMETA(DisplayName = "Spanish (Mexico)"),
	SpanishUS            UMETA(DisplayName = "Spanish (US)"),
	Swedish              UMETA(DisplayName = "Swedish"),
	Turkish              UMETA(DisplayName = "Turkish"),
	Vietnamese           UMETA(DisplayName = "Vietnamese")
};

// Enum for Gender
UENUM(BlueprintType)
enum class EGenderType : uint8
{
	Male    UMETA(DisplayName = "Male"),
	Female  UMETA(DisplayName = "Female")
};


USTRUCT(BlueprintType)
struct FVoiceLanguageStruct
{
	GENERATED_BODY()
	
	//UPROPERTY(BlueprintReadOnly)
	FString VoiceType;

	//UPROPERTY(BlueprintReadOnly)
	FString VoiceName;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Language")
	FString VoiceValue;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Language")
	TArray<FString> LangCodes;

	//UPROPERTY(BlueprintReadOnly)
	FString Gender;

	FVoiceLanguageStruct() {}
};


// LTM
USTRUCT(BlueprintType)
struct FConvaiSpeakerInfo
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speaker Info")
	FString SpeakerID;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speaker Info")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speaker Info")
	FString DeviceID;
	
	FConvaiSpeakerInfo()
		: SpeakerID(TEXT(""))
		, Name(TEXT(""))
		, DeviceID(TEXT(""))
	{
	}
};

UENUM(BlueprintType)
enum class EC_ConnectionState : uint8
{
	Disconnected UMETA(DisplayName = "Disconnected"),
	Connecting   UMETA(DisplayName = "Connecting"),
	Connected    UMETA(DisplayName = "Connected"),
	Reconnecting UMETA(DisplayName = "Reconnecting"),
};

// Forward declaration
namespace convai {
	class ConvaiClient;
}

USTRUCT(BlueprintType)
struct CONVAI_API FConvaiVADSettings
{
	GENERATED_BODY()

	/** Master gate. When true, all per-field values below are ignored and the server applies its defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Audio Settings|VAD")
	bool bUseServerDefault = true;

	/** Minimum VAD model probability that a frame contains speech. Higher = stricter "is this speech?" check. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Audio Settings|VAD",
	          meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "!bUseServerDefault"))
	float Confidence = 0.7f;

	/** Seconds of sustained speech before "user started speaking" fires. Higher = ignores brief bursts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Audio Settings|VAD",
	          meta = (ClampMin = "0.0", EditCondition = "!bUseServerDefault"))
	float StartSecs = 0.2f;

	/** Seconds of silence before "user stopped speaking" fires. Lower = faster end-of-turn, more risk of cuts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Audio Settings|VAD",
	          meta = (ClampMin = "0.0", EditCondition = "!bUseServerDefault"))
	float StopSecs = 2.2f;

	/** Amplitude floor — normalized audio below this level is treated as silence. Primary lever for rejecting background voices. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Audio Settings|VAD",
	          meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "!bUseServerDefault"))
	float MinVolume = 0.6f;
};

/**
 * Per-frame audio analysis snapshot — produced by UConvaiUtils::ContainsActualAudio.
 *
 * Plain C++ struct (not a USTRUCT) because the function it pairs with takes raw
 * int16_t* PCM and can't be a UFUNCTION anyway. Callers cache one between calls
 * so the analyser can see frame-to-frame deltas (e.g. a sudden RMS drop marking
 * the tail of speech).
 */
struct CONVAI_API FConvaiAudioFrameStats
{
	/** Peak |sample| seen in this frame, 0..32767. 0 means no signal whatsoever. */
	int32 PeakAmplitude = 0;

	/** Root-mean-square magnitude — frame loudness. Monotonic in linear PCM, so
	 *  it's the cleanest "is this loud" number to compare across frames. */
	float RMS = 0.0f;

	/** Sample variance around the mean — signal-modulation indicator. Speech
	 *  carries high variance (rapidly-changing waveform); a steady hum or DC
	 *  offset registers low. Used together with RMS to distinguish real audio
	 *  from background noise that happens to clear a peak threshold. */
	float Variance = 0.0f;

	/** Samples examined producing the values above. */
	int32 SampleCount = 0;

	/** Cached decision from the ContainsActualAudio call that filled this
	 *  struct — saves callers having to re-derive it. */
	bool bHasContent = false;
};

USTRUCT()
struct CONVAI_API FConvaiConnectionParams
{
	GENERATED_BODY()

public:
	/** ConvaiClient pointer for the connection */
	convai::ConvaiClient* Client;

	/** Character ID for the connection */
	FString CharacterID;

	/** LLM provider for the connection */
	FString LLMProvider;

	/** Connection type for the connection */
	FString ConnectionType;

	/** Blendshape provider (e.g., "neurosync", "ovr", "not_provided") */
	FString BlendshapeProvider;

	/** Blendshape format (e.g., "mha" for MetaHuman, "arkit" for ARKit) */
	FString BlendshapeFormat;

	/** Emotion provider (e.g., "neurosync", "not_provided") */
	FString EmotionProvider;

	/** End User ID for long term memory (LTM) */
	FString EndUserID;

	/** End User Metadata as a JSON string for long term memory (LTM) */
	FString EndUserMetadata;

	/** Optional action_config JSON string sent at /connect time. Empty = action_config not sent. */
	FString ActionConfigJson;

	int32 ChunkSize;

	int32 OutputFPS;

	float FramesBufferDuration;

	/** Server-side VAD overrides. -1.0f for any field means "use server default". */
	float VADConfidence;
	float VADStartSecs;
	float VADStopSecs;
	float VADMinVolume;

	FConvaiConnectionParams()
		: Client(nullptr)
		, CharacterID(TEXT(""))
		, LLMProvider(TEXT("dynamic"))
		, ConnectionType(TEXT("audio"))
		, BlendshapeProvider(TEXT("not_provided"))
		, BlendshapeFormat(TEXT(""))
		, EmotionProvider(TEXT("nrclex"))
		, EndUserID(TEXT(""))
		, EndUserMetadata(TEXT(""))
		, ChunkSize(10)
		, OutputFPS(90)
		, FramesBufferDuration(0.0f)
		, VADConfidence(-1.0f)
		, VADStartSecs(-1.0f)
		, VADStopSecs(-1.0f)
		, VADMinVolume(-1.0f)
	{
	}

	FConvaiConnectionParams(convai::ConvaiClient* InClient, const FString& InCharacterID, 
		const FString& InLLMProvider = TEXT("dynamic"), const FString& InConnectionType = TEXT("audio"),
		const FString& InBlendshapeProvider = TEXT("not_provided"), const FString& InBlendshapeFormat = TEXT(""),
		const FString& InEmotionProvider = TEXT("nrclex"),
		const FString& InEndUserID = TEXT(""), const FString& InEndUserMetadata = TEXT(""),
		int32 InChunkSize = 10, int32 InOutputFPS = 90, float InFramesBufferDuration = 0.0f)
		: Client(InClient)
		, CharacterID(InCharacterID)
		, LLMProvider(InLLMProvider)
		, ConnectionType(InConnectionType)
		, BlendshapeProvider(InBlendshapeProvider)
		, BlendshapeFormat(InBlendshapeFormat)
		, EmotionProvider(InEmotionProvider)
		, EndUserID(InEndUserID)
		, EndUserMetadata(InEndUserMetadata)
		, ChunkSize(InChunkSize)
		, OutputFPS(InOutputFPS)
		, FramesBufferDuration(InFramesBufferDuration)
	{
	}

	/**
	 * Creates connection parameters by determining the appropriate settings
	 * @param InClient - The ConvaiClient instance
	 * @param InCharacterID - The character ID to connect to
	 * @param SessionProxy - The session proxy to determine settings from
	 * @return Configured connection parameters
	 */
	static FConvaiConnectionParams Create(convai::ConvaiClient* InClient, const FString& InCharacterID, class UConvaiConnectionSessionProxy* SessionProxy);
};

UENUM(BlueprintType)
enum class EC_LipSyncMode : uint8
{
	Off				UMETA(DisplayName = "Off"),
	Auto            UMETA(DisplayName = "Auto"),
	VisemeBased     UMETA(DisplayName = "Viseme Based"),
	BS_MHA			UMETA(DisplayName = "MetaHuman Blendshapes"),
	BS_ARKit		UMETA(DisplayName = "ARKit Blendshapes"),
	BS_CC4_Extended	UMETA(DisplayName = "CC4 Extended Blendshapes")
};

UENUM(BlueprintType)
enum class EC_ContextUpdateMode : uint8
{
	Append  UMETA(DisplayName = "Append"),
	Replace UMETA(DisplayName = "Replace"),
	Reset   UMETA(DisplayName = "Reset")
};

UENUM(BlueprintType)
enum class EC_RunLLMOption : uint8
{
	Auto    UMETA(DisplayName = "Auto"),
	Always  UMETA(DisplayName = "Always"),
	Never   UMETA(DisplayName = "Never")
};

/**
 * Where the chatbot's current "object in attention" came from. Used to gate gaze-driven
 * overwrites: gaze may only take the slot when it is None or already Gaze. Explicit
 * (BP/C++) wins and locks the slot until cleared.
 */
UENUM(BlueprintType)
enum class EConvaiAttentionSource : uint8
{
	None     UMETA(DisplayName = "None"),
	Explicit UMETA(DisplayName = "Explicit (Blueprint/C++)"),
	Gaze     UMETA(DisplayName = "Gaze")
};

/**
 * Tells the LLM what one specific value of a tracked property means.
 *
 * Example: for a "DoorState" enum tracked property, you might add two of these:
 *   { Value = "Locked",   Description = "The door is bolted; the player cannot pass." }
 *   { Value = "Unlocked", Description = "The door swings freely." }
 *
 * The chatbot folds these descriptions into the object's overall description at
 * session start, so the AI knows what each state means without you having to
 * spell it out in a backstory.
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiTrackedPropertyStateValueDesc
{
	GENERATED_BODY()

	/** The literal value as it will be sent to the chatbot (e.g. "Locked", "true", "0", "Idle"). Must match what the property's value will read at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object")
	FString Value;

	/** What this value means in human terms — what the AI should understand when it sees this value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object", meta = (MultiLine = true))
	FString Description;
};

/**
 * One value on the owning Actor that the Convai Object Component will watch
 * and report to chatbots.
 *
 * Each tracked property is sent to chatbots once at session start (so the AI
 * knows the current state), and again every time the value changes during play
 * (so the AI stays in sync). The key the chatbot sees is "<ObjectName>.<PropertyPath>".
 *
 * Click the "Bind" button on PropertyPath and pick a value from the AnimGraph-style
 * picker — it walks the Actor's properties to any depth (drill into structs to pick
 * their members), and also lists pure functions that return a string (those get
 * called every poll tick and their return value is broadcast). Object references,
 * classes, delegates, etc. are filtered out (we can't safely poll across them).
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiTrackedProperty
{
	GENERATED_BODY()

	/**
	 * Dotted path to the value on this Actor (e.g. "bActive", "Stats.HP",
	 * "Stats.Vitals.Stamina", or "GetCurrentRoomName" for a string-returning
	 * function). Pick via the Bind button; typing here directly is supported
	 * but unsafe — the picker enforces the validity rules.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object")
	FName PropertyPath;

	/**
	 * What this property represents, in plain language for the AI.
	 *
	 * Example: "Whether the player has activated this switch."
	 * This text is folded into the object's overall description at session
	 * start, so the AI knows what the property means whenever it sees the
	 * value flow in.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object", meta = (MultiLine = true))
	FString Description;

	/**
	 * Optional. If your property has a small set of meaningful values (an
	 * enum, a bool, named states), describe each one here so the AI knows
	 * what they mean. Skip for free-form values like counters or floats.
	 *
	 * Tucked under Advanced because most properties don't need per-value
	 * descriptions — the property-level Description above is usually enough.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object", AdvancedDisplay)
	TArray<FConvaiTrackedPropertyStateValueDesc> StateValueDescriptions;

	/**
	 * What the chatbot should do when this value changes at runtime:
	 *   - Auto:   let the chatbot decide based on the value type / context.
	 *   - Always: every change makes the chatbot speak / react.
	 *   - Never:  the AI is silently informed of the new value, but won't say
	 *             anything about it on its own.
	 *
	 * The initial value at session start is always seeded with "Never", so
	 * connecting to a character with ten tracked objects doesn't make them
	 * announce every state. This setting only governs *changes* during play.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object")
	EC_RunLLMOption ShouldRespond = EC_RunLLMOption::Never;
};
