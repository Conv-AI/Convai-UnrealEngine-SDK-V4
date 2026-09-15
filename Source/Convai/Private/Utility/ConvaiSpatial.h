// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiDefinitions.h"

class AActor;
class UWorld;

/**
 * Stateless helpers for the Convai spatial-awareness system: turning relative
 * positions into the plain-language phrases the AI reads ("close by, in front
 * of you"), classifying distance into coarse bands, finding an actor's eye
 * line, line-of-sight tracing, and facing.
 *
 * Everything here is a pure function — no state, no allocation beyond the
 * returned strings — so the context subsystem can call it freely from its poll.
 * Phrases come in two voices that share one geometry core:
 *   - second person  ("in front of you")     — how a subject reads to the bot
 *                                               observing it (category 1).
 *   - relational     ("in front of <Ref>")   — how one entity reads relative to
 *                                               another anchor entity (category 2).
 *
 * All directions are computed in the OBSERVER's local frame: +X forward,
 * +Y right, +Z up.
 */
namespace ConvaiSpatial
{
	/** An observer's first spatial snapshot establishes baseline knowledge and
	 *  must not request an unsolicited opening response. */
	inline EC_RunLLMOption ResolveSnapshotResponse(
		EC_RunLLMOption ConfiguredResponse, bool bInitialSnapshotDelivered)
	{
		return bInitialSnapshotDelivered
			? ConfiguredResponse : EC_RunLLMOption::Never;
	}

	/** Arrival is a successful destination even when pathfinding cannot produce
	 *  a path from the point the observer already occupies. */
	inline bool HasUsableDestination(bool bReachable, bool bAlreadyThere)
	{
		return bAlreadyThere || bReachable;
	}

	/** A person is "already with" the observer only when they are physically
	 *  close and the navmesh agrees that the short gap is directly walkable.
	 *  The path cap prevents a person across a wall or on another floor from
	 *  suppressing the correct blocked/route verdict. */
	inline bool IsPersonAlreadyThere(bool bReachable, float DirectDistance2DUU,
		float VerticalDistanceUU, double PathLengthUU)
	{
		return bReachable
			&& DirectDistance2DUU <= 150.0f
			&& VerticalDistanceUU <= 200.0f
			&& PathLengthUU >= 0.0
			&& PathLengthUU <= 200.0;
	}

	/** Shared motion thresholds used by the central object poll. Unreal units are
	 *  centimetres, so the linear values are cm/s. Start is deliberately higher
	 *  than stop; that hysteresis keeps easing tails and physics jitter quiet. */
	inline constexpr float MotionStartLinearSpeed = 5.0f;
	inline constexpr float MotionStopLinearSpeed = 2.0f;
	inline constexpr float MotionStartAngularSpeed = 5.0f;
	inline constexpr float MotionStopAngularSpeed = 2.0f;
	inline constexpr double MotionStartConfirmationSeconds = 0.25;
	inline constexpr double MotionStopConfirmationSeconds = 0.50;
	inline constexpr double MotionTrendWindowSeconds = MotionStopConfirmationSeconds;
	inline constexpr float MotionLinearExcursionFloor =
		MotionStartLinearSpeed * static_cast<float>(MotionTrendWindowSeconds);
	inline constexpr float MotionAngularExcursionFloor =
		MotionStartAngularSpeed * static_cast<float>(MotionTrendWindowSeconds);
	// A prospective direction must make enough qualified progress before it can
	// replace an established trip direction.
	inline constexpr float MotionDirectionProgressMultiplier = 5.0f;
	// Direction-churning motion may settle only while every sampled member stays
	// inside this larger local envelope. Low enters at 60 cm and releases at
	// 70 cm, so a boundary sample cannot flap Stopped/Moving.
	inline constexpr float MotionSettlingEnvelopeMultiplier = 12.0f;
	inline constexpr float MotionSettlingReleaseMultiplier = 14.0f;
	// Direction wording must remain available at the most sensitive profile.
	// Detection still gates whether such a small velocity reaches the semantic
	// cache, so this lower linguistic floor does not make Medium noisier.
	inline constexpr float MotionMinimumLinearSpeed = 0.5f;

	/** Per-object magnitude thresholds. The trend window stays fixed so changing
	 *  sensitivity changes what counts as motion, not how long detection takes. */
	struct FMotionThresholds
	{
		float StartLinearSpeed = MotionStartLinearSpeed;
		float StopLinearSpeed = MotionStopLinearSpeed;
		float StartAngularSpeed = MotionStartAngularSpeed;
		float StopAngularSpeed = MotionStopAngularSpeed;
		float LinearExcursionFloor = MotionLinearExcursionFloor;
		float AngularExcursionFloor = MotionAngularExcursionFloor;
	};

