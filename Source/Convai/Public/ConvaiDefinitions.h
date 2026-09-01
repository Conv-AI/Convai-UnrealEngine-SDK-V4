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
 * What this entry's reference points at — the OBJECT itself. Used for gaze /
 * attention scoping, vision tagging, and as the movement fallback when no
 * Movement Points are authored.
 *
 *  - WholeActor:        the whole Actor is the object. Movement fallback:
 *                       AI Move To uses Ref directly and stops at the actor's
 *                       bounds.
 *  - SpecificComponent: a sub-component on the Actor is the object (set
 *                       Component Name, optionally a socket/bone). Gaze matches
 *                       only that component; movement fallback targets its
 *                       world location.
 *
 * When Movement Points are authored on the entry, THEY are the movement target —
 * this reference then only defines what the object is.
 */
UENUM(BlueprintType)
enum class EConvaiObjectReference : uint8
{
	/** The whole Actor is the object. Gaze matches any of its primitives; movement fallback stops at the actor's bounds. */
	WholeActor        UMETA(DisplayName = "Whole Actor"),
	/** A specific sub-component is the object (set Component Name / socket). Gaze matches only that component; movement fallback targets its location. */
	SpecificComponent UMETA(DisplayName = "Specific Component"),
};

/** How a Movement Point's transform is interpreted. */
UENUM(BlueprintType)
enum class EConvaiMovementPointAttachment : uint8
{
	/** Transform is relative to the object — the point moves with the goal actor, or with the specific component when the Object Reference is component-scoped. Default. */
	RelativeToObject UMETA(DisplayName = "Relative To Object"),
	/** Transform is absolute world space — the point stays put even if the object moves. Advanced. */
	KeepWorldPosition UMETA(DisplayName = "Keep World Position"),
};

