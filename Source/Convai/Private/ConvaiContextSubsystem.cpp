// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiContextSubsystem.h"

#include "ConvaiSubsystem.h"
#include "ConvaiUtils.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiObjectComponent.h"
#include "ConvaiConversationComponent.h"
#include "../Convai.h"
#include "Utility/ConvaiSpatial.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"

void UConvaiContextSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// The clock starts lazily on the first object-component registration
	// (UConvaiSubsystem::RegisterObjectComponent -> RestartObjectPollClock).
}

void UConvaiContextSubsystem::Deinitialize()
{
	StopObjectPollClock();
	Super::Deinitialize();
}

float UConvaiContextSubsystem::ComputeEffectiveObjectPollInterval() const
{
	// Plain user-configured tick rate. The chatbot's own debounce window already
	// coalesces same-tick context updates, so the poll clock doesn't enforce a
	// floor relative to it — designers get exactly the cadence they ask for.
	return UConvaiUtils::GetObjectPollIntervalSeconds();
}

void UConvaiContextSubsystem::RestartObjectPollClock()
{
	StopObjectPollClock();

	UConvaiSubsystem* Subsystem = GetConvaiSubsystem();
	if (!Subsystem ||
		(Subsystem->GetAllObjectComponents().Num() == 0 &&
		 Subsystem->GetAllChatbotComponents().Num() == 0))
	{
		return; // nothing to evaluate yet (no objects and no chatbot observers)
	}

	const float Interval = ComputeEffectiveObjectPollInterval();
	ObjectPollTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UConvaiContextSubsystem::TickObjectPollClock),
		Interval);
}

void UConvaiContextSubsystem::StopObjectPollClock()
{
	if (ObjectPollTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ObjectPollTickerHandle);
		ObjectPollTickerHandle.Reset();
	}
}

bool UConvaiContextSubsystem::TickObjectPollClock(float /*DeltaTime*/)
{
	UConvaiSubsystem* Subsystem = GetConvaiSubsystem();
	if (!Subsystem)
	{
		ObjectPollTickerHandle.Reset();
		return false;
	}

	// Object tracked-property poll (also prunes destroyed object components).
	Subsystem->PollObjectComponents();

	// Spatial-awareness pass (objects + characters + players, per chatbot).
	EvaluateSpatialAwareness();

	// Keep ticking while there's anything to drive: object components (tracked
	// properties) OR chatbots (spatial-awareness observers). Returning false
	// removes this ticker; clear our stored handle to match.
	const bool bHasWork =
		Subsystem->GetAllObjectComponents().Num() > 0 ||
		Subsystem->GetAllChatbotComponents().Num() > 0;
	if (!bHasWork)
	{
		ObjectPollTickerHandle.Reset();
		return false;
	}
	return true; // keep ticking
}