	/** Convert the designer-facing sensitivity into detector thresholds. */
	FMotionThresholds GetMotionThresholds(EConvaiMovementSensitivity Sensitivity);

	/** A confirmed edge from the coarse Moving/Stopped detector. */
	enum class EMotionTransition : uint8 { None, Started, Stopped };

	/** Net transform velocity over a sample window. Values are useful during
	 *  warm-up too; bReady means the full confirmation window has elapsed. Net
	 *  displacement cancels small collision bounce while preserving coherent
	 *  slow travel. */
	struct FMotionTrend
	{
		FVector LinearVelocity = FVector::ZeroVector;
		float AngularSpeed = 0.0f;
		float MaxLinearExcursion = 0.0f;
		float MaxAngularExcursion = 0.0f;
		bool bReady = false;
	};

	/** Coarse distance/reachability bucket that drives the "close by / far away /
	 *  no walking path" prefix. Unreachable wins over any distance — if there's
	 *  no nav path the exact distance is moot. */
	enum class EProximityBand : uint8 { Nearby, Moderate, Far, Unreachable };

	enum class EForwardDir  : uint8 { None, Front,  Behind };
	enum class ELateralDir  : uint8 { None, Right,  Left };
	enum class EVerticalDir : uint8 { None, Above,  Below };

	/** Which axes a target dominantly lies along, in the observer's local frame.
	 *  Any axis may be None (not clearly dominant). bColocated means the target
	 *  is essentially at the observer's position (no meaningful direction). */
	struct FDirection
	{
		EForwardDir  Forward  = EForwardDir::None;
		ELateralDir  Lateral  = ELateralDir::None;
		EVerticalDir Vertical = EVerticalDir::None;
		bool         bColocated = false;

		bool IsEmpty() const
		{
			return Forward == EForwardDir::None
				&& Lateral == ELateralDir::None
				&& Vertical == EVerticalDir::None;
		}
	};

	/**
	 * Resolve which axes dominate for a target at LocalDelta (the vector from the
	 * observer to the target, expressed in the observer's local frame).
	 *
	 * A horizontal axis is reported only when its normalised component clears
	 * 0.35, so a near-purely-forward target reads "in front" rather than
	 * "in front and slightly to the right". The vertical axis is decided
	 * differently: it ignores the normalised-dominance test and instead compares
	 * the raw Z against the observer's own silhouette — HeadAboveUU / FootBelowUU
	 * are the top/bottom of the observer relative to its pivot (see
	 * ComputeEyeLine) — so a lamp 4 m forward and 4 m up still reads "above" even
	 * though its normalised Z is small. A 30 uu grace band around head/foot keeps
	 * at-eye-level targets from flickering into "above"/"below".
	 */
	FDirection ComputeDirection(const FVector& LocalDelta, float HeadAboveUU, float FootBelowUU);

	/** Second-person phrase, e.g. "in front of you and to your right",
	 *  "right next to you" (colocated), or "nearby" (no dominant axis). */
	FString DirectionToYou(const FDirection& Dir);

	/** Relational phrase anchored on RefName, e.g. "in front of and to the right
	 *  of Crate", "above Jason", "right next to Crate", or "near Crate". RefName
	 *  is inserted verbatim — the caller decides articles ("the crate" vs
	 *  "Jason"). */
	FString DirectionToRef(const FDirection& Dir, const FString& RefName);

	/** Frame-free direction words for a clause whose frame the caller names
	 *  explicitly ("From Eshmawy's position, the crate is ... ahead and to the left"):
	 *  "ahead", "behind", "to the left/right", "above", "below". No pronouns and
	 *  no reference name, so nothing in it can be misread as another frame.
	 *  Colocated / no dominant axis reads "right there". */
	FString DirectionPlain(const FDirection& Dir);

	/** Bucket a straight-line distance (uu) into a band. Below NearbyUU is
	 *  "close by"; below ModerateUU is "some distance away"; beyond is "far away".
	 *  bReachable=false overrides everything with Unreachable (reserved for a
	 *  future nav-aware path; callers without nav data pass true). */
	EProximityBand ClassifyDistance(float DistanceUU, bool bReachable, float NearbyUU, float ModerateUU);

	/** Bare band phrase with no reachability verdict — "close by", "some distance
	 *  away", "far away". For frames (e.g. a player's) whose nav reachability is
	 *  unknown; Unreachable also reads "far away". */
	FString BandWord(EProximityBand Band);