/**
 * A designer-authored access point for movement: where a character should stand
 * when moving to this object (e.g. one point on each side of a door). Pure data —
 * edited via the viewport visualizer on Convai Object components or directly in
 * the Details panel. The resolver picks the reachable point with the shortest
 * walking path.
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiMovementPoint
{
	GENERATED_BODY()

	/** Where the character stands when this point is chosen. While the object
	 *  is selected, every point is a grab handle you can drag right in the
	 *  viewport; by default the point follows the object around (see
	 *  Attachment). Only the location drives movement today — rotation is
	 *  stored and editable, reserved for future facing control. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	FTransform Transform;

	/** Whether the point travels with the object or stays fixed in the world.
	 *  Relative To Object glues it on — move the door and its points move
	 *  too. Keep World Position nails it down — right for an elevator's floor
	 *  landings, which must stay at each floor while the platform rides
	 *  between them. Switching modes converts the stored position for you, so
	 *  the point never jumps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	EConvaiMovementPointAttachment Attachment = EConvaiMovementPointAttachment::RelativeToObject;

	/** Untick to take this point out of play without deleting it — characters
	 *  are never sent here while it is off. Handy for temporarily closing one
	 *  side of a door or comparing placements. If every point is disabled,
	 *  the object behaves as if it had none: characters walk up to the object
	 *  itself. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
	bool bEnabled = true;

	/** Off: this point is simply one of the places to stand when the AI goes
	 *  to the object itself — like standing ON an elevator platform. On: the
	 *  point becomes its OWN destination, named below — an elevator landing
	 *  named "Upper Landing" lets the AI be sent to "Elevator Upper Landing",
	 *  separate from the platform, with its own position, description in the
	 *  AI's surroundings, and walkability. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API",
		meta = (DisplayName = "Create Separate Destination"))
	bool bCreatesSeparateDestination = false;

	/** What this destination is called. The AI addresses it as
	 *  "<Object> <Destination Name>" — a door point named "Other Side"
	 *  becomes "Door Other Side". Keep it short and speakable: the AI says
	 *  this name out loud. Only used while Create Separate Destination is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API",
		meta = (DisplayName = "Destination Name", EditCondition = "bCreatesSeparateDestination"))
	FString Name;

	/** The point's world location. Relative points anchor to RefComponent when
	 *  provided (entry scoped to a specific component), else to RefActor's
	 *  transform. Null-safe: with neither, the transform reads as absolute. */
	FVector ResolveWorldLocation(const AActor* RefActor, const USceneComponent* RefComponent = nullptr) const;
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

	// ── Object reference & movement ──────────────────────────────────
	// Object Reference + Component Name define WHAT the object is (gaze /
	// attention / vision scope). Movement Points define WHERE a character
	// stands when moving to it; when none are authored, movement falls back
	// to the object reference itself.

	/** Which part of the actor counts as this object: the whole thing, or one
	 *  specific component inside it. It controls where a character looks when
	 *  it pays attention to the object, and where it walks when the Movement
	 *  Points list is empty. Example: make an elevator's button panel the
	 *  object, and the character looks at and walks to the panel instead of
	 *  the elevator's center. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API",
		meta = (DisplayName = "Object Is"))
		EConvaiObjectReference ObjectReference = EConvaiObjectReference::WholeActor;

	/** Type part of a component's name to pick which piece of the actor is
	 *  the object. The match ignores capitalization and looks anywhere in the
	 *  name — "muzzle" finds "MuzzleSocket_L". If several components match,
	 *  the first is used and a warning is logged, so type enough of the name
	 *  to single one out. Leave empty to use the actor's own position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API",
		meta = (EditCondition = "ObjectReference == EConvaiObjectReference::SpecificComponent"))
		FString ComponentName;

	/** Optional — aim at a named socket or bone on the matched component
	 *  instead of its origin. Sockets work on Static and Skeletal Mesh
	 *  components; bones only on Skeletal Mesh. If the name isn't found,
	 *  movement falls back to the component's origin and logs a warning, so a
	 *  typo never breaks movement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API",
		meta = (EditCondition = "ObjectReference == EConvaiObjectReference::SpecificComponent"))
		FName SocketOrBoneName;

	/** How close the character must get, in centimeters, to count as having
	 *  arrived. The default 150 cm is roughly arm's reach — and 150 is also
	 *  the floor for movement-point arrival, so smaller values only tighten
	 *  the engine's AI Move To stop distance, not Convai's arrival test.
	 *  Larger values suit vehicles and wide objects; arrival at the object
	 *  body (no points) tests about twice this radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API",
		meta = (ClampMin = "0.0"))
		float AcceptanceRadius = 150.f;

	/** Spots where a character stands when it walks to this object — for a
	 *  door, author one on each side. The character picks the reachable point
	 *  with the shortest walking path; if all points are blocked, the object
	 *  reports unreachable (see Use Object as Fallback). Leave the list empty
	 *  to have characters walk up to the object itself. Tick Create Separate
	 *  Destination on a point to make it its own destination the AI can be
	 *  sent to ("Door Other Side"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API")
		TArray<FConvaiMovementPoint> MovementPoints;

	/** If every Movement Point is unreachable, walk to the object itself
	 *  instead of giving up. Off by default: when you authored points and all
	 *  of them are blocked, that usually means the object SHOULD count as
	 *  unreachable — a door with both sides barricaded. Tick it when getting
	 *  near the object is still useful even with every authored spot blocked. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite,
		category = "Convai|Action API", AdvancedDisplay,
		meta = (DisplayName = "Use Object as Fallback"))
		bool bFallbackToObjectWhenPointsUnreachable = false;

	/** Output only — the component Convai resolved using Component Name.
	 *  Convai fills this in when the action arrives. Read it from Blueprint
	 *  when you need to do something component-specific (attach an effect,
	 *  change a property). Clears itself if the component is destroyed. */
	UPROPERTY(Transient, BlueprintReadOnly,
		category = "Convai|Action API")
		TWeakObjectPtr<USceneComponent> ResolvedComponent;

	/** True on auto-generated sub-object entries ("<Object> <PointName>") that
	 *  named Movement Points expand into. Never set by designers — marks the
	 *  entry so stale sub-objects retire precisely and never re-expand. */
	UPROPERTY(Transient, BlueprintReadOnly, category = "Convai|Action API", AdvancedDisplay)
		bool bIsMovementPointSubObject = false;

	/** Sub-object bookkeeping (set with bIsMovementPointSubObject): the base
	 *  object's name and the point name this entry represents. Retirement
	 *  matches on THESE, never on string-prefix guesses against Name. */
	UPROPERTY(Transient, BlueprintReadOnly, category = "Convai|Action API", AdvancedDisplay)
		FString MovementPointSubObjectBaseName;
	UPROPERTY(Transient, BlueprintReadOnly, category = "Convai|Action API", AdvancedDisplay)
		FString MovementPointSubObjectPointName;

	// ── Named movement points → sub-objects ──────────────────────────

	/** Canonical form of an AI-facing name (movement points, and object names
	 *  at registration): trimmed, inner whitespace collapsed. Identity
	 *  comparisons are case-insensitive on this; display keeps the designer's
	 *  casing. Deliberately NOT PascalCased — names are prompt/speech text the
	 *  model says out loud, unlike machine-ish state keys. */
	static FString NormalizeMovementPointName(const FString& Raw);

	/** The name that counts for sub-object identity: the normalized Name for
	 *  points with Create Separate Destination ticked, EMPTY for everything
	 *  else. Every identity decision routes through this, so a point that
	 *  isn't a separate destination is byte-identical to a bare one
	 *  everywhere that matters. */
	static FString EffectiveMovementPointName(const struct FConvaiMovementPoint& Point);

	/** The distinct movement-point names across a logical object's member
	 *  entries (merged sets pass every member), insertion-ordered, display
	 *  casing from the first occurrence. Returns EMPTY when naming is dormant —
	 *  no enabled point has Create Separate Destination ticked — the checkbox
	 *  is an explicit opt-in, so a ticked, named point ALWAYS expands. */
	static void CollectMovementPointSubNames(
		const TArray<const FConvaiObjectEntry*>& MemberEntries,
		TArray<FString>& OutDisplayNames);

	/** Drop every separate-destination point (keep the ones that belong to
	 *  the object itself) — the base object's share once sub-objects are
	 *  expanded. */
	void FilterMovementPointsToObjectItself();

	/** Keep only points whose normalized name matches DisplayName (disabled
	 *  matching points stay — the resolver already skips them). */
	void FilterMovementPointsToName(const FString& DisplayName);

	/** Describe a named destination across every member of a merged logical
	 *  object. Consistently world-fixed or relative points say so; mixed modes
	 *  deliberately use generic separate-place wording rather than lie about
	 *  whichever member happens to be the current navigation representative. */
	static FString MovementPointDestinationDescription(const FString& BaseName,
		const TArray<const FConvaiObjectEntry*>& MemberEntries,
		const FString& SubDisplayName);

	/** Clone this entry as the sub-object for one point name: named
	 *  "<Name> <Sub>", points filtered to that name, and described as a
	 *  separate fixed/attached place so the AI does not mistake it for the
	 *  object body. It never falls back to the body either. */
	FConvaiObjectEntry MakeMovementPointSubEntry(const FString& SubDisplayName) const;

	/** Returns true when Object Reference is Specific Component AND Component Name is set. */
	bool HasComponentFilters() const;

	/** Resolves Component Name against Ref's component tree and caches the
	 *  match in Resolved Component. The cache is revalidated on every call
	 *  (owner still Ref, name still matches the filter) and rescanned when it
	 *  fails. Returns null when Object Reference is not Specific Component,
	 *  when Ref or Component Name is missing, or when nothing matched. */
	USceneComponent* ResolveComponent();

	/** Resolves this entry into the full set of inputs an AI Move To node needs,
	 *  PLUS optional navmesh reachability data when Source Actor is provided.
	 *
	 *  Movement Points (when any are enabled) take over position resolution:
	 *   - With Source Actor: an arrival pre-pass first checks whether the actor
	 *     is already within acceptance of ANY enabled point (no nav query);
	 *     otherwise one nav path per point runs (nav data hoisted across the
	 *     loop) and the reachable point with the SHORTEST PATH wins (exact
	 *     ties keep the lower array index). bOut Move To Location is always
	 *     true; Out Movement Point Index / Out Goal Travel Distance report the
	 *     winner. All points unreachable → bOut Reachable false with the first
	 *     enabled point as the reported goal, unless
	 *     bFallbackToObjectWhenPointsUnreachable re-runs the object fallback.
	 *   - Without Source Actor: deterministically the first enabled point.
	 *
	 *  Object fallback (no enabled points — also the pre-Movement-Points
	 *  behavior), in Out Goal Location:
	 *   - Whole Actor:          Ref's origin (informational; AI Move To uses
	 *                           the actor pin — bOut Move To Location false).
	 *   - Specific Component + no
	 *     Component Name:       Ref's origin.
	 *   - Specific Component + Component
	 *     Name matched:         component / socket location.
	 *   - No Ref:               falls back to Optional Position Vector as-is
	 *                           (deprecated marker-only entries); bOut Success
	 *                           is false.
	 *  Also snapshots the resolved location into Optional Position Vector so
	 *  legacy reads stay consistent.
	 *
	 *  bOut Success — branch on this before consuming the other outputs:
	 *    - true: Ref was alive and resolution succeeded. Out Goal Actor, Out
	 *            Goal Component (Component Name match), Out Goal Location, Out
	 *            Acceptance Radius, and bOut Move To Location are all valid;
	 *            reachability outputs are valid when Source Actor was passed.
	 *    - false: Ref is null/destroyed. Out Goal Location holds the
	 *             snapshotted Optional Position Vector (deprecated marker-
	 *             only path), Out Goal Actor is null, Out Goal Component is
	 *             null. Reachability outputs are zero-initialised — the nav
	 *             query is skipped because there's no live target to path TO.
	 *  AI Move To consumers MUST gate on this; passing a destroyed Ref will
	 *  silently no-op (actor goal) or send the pawn to a stale point
	 *  (location goal).
	 *
	 *  Source-relative outputs (only populated when Source Actor is non-null):
	 *   - bOut Already There: true when Source Actor is effectively at the
	 *                       goal. With Movement Points: within
	 *                       max(Acceptance Radius, 150 uu) of any enabled
	 *                       point (the nearest wins). Object fallback:
	 *                       distance from Source Actor to the nearest point on
	 *                       the entry's bounding-box footprint within
	 *                       max(Acceptance Radius × 2, 150 uu), a conservative
	 *                       test wider than AIMoveTo's own trigger. Either way
	 *                       ANDed with a vertical check against Source Actor's
	 *                       own height. Cheap, no nav query — check it BEFORE
	 *                       issuing AI Move To to avoid no-op move commands.
	 *   - bOut Reachable:   true when a navmesh path from Source Actor lands
	 *                       within tolerance of the goal — max(Acceptance
	 *                       Radius, 150 uu) of a Movement Point, or (object
	 *                       fallback) within
	 *                       max(Acceptance Radius × 2, 150 uu) of the nearest
	 *                       footprint face. Plus Source Actor's own vertical
	 *                       span. False on no nav system, no nav data for Source Actor's
	 *                       agent props, path failure, or out-of-tolerance.
	 *   - Out Path End:     the path's final navpoint (may be a partial-path
	 *                       endpoint if the goal isn't fully reachable).
	 *   - Out Path Points:  the full nav path (world space) from start to
	 *                       Out Path End. Useful for debug visualisation.
	 *   - Out Goal Travel Distance: nav path length (uu) to the selected goal;
	 *                       0 when no path was computed / already there.
	 *   - Out Movement Point Index: index into Movement Points of the selected
	 *                       point; INDEX_NONE (-1) when the object fallback
	 *                       resolved the goal.
	 *  When Source Actor is null, the source-relative outputs are zero-
	 *  initialised.
	 *
	 *  Must be called on the game thread — it reads live actor/component
	 *  transforms and bounds, and may run a synchronous nav query. */
	void ResolveGoalLocation(
		AActor* SourceActor,
		AActor*& OutGoalActor,
		USceneComponent*& OutGoalComponent,
		FVector& OutGoalLocation,
		float& OutAcceptanceRadius,
		bool& bOutMoveToLocation,
		bool& bOutSuccess,
		bool& bOutAlreadyThere,
		bool& bOutReachable,
		FVector& OutPathEndPoint,
		TArray<FVector>& OutPathPoints,
		float& OutGoalTravelDistance,
		int32& OutMovementPointIndex);

	/** Refreshes the Optional Position Vector + Resolved Component snapshots
	 *  without computing nav reachability. Use this from non-movement code
	 *  paths that just need the cached values to be current (e.g. param
	 *  resolution at receive time). Skips when Ref is destroyed. */
	void RefreshSnapshot();

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

	/** Whether this action is advertised to the AI when a fresh connection is created.
	 *  Turning it off preserves the action definition so it can be re-enabled later. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API",
		meta = (DisplayName = "Enabled",
			ToolTip = "Advertise this action when a fresh connection is created. Turning it off keeps its setup but prevents the AI from choosing it. Reconnect required."))
	bool bEnabled = true;

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

		// The server sends Plutchik-wheel names (Serenity/Joy/Ecstasy, ...); the TTS-style
		// names (Calm/Bored/Neutral) are what older servers sent.
		EBasicEmotions Emotion;
		EEmotionIntensity Intensity;
		GetEmotionDetails(EmotionString, Intensity, Emotion);
		if (Emotion == EBasicEmotions::None)
		{
			GetTTSEmotion(EmotionString, Emotion);
		}

		ResetEmotionScores();
		if (Emotion != EBasicEmotions::None)
		{
			EmotionsScore.Add(Emotion, Scale);
		}
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
			constexpr const char* ForceUserStoppedSpeaking = "force-user-stopped-speaking";
			constexpr const char* ClientReady         = "client-ready";
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

// Knowledge Bank
USTRUCT(BlueprintType)
struct FConvaiKnowledgeBankDocument
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Knowledge Bank")
	FString DocumentID;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Knowledge Bank")
	FString FileName;

	/** Processing finished server-side, so the document can be connected to a character. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Knowledge Bank")
	bool bIsAvailable;

	/** Connected to the character the listing was requested for. Distinct from bIsAvailable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Knowledge Bank")
	bool bConnected;

	/** Upload time as the server reported it. Kept verbatim; it carries microseconds FDateTime::Parse drops. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Knowledge Bank")
	FString Timestamp;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Knowledge Bank")
	int64 FileSizeBytes;

	FConvaiKnowledgeBankDocument()
		: bIsAvailable(false)
		, bConnected(false)
		, FileSizeBytes(0)
	{
	}
};

/**
 * Why a session ended. Only Unexpected is ever a candidate for reconnection —
 * the other two are outcomes somebody asked for. See CONTEXT.md.
 */
