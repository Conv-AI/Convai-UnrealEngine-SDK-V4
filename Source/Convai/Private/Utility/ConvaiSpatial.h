// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

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

	/** Bucket a straight-line distance (uu) into a band. Below NearbyUU is
	 *  "close by"; below ModerateUU is "some distance away"; beyond is "far away".
	 *  bReachable=false overrides everything with Unreachable (reserved for a
	 *  future nav-aware path; callers without nav data pass true). */
	EProximityBand ClassifyDistance(float DistanceUU, bool bReachable, float NearbyUU, float ModerateUU);

	/** Compose the band prefix with a direction phrase, e.g.
	 *  Nearby + "in front of you" -> "close by, in front of you";
	 *  Unreachable + "to your left" -> "no walking path, to your left". */
	FString BandPhrase(EProximityBand Band, const FString& Direction);

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