	/** Compose the band prefix with direction and an explicit nav verdict, e.g.
	 *  Nearby + "in front of you" ->
	 *  "close by, in front of you, reachable by walking";
	 *  Unreachable + "to your left" ->
	 *  "to your left, with no walking path there". */
	FString BandPhrase(EProximityBand Band, const FString& Direction);

	/** Compose a support/adjacency phrase without repeating the subject name.
	 *  Relation is the positional predicate (for example, "underneath you"),
	 *  Motion is empty / "stopped" / an active MotionPhrase, and reachability is
	 *  omitted entirely once the destination has already been reached. */
	FString ComposeSupportClause(const FString& Relation, const FString& Motion,
		bool bIncludeReachability, bool bReachable);

	/** A reached destination needs no walking verdict. Objects read "at" while
	 *  people read "with", using the already article-qualified display name. */
	FString AlreadyThereSentence(const FString& DisplayName, bool bIsPerson);

	/** Compose an arrival fact while naming the destination exactly once. An
	 *  optional current spatial clause is joined with "which" / "who", avoiding
	 *  both repeated subjects and an ambiguous "You are already there." */
	FString AlreadyThereFact(const FString& DisplayName, bool bIsPerson,
		const FString& CurrentClause);

	/**
	 * Advance a two-state motion detector with confirmation windows and
	 * hysteresis. bAboveStart is normally "translation OR rotation clears its
	 * start threshold"; bBelowStop is "translation AND rotation are below their
	 * stop thresholds". Candidate states never escape into context.
	 *
	 * CandidateSince is caller-owned persistent storage; initialise it to -1.
	 */
	EMotionTransition UpdateMotionState(bool bAboveStart, bool bBelowStop,
		double NowSeconds, bool& bInOutMoving, double& CandidateSince,
		double StartConfirmationSeconds = MotionStartConfirmationSeconds,
		double StopConfirmationSeconds = MotionStopConfirmationSeconds);

	/** Measure net translation and rotation between two poses. The velocities and
	 *  excursions are populated for every positive span; bReady becomes true only
	 *  after MinWindowSeconds, allowing that window itself to serve as stop
	 *  confirmation instead of adding a second delay. */
	FMotionTrend ComputeMotionTrend(const FVector& StartLocation,
		const FQuat& StartRotation, double StartTimeSeconds,
		const FVector& EndLocation, const FQuat& EndRotation,
		double EndTimeSeconds, float MaxLinearExcursion,
		float MaxAngularExcursion,
		double MinWindowSeconds = MotionTrendWindowSeconds);

	/** Trim a caller-owned sample window and compute its net trend. SampleArrayType
	 *  only needs Num/Last/RemoveAt and elements with Location, Rotation, and
	 *  TimeSeconds fields; this keeps the subsystem's cache type private while the
	 *  exact rolling algorithm remains directly testable. */
	template <typename SampleArrayType>
	FMotionTrend ComputeRollingMotionTrend(SampleArrayType& Samples,
		double WindowSeconds = MotionTrendWindowSeconds)
	{
		if (Samples.Num() == 0)
		{
			return FMotionTrend();
		}

		const double Cutoff = Samples.Last().TimeSeconds - WindowSeconds;
		int32 SamplesToRemove = 0;
		while (SamplesToRemove + 1 < Samples.Num()
			&& Samples[SamplesToRemove + 1].TimeSeconds <= Cutoff)
		{
			++SamplesToRemove;
		}
		if (SamplesToRemove > 0)
		{
			Samples.RemoveAt(0, SamplesToRemove);
		}

		const auto& Start = Samples[0];
		float MaxLinearExcursion = 0.0f;
		float MaxAngularExcursion = 0.0f;
		for (const auto& Sample : Samples)
		{
			MaxLinearExcursion = FMath::Max(MaxLinearExcursion,
				FVector::Distance(Start.Location, Sample.Location));
			const double ExcursionDot = FMath::Clamp(
				FMath::Abs(static_cast<double>(Start.Rotation | Sample.Rotation)),
				0.0, 1.0);
			MaxAngularExcursion = FMath::Max(MaxAngularExcursion,
				static_cast<float>(FMath::RadiansToDegrees(
					2.0 * FMath::Acos(ExcursionDot))));
		}

		const auto& End = Samples.Last();
		return ComputeMotionTrend(
			Start.Location, Start.Rotation, Start.TimeSeconds,
			End.Location, End.Rotation, End.TimeSeconds,
			MaxLinearExcursion, MaxAngularExcursion, WindowSeconds);
	}