UENUM(BlueprintType)
enum class EC_DisconnectReason : uint8
{
	/** Nothing has disconnected yet, or the cause was network/server fault. */
	Unexpected UMETA(DisplayName = "Unexpected"),
	/** The game or the player asked for it. */
	Explicit   UMETA(DisplayName = "Explicit"),
	/** AFK Time ran out and the plugin let the server close the session. */
	Idle       UMETA(DisplayName = "Idle"),
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
	Auto    UMETA(DisplayName = "Auto", ToolTip = "Let the server decide whether this update deserves an LLM response."),
	Always  UMETA(DisplayName = "Always", ToolTip = "Request an LLM response to this update (the server generates one; whether it is spoken depends on the response)."),
	Never   UMETA(DisplayName = "Never", ToolTip = "Inform the AI silently - the update lands in its context without requesting any response.")
};

/**
 * WHEN a dynamic-context update reaches the character.
 *
 *   - Send Normally: batched into the next scheduled send (the default — same
 *     behavior as before this option existed).
 *   - Wait Until Conversation Is Idle: held back until the conversation has
 *     stayed quiet for the chatbot's "Quiet Time Before Delivery" seconds, so
 *     the update can't make the character interrupt itself or talk over the
 *     user. There is deliberately no time limit on the wait — a waiting update
 *     never interrupts an ongoing conversation.
 *
 * Waiting only applies when ShouldRespond is Auto/Always — a silent (Never)
 * update has nothing to interrupt with, so it is always sent normally.
 */