UConvaiSubsystem* UConvaiContextSubsystem::GetConvaiSubsystem() const
{
	if (UGameInstance* GI = GetGameInstance())
	{
		return GI->GetSubsystem<UConvaiSubsystem>();
	}
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Spatial awareness
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	// A single thing a chatbot can perceive — an object, another character, or
	// the player. Built fresh each pass; not persisted.
	struct FSpatialEntity
	{
		FString  Name;
		FString  CacheKey;
		FVector  Location  = FVector::ZeroVector;
		FVector  Forward   = FVector::ForwardVector; // meaningful only when bHasFacing
		AActor*  Actor     = nullptr;                // for line-of-sight + frame
		uint8    Priority  = 0;                       // player=2 > character=1 > object=0
		bool     bHasFacing = false;                  // characters/players have a forward vector
		bool     bIsObject  = false;
		UConvaiChatbotComponent* AsChatbot = nullptr; // set when this subject IS a chatbot (skip self)
		// Set when this subject IS an object: every component making up this logical
		// object (one for a normal object, several for a merged same-named set). Used
		// for nav reachability — the group is reachable if ANY member can be pathed to.
		TArray<UConvaiObjectComponent*> ObjectMembers;
	};

	// Objects read as "the <name>"; characters/players use their name verbatim.
	FString MakeDisplayName(const FSpatialEntity& E)
	{
		return E.bIsObject ? FString::Printf(TEXT("the %s"), *E.Name) : E.Name;
	}

	int32 RunLLMRank(EC_RunLLMOption Option)
	{
		switch (Option)
		{
			case EC_RunLLMOption::Always: return 2;
			case EC_RunLLMOption::Auto:   return 1;
			default:                      return 0; // Never
		}
	}
	EC_RunLLMOption MaxResp(EC_RunLLMOption A, EC_RunLLMOption B)
	{
		return RunLLMRank(A) >= RunLLMRank(B) ? A : B;
	}

	FString JoinClauses(const TArray<FString, TInlineAllocator<2>>& Parts)
	{
		if (Parts.Num() == 0) { return FString(); }
		if (Parts.Num() == 1) { return Parts[0]; }
		return FString::Printf(TEXT("%s and %s"), *Parts[0], *Parts[1]);
	}

	// Point-based stacking/adjacency test: returns "on top of" / "underneath" /
	// "next to", or empty when the two aren't tightly coupled. Coarse (uses the
	// pivots, not full bounds) — good enough for "crate on plate".
	FString SupportRelation(const FVector& S, const FVector& T)
	{
		const float HorizDist = FVector(S.X - T.X, S.Y - T.Y, 0.0f).Size();
		const float Vert = S.Z - T.Z;
		constexpr float HorizThresholdUU = 100.0f;
		constexpr float VertThresholdUU  = 20.0f;
		if (HorizDist <= HorizThresholdUU)
		{
			if (Vert >  VertThresholdUU) { return TEXT("on top of"); }
			if (Vert < -VertThresholdUU) { return TEXT("underneath"); }
			return TEXT("next to");
		}
		return FString();
	}

	// How subject S reads relative to its anchor T. Support relations win; else a
	// direction, taken from the anchor's own frame when it has facing
	// (character/player), or from the observing chatbot's frame for object-object.
	FString ComposeRelationClause(const FSpatialEntity& S, const FSpatialEntity& Anchor, const FTransform& ObserverXform)
	{
		const FString Support = SupportRelation(S.Location, Anchor.Location);
		if (!Support.IsEmpty())
		{
			return FString::Printf(TEXT("%s %s"), *Support, *MakeDisplayName(Anchor));
		}

		const FTransform Frame = (Anchor.bHasFacing && Anchor.Actor)
			? Anchor.Actor->GetActorTransform()
			: ObserverXform;
		const FVector LocalDelta = Frame.InverseTransformVectorNoScale(S.Location - Anchor.Location);

		FVector EyeUnused; float HeadAbove, FootBelow;
		ConvaiSpatial::ComputeEyeLine(Anchor.bHasFacing ? Anchor.Actor : nullptr, EyeUnused, HeadAbove, FootBelow);

		const ConvaiSpatial::FDirection Dir = ConvaiSpatial::ComputeDirection(LocalDelta, HeadAbove, FootBelow);
		if (Dir.IsEmpty() && !Dir.bColocated)
		{
			return FString();
		}
		return ConvaiSpatial::DirectionToRef(Dir, MakeDisplayName(Anchor));
	}

	// The anchor for subject S: the closest OTHER entity within ClusterDist that
	// outranks S — higher priority, or same priority with an earlier name. The
	// key tiebreak makes each pair described exactly once (dedup) and stable
	// across passes. Returns null when nothing nearby outranks S.
	const FSpatialEntity* FindAnchor(const FSpatialEntity& S, int32 SelfIndex,
		const TArray<FSpatialEntity>& All, float ClusterDist)
	{
		const FSpatialEntity* Best = nullptr;
		const float ClusterDistSq = ClusterDist * ClusterDist;
		float BestDistSq = TNumericLimits<float>::Max();
		for (int32 i = 0; i < All.Num(); ++i)
		{
			if (i == SelfIndex) { continue; }
			const FSpatialEntity& T = All[i];
			const bool bOutranks = (T.Priority > S.Priority)
				|| (T.Priority == S.Priority && T.CacheKey < S.CacheKey);
			if (!bOutranks) { continue; }
			const float DistSq = FVector::DistSquared(S.Location, T.Location);
			if (DistSq <= ClusterDistSq && DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Best = &T;
			}
		}
		return Best;
	}
} // namespace