	/** A ready trend is quiet only when both its net velocity and maximum
	 *  excursion stay beneath the perception floor. The excursion guard keeps a
	 *  real oscillating object moving even if it returns to its starting pose. */
	bool IsMotionTrendQuiet(const FMotionTrend& Trend,
		const FMotionThresholds& Thresholds = GetMotionThresholds(
			EConvaiMovementSensitivity::Medium));

	/** Raw speed keeps starts responsive, but must agree with coherent net progress
	 *  while the trend warms up. Once ready, a quiet trend vetoes collision spikes
	 *  so a settled object cannot flap straight back to Moving. */
	bool IsMotionStartEvidence(bool bRawAboveStart, const FMotionTrend& Trend,
		const FMotionThresholds& Thresholds = GetMotionThresholds(
			EConvaiMovementSensitivity::Medium));

	/** A detector edge is gameplay only when it is not structural recovery.
	 *  Reattaching a moving cache to an already-stationary target must not
	 *  fabricate a watched/responding stop. */
	bool IsGameplayMotionTransition(EMotionTransition Transition,
		bool bReinitializedThisPoll, bool bAwaitingPostReinitMotion);

	/** True when active motion changes mode or world-space direction enough to
	 *  refresh its semantic label immediately instead of waiting for cadence. */
	bool HasMeaningfulMotionDirectionChange(const FVector& PreviousVelocity,
		bool bPreviousRotationOnly, const FVector& NewVelocity,
		bool bNewRotationOnly);

	/** True when both the mature rolling trend and latest raw sample make
	 *  start-level progress in the same prospective direction. Stop detection
	 *  deliberately uses the lower stop threshold; the stricter relabel check
	 *  prevents a stale endpoint-rebound trend from replacing a completed trip. */
	bool HasClearMotionDirectionEvidence(
		const FVector& RawLinearVelocity,
		float RawAngularSpeed,
		const FVector& FilteredLinearVelocity,
		float FilteredAngularSpeed,
		bool bRotationOnly,
		const FMotionThresholds& Thresholds = GetMotionThresholds(
			EConvaiMovementSensitivity::Medium));

	/** Associates per-member clear linear evidence with the merged semantic
	 *  candidate. Evidence from another member moving a different way must not
	 *  authorize a weak reversal. A directionless merged candidate is clear only
	 *  when every active linear contributor individually cleared its threshold. */
	bool HasCoherentClearLinearDirectionEvidence(
		const FVector& SemanticCandidate,
		const FVector& ClearVelocitySum,
		int32 ClearVelocityCount,
		int32 ActiveVelocityCount);

	/** A prospective direction/mode is confirmed only after continuous coherent
	 *  evidence lasts long enough and accumulates sensitivity-scaled progress. */
	bool IsMotionDirectionCandidateConfirmed(
		bool bCurrentlyCoherent,
		double CoherentSeconds,
		float IntegratedProgress,
		float RequiredProgress,
		double ConfirmationSeconds = MotionStartConfirmationSeconds);

	/** Progress speed derived only from contributors whose latest raw sample
	 *  agrees with their rolling direction. A directionless merged candidate uses
	 *  scalar member speed; a directed one uses projection onto its candidate. */
	float MotionDirectionCandidateProgressSpeed(
		const FVector& CandidateVelocity,
		bool bRotationOnly,
		const FVector& EvidenceVelocitySum,
		float EvidenceLinearSpeedSum,
		int32 EvidenceLinearVelocityCount,
		float EvidenceAngularSpeed);

	/** Whether a member transform remains inside a sensitivity-scaled local
	 *  motion envelope. Rotation uses the shortest quaternion arc in degrees. */
	bool IsInsideMotionSettlingEnvelope(
		const FVector& AnchorLocation,
		const FQuat& AnchorRotation,
		const FVector& CurrentLocation,
		const FQuat& CurrentRotation,
		const FMotionThresholds& Thresholds,
		float EnvelopeMultiplier = MotionSettlingEnvelopeMultiplier);

	/** A bounded stop needs either real evidence loss/quiet or repeated direction
	 *  churn. One ordinary L-turn is not settling. */
	bool HasMotionSettlingEvidence(
		bool bBecameIncoherent,
		int32 DirectionSwitchCount,
		bool bCurrentDirectionCoherent,
		bool bCurrentDirectionStable);