UENUM(BlueprintType)
enum class EConvaiContextDelivery : uint8
{
	SendNormally                UMETA(DisplayName = "Send Normally", ToolTip = "Send through the normal batched delivery without waiting for a pause in the conversation. Default - the behavior before this option existed."),
	WaitUntilConversationIsIdle UMETA(DisplayName = "Wait Until Conversation Is Idle", ToolTip = "Hold the update until the conversation has stayed quiet for the chatbot's Quiet Time Before Delivery, so the character can't interrupt itself or talk over anyone. No time limit - it waits as long as the conversation lasts. With Flush Immediately, it is sent at the first quiet moment instead. Only applies when Should Respond is Auto/Always.")
};

/** How much transform motion must be present before an object counts as moving. */
UENUM(BlueprintType)
enum class EConvaiMovementSensitivity : uint8
{
	VeryLow UMETA(DisplayName = "Very Low", ToolTip = "Recognizes only clear movement. Best for noisy physics objects."),
	Low UMETA(DisplayName = "Low", ToolTip = "Ignores small drift and collision bumps."),
	Medium UMETA(DisplayName = "Medium", ToolTip = "Balanced movement detection for most objects."),
	High UMETA(DisplayName = "High", ToolTip = "Detects slow or subtle movement, but may notice physics noise."),
	VeryHigh UMETA(DisplayName = "Very High", ToolTip = "Detects extremely small movement. Best for precisely controlled objects.")
};