void UConvaiContextSubsystem::EvaluateSpatialAwareness()
{
	UConvaiSubsystem* Subsystem = GetConvaiSubsystem();
	if (!Subsystem)
	{
		return;
	}

	// Read the REGISTERED settings instance — the one the Project Settings page edits
	// (Convai::StartupModule registers Convai::Get().GetConvaiSettings(), and the rest of
	// the plugin reads it). NOT GetDefault<>()/the CDO: the CDO is a separate object the
	// live editor toggle never updates, so reading it ignored Project-Settings changes to
	// the spatial toggles until an editor restart reloaded the CDO from config.
	const UConvaiSettings* Settings = Convai::Get().GetConvaiSettings();
	if (!Settings || !Settings->bEnableSpatialAwareness)
	{
		for (auto It = SpatialCaches.CreateIterator(); It; ++It)
		{
			if (UConvaiChatbotComponent* Observer = It.Key().Get())
			{
				for (const TPair<FString, FString>& Pair : It.Value().Facts)
				{
					Observer->RemoveContextFact(Pair.Key);
				}
			}
		}
		SpatialCaches.Empty();
		return;
	}

	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;

	// ── Gather every subject once (objects + characters + players) ──
	TArray<FSpatialEntity> Subjects;
	// Objects are gathered as LOGICAL objects: BuildObjectGroups collapses a merged
	// same-named set into one group (centroid position, shared name), and leaves every
	// other object as a group of one. One FSpatialEntity per group.
	TArray<UConvaiSubsystem::FConvaiObjectGroup> ObjectGroups;
	Subsystem->BuildObjectGroups(ObjectGroups);
	for (const UConvaiSubsystem::FConvaiObjectGroup& Group : ObjectGroups)
	{
		// Include the logical object only if at least one member opts into spatial
		// awareness; that member also supplies the line-of-sight frame/actor.
		UConvaiObjectComponent* Representative = nullptr;
		for (UConvaiObjectComponent* Member : Group.Members)
		{
			if (IsValid(Member) && Member->bIncludeInSpatialAwareness)
			{
				Representative = Member;
				break;
			}
		}
		if (!Representative) { continue; }
		AActor* Owner = Representative->GetOwner();
		if (!IsValid(Owner)) { continue; }

		FSpatialEntity E;
		E.Name = Group.Name;
		// Keyed by the (unique, stable) logical-object name so change-gating and
		// retirement track the merged object correctly across polls.
		E.CacheKey = FString::Printf(TEXT("Object:%s"), *Group.Name.ToLower());
		E.Location = Group.Centroid;
		E.Actor = Owner;
		E.Priority = 0;
		E.bIsObject = true;
		E.ObjectMembers = Group.Members;
		Subjects.Add(MoveTemp(E));
	}
	for (UConvaiChatbotComponent* Bot : Subsystem->GetAllChatbotComponents())
	{
		if (!IsValid(Bot)) { continue; }
		AActor* Owner = Bot->GetOwner();
		if (!IsValid(Owner)) { continue; }
		const FString Name = Bot->GetConversationalName();
		if (Name.IsEmpty()) { continue; }

		FSpatialEntity E;
		E.Name = Name;
		E.CacheKey = FString::Printf(TEXT("Chatbot:%s"), *Bot->GetPathName());
		E.Location = Owner->GetActorLocation();
		E.Forward = Owner->GetActorForwardVector();
		E.Actor = Owner;
		E.Priority = 1;
		E.bHasFacing = true;
		E.AsChatbot = Bot;
		Subjects.Add(MoveTemp(E));
	}
	for (UConvaiPlayerComponent* Player : Subsystem->GetAllPlayerComponents())
	{
		if (!IsValid(Player)) { continue; }
		AActor* Owner = Player->GetOwner();
		if (!IsValid(Owner)) { continue; }
		// GetConversationalName() is public on the base UConvaiConversationComponent,
		// but UConvaiPlayerComponent declares its override private — call via the base.
		const FString Name = static_cast<const UConvaiConversationComponent*>(Player)->GetConversationalName();
		if (Name.IsEmpty()) { continue; }

		FSpatialEntity E;
		E.Name = Name;
		E.CacheKey = FString::Printf(TEXT("Player:%s"), *Player->GetPathName());
		E.Location = Owner->GetActorLocation();
		E.Forward = Owner->GetActorForwardVector();
		E.Actor = Owner;
		E.Priority = 2;
		E.bHasFacing = true;
		Subjects.Add(MoveTemp(E));
	}

	// ── Drop caches for destroyed observers ──
	for (auto It = SpatialCaches.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	// ── Per observer: compose one Context Fact per subject ──
	for (UConvaiChatbotComponent* Observer : Subsystem->GetAllChatbotComponents())
	{
		if (!IsValid(Observer)) { continue; }

		const FConvaiSpatialAwarenessPreferences& Pref = Observer->SpatialAwareness;
		FObserverSpatialCache& Cache = SpatialCaches.FindOrAdd(Observer);

		AActor* ObsOwner = Observer->GetOwner();
		const bool bWantsAnything = Pref.bReceiveSurroundings || Pref.bReceiveRelations;
		if (!IsValid(ObsOwner) || !bWantsAnything)
		{
			// Nothing wanted (or no body) — clear anything we previously published.
			for (const TPair<FString, FString>& Pair : Cache.Facts)
			{
				Observer->RemoveContextFact(Pair.Key);
			}
			Cache.Facts.Empty();
			continue;
		}

		const FVector ObsLoc = ObsOwner->GetActorLocation();
		const FTransform ObsXform = ObsOwner->GetActorTransform();
		FVector EyePos; float HeadAbove, FootBelow;
		ConvaiSpatial::ComputeEyeLine(ObsOwner, EyePos, HeadAbove, FootBelow);

		TSet<FString> CurrentKeys;

		for (int32 si = 0; si < Subjects.Num(); ++si)
		{
			const FSpatialEntity& Subj = Subjects[si];
			if (Subj.AsChatbot == Observer) { continue; } // never relative to itself

			// Line-of-sight gate: a subject the chatbot can't see is withheld.
			if (Settings->bEnableLineOfSight &&
				!ConvaiSpatial::HasLineOfSight(World, EyePos, Subj.Location, ObsOwner, Subj.Actor))
			{
				continue;
			}

			TArray<FString, TInlineAllocator<2>> Clauses;
			EC_RunLLMOption Resp = EC_RunLLMOption::Never;

			// Relation to the nearest higher-priority neighbour, computed first so a
			// tight support relation (on top of / underneath / next to) can take over
			// the position description instead of repeating distance/direction.
			FString RelationClause;
			bool bSupportRelation = false;
			if (Pref.bReceiveRelations && Settings->bEnableRelations)
			{
				if (const FSpatialEntity* Anchor =
						FindAnchor(Subj, si, Subjects, Settings->RelationClusterDistance))
				{
					bSupportRelation = !SupportRelation(Subj.Location, Anchor->Location).IsEmpty();
					RelationClause = ComposeRelationClause(Subj, *Anchor, ObsXform);
				}
			}

			// Category 1 — where the subject is relative to THIS chatbot.
			if (Pref.bReceiveSurroundings)
			{
				const FVector WorldDelta = Subj.Location - ObsLoc;
				// Nav reachability (objects only). Reused until the observer or the
				// object moves >100 uu (10000 = 100^2, kept squared) so the pathfind
				// doesn't run every poll. Characters/players have no nav target and
				// are treated as reachable. Unreachable -> "no walking path", which
				// is what the AI uses to avoid trying to move to things it can't path to.
				bool bReachable = true;
				if (Subj.ObjectMembers.Num() > 0)
				{
					auto& Nav = Cache.NavCache.FindOrAdd(Subj.CacheKey);
					const bool bRecompute = !Nav.bValid
						|| Nav.LastMemberCount != Subj.ObjectMembers.Num()
						|| FVector::DistSquared(ObsLoc, Nav.LastObserverLoc) >= 10000.0f
						|| FVector::DistSquared(Subj.Location, Nav.LastObjectLoc) >= 10000.0f;
					if (bRecompute)
					{
						// Reachable if ANY member can be pathed to — a scattered pile is
						// reachable as long as one of its objects is. Also track the NEAREST
						// reachable member so a merged object's move-to target points at the
						// closest one the bot can actually walk to.
						UConvaiObjectComponent* NearestReachable = nullptr;
						float NearestDistSq = TNumericLimits<float>::Max();
						for (UConvaiObjectComponent* Member : Subj.ObjectMembers)
						{
							if (!IsValid(Member)) { continue; }
							AActor* GA = nullptr; USceneComponent* GC = nullptr;
							FVector ResolvedLoc = FVector::ZeroVector; float AR = 0.0f;
							EConvaiMoveTarget Mode = EConvaiMoveTarget::Actor;
							bool bResolved = false, bAlreadyThere = false, bReach = false;
							FVector PathEnd = FVector::ZeroVector; TArray<FVector> PathPoints;
							Member->ObjectEntry.ResolveGoalLocation(ObsOwner, /*bForceRefresh*/ false,
								GA, GC, ResolvedLoc, AR, Mode, bResolved, bAlreadyThere, bReach, PathEnd, PathPoints);
							if (bReach && IsValid(Member->GetOwner()))
							{
								const float DistSq = FVector::DistSquared(ObsLoc, Member->GetOwner()->GetActorLocation());
								if (DistSq < NearestDistSq)
								{
									NearestDistSq = DistSq;
									NearestReachable = Member;
								}
							}
						}
						Nav.bReachable = (NearestReachable != nullptr);
						Nav.LastObserverLoc = ObsLoc;
						Nav.LastObjectLoc = Subj.Location;
						Nav.LastMemberCount = Subj.ObjectMembers.Num();
						Nav.bValid = true;

						// For a merged set, repoint THIS chatbot's object entry at the nearest
						// reachable member so "go to <name>" walks to the closest one. A single
						// object's Ref is already itself. Local nav target only — Ref isn't part
						// of the server payload, so this causes no scene-metadata churn.
						if (Subj.ObjectMembers.Num() > 1 && NearestReachable)
						{
							if (FConvaiObjectEntry* MovingEntry = Observer->EnvironmentData.Objects.FindByPredicate(
									[&Subj](const FConvaiObjectEntry& E){ return E.Name == Subj.Name; }))
							{
								MovingEntry->Ref = NearestReachable->GetOwner();
							}
						}
					}
					bReachable = Nav.bReachable;
				}

				const ConvaiSpatial::EProximityBand Band =
					ConvaiSpatial::ClassifyDistance(WorldDelta.Size(), bReachable,
						Settings->NearbyDistance, Settings->ModerateDistance);

				if (bSupportRelation && !RelationClause.IsEmpty())
				{
					// Position is already pinned by the support relation, so drop the
					// redundant distance + direction; keep only the reachability note
					// (when it can't be walked to), appended to the relation clause.
					if (Band == ConvaiSpatial::EProximityBand::Unreachable)
					{
						RelationClause = FString::Printf(TEXT("%s, with no walking path to it"), *RelationClause);
						Resp = MaxResp(Resp, Pref.SurroundingsResponse);
					}
				}
				else
				{
					const FVector LocalDelta = ObsXform.InverseTransformVectorNoScale(WorldDelta);
					const ConvaiSpatial::FDirection Dir =
						ConvaiSpatial::ComputeDirection(LocalDelta, HeadAbove, FootBelow);
					FString Ego = ConvaiSpatial::BandPhrase(Band, ConvaiSpatial::DirectionToYou(Dir));
					if (Subj.bHasFacing)
					{
						const FString Facing = ConvaiSpatial::FacingPhrase(Subj.Forward, Subj.Location, ObsLoc);
						if (!Facing.IsEmpty())
						{
							Ego = FString::Printf(TEXT("%s, %s"), *Ego, *Facing);
						}
					}
					Clauses.Add(Ego);
					Resp = MaxResp(Resp, Pref.SurroundingsResponse);
				}
			}

			// Category 2 — how the subject relates to a nearby higher-priority
			// neighbour (bagged into this same fact, described once per pair).
			if (!RelationClause.IsEmpty())
			{
				Clauses.Add(RelationClause);
				Resp = MaxResp(Resp, Pref.RelationsResponse);
			}

			if (Clauses.Num() == 0) { continue; }

			FString Sentence = FString::Printf(TEXT("%s is %s."), *MakeDisplayName(Subj), *JoinClauses(Clauses));
			if (Sentence.Len() > 0)
			{
				Sentence[0] = FChar::ToUpper(Sentence[0]);
			}

			CurrentKeys.Add(Subj.CacheKey);
			const FString* Last = Cache.Facts.Find(Subj.CacheKey);
			if (!Last || *Last != Sentence)
			{
				Observer->SetContextFact(Sentence, Subj.CacheKey, Resp, /*bFlushImmediately*/ false);
				Cache.Facts.Add(Subj.CacheKey, Sentence);
			}
		}

		// Retire facts for subjects that dropped out (destroyed, out of view, or
		// no longer wanted).
		for (auto It = Cache.Facts.CreateIterator(); It; ++It)
		{
			if (!CurrentKeys.Contains(It.Key()))
			{
				Observer->RemoveContextFact(It.Key());
				It.RemoveCurrent();
			}
		}

		// Drop nav-cache entries for subjects no longer present this pass.
		for (auto It = Cache.NavCache.CreateIterator(); It; ++It)
		{
			if (!CurrentKeys.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
	}
}