	/** Once a prospective direction has travelled outside its local envelope, a
	 *  different coherent segment -- or the same segment returning after a real
	 *  evidence gap -- begins a fresh settling episode. This prevents terminal
	 *  bounce from inheriting an earlier trip segment's escape while uninterrupted
	 *  sustained travel keeps proving real progress. */
	bool ShouldRebaseEscapedMotionEpisode(
		bool bEpisodeActive,
		bool bEscapedEnvelope,
		bool bCurrentDirectionCoherent,
		bool bMatchesDirectionCandidate,
		bool bResumedAfterCoherenceLoss);

	/** A short bounded translation may settle after the normal stop latency,
	 *  even when its excursion exceeds the strict quiet floor. Escaped/confirmed
	 *  motion, a stable current direction segment, and angular activity must
	 *  continue through the ordinary detector path. */
	bool CanUseBoundedMotionSettling(
		bool bHasSettlingEvidence,
		bool bEscapedEnvelope,
		bool bCandidateConfirmed,
		bool bCurrentDirectionStable,
		double EpisodeSeconds,
		bool bAngularTrendQuiet,
		double SettleSeconds = MotionStopConfirmationSeconds);

	/**
	 * Plain, observer-relative motion wording for an already-confirmed moving
	 * object. Vertical motion reads upward/downward in world space; horizontal
	 * motion prefers toward/away, then the observer's left/right. Speed is kept
	 * qualitative: slowly, no adjective, or quickly. RotationOnly yields
	 * "rotating" rather than inventing a linear direction.
	 */
	FString MotionPhrase(const FVector& WorldVelocity, bool bRotationOnly,
		const FTransform& ObserverTransform, const FVector& SubjectLocation);

	/** Compact value for the synthetic `<Object>.Movement` state: Stopped,
	 *  Moving, Upward, Downward, Toward You, Away, Left, Right, or Rotating.
	 *  Direction is observer-relative except world vertical; speed is omitted. */
	FString MotionStateValue(bool bIsMoving, const FVector& WorldVelocity,
		bool bRotationOnly, const FTransform& ObserverTransform,
		const FVector& SubjectLocation);

	/** Active motion is automatic for movement-awareness-enabled spatial objects.
	 *  A stationary object says "stopped" only when its explicit Movement state
	 *  is enabled, keeping ordinary static scene props token-free. */
	FString MotionStatusPhrase(bool bIsMoving, bool bReportStopped,
		const FVector& WorldVelocity, bool bRotationOnly,
		const FTransform& ObserverTransform, const FVector& SubjectLocation);

	/**
	 * Compute an actor's eye line — the top-centre of its silhouette, used as the
	 * start of a line-of-sight trace and as the vertical reference for the
	 * "above"/"below" decision.
	 *
	 * Prefers colliding-only bounds (capsule + movement-blocking props) so
	 * attached cosmetics (name plates, particle systems) don't inflate the head
	 * line; falls back to full bounds, then to a small symmetric default around
	 * the pivot when the actor has no usable bounds at all.
	 *
	 * @param Actor          The actor to measure (null yields the zero vector +
	 *                       default head/foot band).
	 * @param OutEyePos      World-space top-centre of the silhouette.
	 * @param OutHeadAboveUU Height of the silhouette top above the actor pivot.
	 * @param OutFootBelowUU Depth of the silhouette bottom below the pivot
	 *                       (negative).
	 */
	void ComputeEyeLine(const AActor* Actor, FVector& OutEyePos, float& OutHeadAboveUU, float& OutFootBelowUU);

	/**
	 * Visibility-channel line-of-sight test. Returns true when From has an
	 * unobstructed view of ObjectActor — either nothing blocked the ray, or the
	 * first blocking hit IS ObjectActor. Anything else in between counts as
	 * occlusion.
	 *
	 * Ignores IgnoreActor (the observer, whose own bounds the ray starts inside)
	 * and every player pawn (so a player standing in front of the target doesn't
	 * make an NPC "lose sight" of it). Fails open (true) when World is null, so a
	 * missing world never suppresses awareness.
	 */
	bool HasLineOfSight(const UWorld* World, const FVector& From, const FVector& To,
		const AActor* IgnoreActor, const AActor* ObjectActor);

	/**
	 * Phrase a subject's facing relative to an observer, for subjects that have a
	 * meaningful forward vector (characters, players): "facing toward you",
	 * "facing away from you", or "" when the subject faces roughly sideways or
	 * the inputs are degenerate. Compared on the horizontal plane only.
	 */
	FString FacingPhrase(const FVector& SubjectForward, const FVector& SubjectLoc, const FVector& ObserverLoc);
}