/**
 * Movement awareness for a Convai Object.
 *
 * The master switch controls transform sampling and the moving/stopped verdict.
 * Spatial facts can use that verdict directly. The optional durable
 * `<Object>.Movement` state can also nudge the character or be armed through
 * the one-shot Watch Property action. Compact values include vertical and
 * observer-relative horizontal direction without redundantly repeating the
 * key's "Movement" wording; speed remains in the spatial fact. Objects with the
 * durable state enabled also say "stopped" there; ordinary stationary props
 * remain terse.
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiObjectMovementSettings
{
	GENERATED_BODY()

	/** Detect this object's translation and rotation for movement-aware context. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ConvaiObjectMovementSettings",
		meta = (DisplayName = "Enable Movement Awareness",
			ToolTip = "Detect this object's translation and rotation. Turning this off removes movement wording and the Movement state, but leaves ordinary position and reachability awareness unchanged."))
	bool bEnableMovementAwareness = true;

	/** How much coherent transform motion counts as moving. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ConvaiObjectMovementSettings",
		meta = (DisplayName = "Sensitivity",
			EditCondition = "bEnableMovementAwareness",
			ToolTip = "How much coherent translation or rotation counts as movement. Higher detects subtler motion; lower filters more physics drift and collision jitter."))
	EConvaiMovementSensitivity MovementSensitivity = EConvaiMovementSensitivity::Medium;

	/** Publish this object's compact movement state: Stopped, Moving, Upward,
	 *  Downward, Toward You, Away, Left, Right, or Rotating. Speed remains in the
	 *  spatial fact; this switch also makes a confirmed stop explicit there. The
	 *  sampled transform is the Object Entry's whole actor or selected component. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ConvaiObjectMovementSettings",
		meta = (DisplayName = "Add Movement State",
			EditCondition = "bEnableMovementAwareness",
			ToolTip = "Add <Object>.Movement to context and make it available to Watch Property. This also says when the object is stopped. Moving direction in surroundings works without this option."))
	bool bExposeMovementState = false;

	/** How the chatbot should react when the object begins moving. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ConvaiObjectMovementSettings",
		meta = (EditCondition = "bEnableMovementAwareness && bExposeMovementState",
			DisplayName = "When Movement Starts",
			ToolTip = "Whether a confirmed start should request a response. The Movement state is updated either way."))
	EC_RunLLMOption StartedMovingResponse = EC_RunLLMOption::Never;

	/** How the chatbot should react when the object comes to a confirmed stop. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ConvaiObjectMovementSettings",
		meta = (EditCondition = "bEnableMovementAwareness && bExposeMovementState",
			DisplayName = "When Movement Stops",
			ToolTip = "Whether a confirmed stop should request a response. The Movement state is updated either way."))
	EC_RunLLMOption StoppedMovingResponse = EC_RunLLMOption::Never;

	/** When a responsive transition reaches the chatbot. This also governs a
	 *  one-shot Watch Property wake-up on an otherwise-silent state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ConvaiObjectMovementSettings",
		meta = (EditCondition = "bEnableMovementAwareness && bExposeMovementState && (StartedMovingResponse != EC_RunLLMOption::Never || StoppedMovingResponse != EC_RunLLMOption::Never)",
			ToolTip = "When a movement transition that can request a response reaches the character. While this row is enabled, a one-shot Watch Property wake-up uses this delivery mode too."))
	EConvaiContextDelivery Delivery = EConvaiContextDelivery::SendNormally;

	/** Bypass normal context debounce for a confirmed transition. With Wait Until
	 *  Conversation Is Idle, it still waits for silence but sends at the first
	 *  idle instant. Useful for short timing windows; leave off for ambient motion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ConvaiObjectMovementSettings",
		meta = (EditCondition = "bEnableMovementAwareness && bExposeMovementState && (StartedMovingResponse != EC_RunLLMOption::Never || StoppedMovingResponse != EC_RunLLMOption::Never)",
			DisplayName = "Flush Immediately",
			ToolTip = "Bypass normal context batching for a responsive movement transition. If delivery waits for idle, send at the first quiet moment."))
	bool bFlushImmediately = false;

	/** True when the optional durable state is active. */
	bool IsMovementStateEnabled() const
	{
		return bEnableMovementAwareness && bExposeMovementState;
	}

	/** True when either confirmed movement edge can request a response. */
	bool HasResponsiveTransition() const
	{
		return StartedMovingResponse != EC_RunLLMOption::Never
			|| StoppedMovingResponse != EC_RunLLMOption::Never;
	}

	/** Restore the only meaningful delivery settings when both edges are silent. */
	void NormalizeResponseSettings()
	{
		if (!HasResponsiveTransition())
		{
			Delivery = EConvaiContextDelivery::SendNormally;
			bFlushImmediately = false;
		}
	}

	/** Runtime-safe delivery even if Blueprint directly mutates this struct. */
	EConvaiContextDelivery GetEffectiveDelivery() const
	{
		return IsMovementStateEnabled() && HasResponsiveTransition()
			? Delivery : EConvaiContextDelivery::SendNormally;
	}

	/** Runtime-safe flushing even if Blueprint directly mutates this struct. */
	bool GetEffectiveFlushImmediately() const
	{
		return IsMovementStateEnabled() && HasResponsiveTransition()
			&& bFlushImmediately;
	}
};

/**
 * How duplicate Convai Object names are disambiguated at registration when
 * "Merge Same-Named Objects" is OFF. The first object keeps its bare name; the
 * 2nd, 3rd, ... collisions get a suffix in the chosen style. (When merge is ON,
 * no suffix is applied — same-named objects are intentionally treated as one.)
 */
UENUM(BlueprintType)
enum class EConvaiObjectNameSuffixStyle : uint8
{
	/** "Crate", "Crate 2", "Crate 3", ... (reads naturally for TTS). */
	Numeric      UMETA(DisplayName = "Numeric (2, 3, 4)"),
	/** "Crate", "Crate A", "Crate B", ... (2nd object -> A, 3rd -> B). */
	Alphabetical UMETA(DisplayName = "Alphabetical (A, B, C)")
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
 * (so the AI stays in sync). The key the chatbot sees is
 * "<ObjectName>.<Alias-or-PropertyPath>", whitespace-stripped (Pascal-cased).
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
	 * function). Paths may hop through actor / component / instanced-object
	 * references into sub-objects ("Turret.Health"); a null hop just pauses
	 * tracking until it becomes valid. Pick via the Bind button; typing here
	 * directly is supported but unsafe — the picker enforces the validity rules.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object")
	FName PropertyPath;

	/**
	 * Optional short name for this property. When set, chatbots see the state
	 * key as "<ObjectName>.<Alias>" instead of the full dotted path. Keys are
	 * whitespace-stripped (Pascal-cased) before reaching the LLM ("door state"
	 * → "DoorState"). Must be unique among this object's tracked properties.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object")
	FString Alias;

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

	/**
	 * WHEN a change to this property reaches the chatbots:
	 *   - Send Normally: batched into the next scheduled send.
	 *   - Wait Until Conversation Is Idle: held until the conversation has
	 *     stayed quiet for the chatbot's "Quiet Time Before Delivery" seconds,
	 *     so the change can't make a character interrupt itself mid-sentence.
	 *
	 * Only applies when Should Respond is Auto or Always. Never is a silent
	 * context update, so its delivery is always Send Normally.
	 */
	// The tracked-property Details customization owns this row's enabled state.
	// A nested EditCondition cannot resolve its sibling through that customization.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object")
	EConvaiContextDelivery Delivery = EConvaiContextDelivery::SendNormally;

	/**
	 * Send a genuine change immediately instead of waiting for the chatbot's
	 * normal context debounce window. With Wait Until Conversation Is Idle, the
	 * update still waits for silence but is released at the first idle instant.
	 *
	 * Only applies when Should Respond is Auto or Always. Kept visible rather
	 * than under Advanced because nested-struct Advanced rows are not consistently
	 * surfaced by every supported Unreal Details panel.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Object",
		meta = (DisplayName = "Flush Immediately"))
	bool bFlushImmediately = false;

	/** Restores the only meaningful delivery settings for a silent property. */
	void NormalizeResponseSettings()
	{
		if (ShouldRespond == EC_RunLLMOption::Never)
		{
			Delivery = EConvaiContextDelivery::SendNormally;
			bFlushImmediately = false;
		}
	}

	/** Runtime-safe delivery even if Blueprint code directly mutates the struct. */
	EConvaiContextDelivery GetEffectiveDelivery() const
	{
		return ShouldRespond == EC_RunLLMOption::Never
			? EConvaiContextDelivery::SendNormally
			: Delivery;
	}

	/** Runtime-safe flush setting even if Blueprint code directly mutates the struct. */
	bool GetEffectiveFlushImmediately() const
	{
		return ShouldRespond != EC_RunLLMOption::Never && bFlushImmediately;
	}

#if WITH_EDITORONLY_DATA
	/**
	 * Internal, editor-only. One rename-stable GUID per PropertyPath segment
	 * (Blueprint variable / SCS component GUID; invalid for native segments).
	 * Recorded and consumed by the in-editor heal pass so paths re-latch after
	 * Blueprint variable renames — see UConvaiObjectComponent::HealTrackedPropertyPaths.
	 */
	UPROPERTY()
	TArray<FGuid> PropertyPathGuids;
#endif
};


// ──────────────────────────────────────────────────────────────────────────
// FConvaiAvatarInfo + nested types — relocated from
// ConvaiHelperLibrary/AssetManagerDataTypes.h so the Convai module's
// `character/list` proxy can return them directly. Definitions are verbatim
// (no field/default changes) so existing UScriptStruct layout matches.
// AssetManagerDataTypes.h re-includes this header for back-compat.
// ──────────────────────────────────────────────────────────────────────────

USTRUCT(BlueprintType)
struct FConvaiMetaHuman
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString AvatarId;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString AvatarImage;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString AvatarImageSquare;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString ExperienceId;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString BackgroundImage;
};

USTRUCT(BlueprintType)
struct FConvaiModelDetails
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString ModelType;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString ModelLink;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString ModelPlaceholder;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FConvaiMetaHuman MetaHuman;
};

USTRUCT(BlueprintType)
struct FConvaiPersonalityTraits
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    int32 Openness;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    int32 Sensitivity;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    int32 Extraversion;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    int32 Agreeableness;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    int32 Meticulousness;
};

USTRUCT(BlueprintType)
struct FConvaiCharacterTraits
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    TArray<FString> CatchPhrases;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString SpeakingStyle;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FConvaiPersonalityTraits PersonalityTraits;
};

USTRUCT(BlueprintType)
struct FConvaiGuardrailMeta
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    int32 LimitResponseLevel;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    TArray<FString> BlockedWords;
};

USTRUCT(BlueprintType)
struct FConvaiMemorySettings
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    bool bEnabled;
};

USTRUCT(BlueprintType)
struct FConvaiSpeakingStyle
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString Description;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString SampleDialogues;
};

USTRUCT(BlueprintType)
struct FConvaiEmbodimentData
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString LooksDescription;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString ClothesDescription;
};

USTRUCT(BlueprintType)
struct FConvaiAvatarInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString CharacterName;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString UserId;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString CharacterId;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString Listing;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    TArray<FString> LanguageCodes;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString VoiceType;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    TArray<FString> CharacterActions;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    TArray<FString> CharacterEmotions;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FConvaiModelDetails ModelDetails;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString LanguageCode;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FConvaiGuardrailMeta GuardrailMeta;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FConvaiCharacterTraits CharacterTraits;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString Timestamp;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    int32 Verbosity;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString OrganizationId;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    bool bIsNarrativeDriven;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString StartNarrativeSectionId;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    bool bModerationEnabled;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    TArray<FString> Pronunciations;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    TArray<FString> BoostedWords;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    TArray<FString> AllowedModerationFilters;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FConvaiMemorySettings MemorySettings;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString UncensoredAccessConsent;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString NsfwModelSize;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    float Temperature;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString Description;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FConvaiSpeakingStyle SpeakingStyle;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString ModelType;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FConvaiEmbodimentData EmbodimentData;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    FString Backstory;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
    bool bEditCharacterAccess;
};
