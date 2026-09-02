// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiDefinitions.h"
#include "ConvaiActionUtils.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiUtils.h"
#include "Internationalization/Regex.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "NavMesh/RecastNavMesh.h"
#include "AI/Navigation/NavAgentInterface.h"
#include "Math/NumericLimits.h"
#include "../Convai.h"

DEFINE_LOG_CATEGORY(ConvaiDefinitionsLog);

namespace
{
	// Render the lowercase wire word for a declared type. Only Auto elides — every
	// other type emits a hint so the wire format round-trips cleanly. (If Reference
	// elided, parse-back would demote it to Auto since there's no hint to recognize.)
	const TCHAR* TypeWireWord(EConvaiActionParamType T)
	{
		switch (T)
		{
		case EConvaiActionParamType::Reference: return TEXT("ref");
		case EConvaiActionParamType::String:    return TEXT("string");
		case EConvaiActionParamType::Number:    return TEXT("number");
		case EConvaiActionParamType::Bool:      return TEXT("bool");
		case EConvaiActionParamType::Enum:      return TEXT("enum");
		default:                                return nullptr; // Auto
		}
	}

	EConvaiActionParamType ParseTypeWireWord(const FString& Word)
	{
		const FString Lower = Word.ToLower();
		if (Lower == TEXT("string"))    return EConvaiActionParamType::String;
		if (Lower == TEXT("number"))    return EConvaiActionParamType::Number;
		if (Lower == TEXT("bool"))      return EConvaiActionParamType::Bool;
		if (Lower == TEXT("enum"))      return EConvaiActionParamType::Enum;
		if (Lower == TEXT("ref") ||
			Lower == TEXT("reference")) return EConvaiActionParamType::Reference;
		return EConvaiActionParamType::Auto;
	}

}

FString FConvaiAction::ToActionConfigString() const
{
	// 1. Name
	FString Out = Name;

	// 2. Param placeholders.
	for (const FConvaiActionParam& P : Parameters)
	{
		if (!P.Connector.IsEmpty())
		{
			Out += FString::Printf(TEXT(" %s"), *P.Connector);
		}

		FString Hint;
		if (P.Type == EConvaiActionParamType::Enum)
		{
			if (P.EnumType)
			{
				const TArray<FString> Labels = UConvaiActions::GetEnumLabels(P.EnumType);
				if (Labels.Num() > 0)
				{
					Hint += FString::Printf(TEXT(" [%s]"), *FString::Join(Labels, TEXT("|")));
				}
			}
			else
			{
				Hint += TEXT(" [ERROR: EnumType not set]");
			}
		}
		else if (P.Choices.Num() > 0)
		{
			Hint += FString::Printf(TEXT(" [%s]"), *FString::Join(P.Choices, TEXT("|")));
		}

		if (const TCHAR* TypeWord = TypeWireWord(P.Type))
		{
			Hint += FString::Printf(TEXT(": %s"), TypeWord);
		}

		Out += FString::Printf(TEXT(" {%s%s}"), *P.Name, *Hint);
	}

	// 3. Conditional description tail — build only out of non-empty bits.
	const bool bHasActionDesc = !Description.IsEmpty();

	TArray<FString> ParamSentences;
	for (const FConvaiActionParam& P : Parameters)
	{
		if (!P.Description.IsEmpty())
		{
			ParamSentences.Add(FString::Printf(TEXT("%s: %s"), *P.Name, *P.Description));
		}
	}

	if (bHasActionDesc || ParamSentences.Num() > 0)
	{
		Out += TEXT(" — ");
		if (bHasActionDesc)
		{
			Out += Description;
			if (ParamSentences.Num() > 0)
			{
				Out += TEXT(". ");
			}
		}
		Out += FString::Join(ParamSentences, TEXT(". "));
		Out += TEXT(".");
	}

	return Out;
}

bool FConvaiAction::ParseFromActionConfigString(const FString& Source, FConvaiAction& Out)
{
	const FString Trimmed = Source.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return false;
	}

	// Split into [head] and [tail] on the first " — " (em-dash). Tail may be absent.
	FString Head, Tail;
	const FString Separator = TEXT(" — ");
	int32 SepIdx = INDEX_NONE;
	if (Trimmed.FindChar(TEXT('—'), SepIdx))
	{
		// Step back one char (the leading space) and forward two (space + em-dash + space).
		Head = Trimmed.Left(SepIdx).TrimEnd();
		Tail = Trimmed.RightChop(SepIdx + 1).TrimStart();
	}
	else
	{
		Head = Trimmed;
	}

	// Find every {...} placeholder in the head. We use the offsets to slice out the
	// connector text immediately preceding each placeholder (or — for the first one —
	// to identify the action name). Invariant: '}' must not appear inside choice values
	// (they're joined with '|') or inside the type hint, so a non-greedy [^}]* is safe.
	struct FPlaceholderHit { int32 OpenIdx; int32 CloseIdx; FString Inner; };
	TArray<FPlaceholderHit> Hits;
	{
		const FRegexPattern Pattern(TEXT("\\{([^}]*)\\}"));
		FRegexMatcher Matcher(Pattern, Head);
		while (Matcher.FindNext())
		{
			FPlaceholderHit H;
			H.OpenIdx  = Matcher.GetMatchBeginning();
			H.CloseIdx = Matcher.GetMatchEnding();
			H.Inner    = Matcher.GetCaptureGroup(1);
			Hits.Add(H);
		}
	}

	FConvaiAction Built;
	if (Hits.Num() == 0)
	{
		// No placeholders — head is just the action name.
		Built.Name = Head.TrimStartAndEnd();
		if (Built.Name.IsEmpty())
		{
			return false;
		}
	}
	else
	{
		// Action name = everything before the first placeholder. The space before the
		// placeholder belongs to the separator, so trim trailing whitespace.
		Built.Name = Head.Left(Hits[0].OpenIdx).TrimStartAndEnd();
		if (Built.Name.IsEmpty())
		{
			return false;
		}

		// Each placeholder gets parsed into a FConvaiActionParam. Connector text is
		// whatever sits between the previous placeholder (or the action name for the
		// first param) and this one's opening quote.
		int32 PrevEnd = Hits[0].OpenIdx + Built.Name.Len();
		for (int32 i = 0; i < Hits.Num(); ++i)
		{
			const FPlaceholderHit& H = Hits[i];

			FConvaiActionParam P;

			// Connector — text between PrevEnd and the placeholder open. The first
			// placeholder uses the gap after Name; subsequent ones, after the prior close.
			const int32 ConnectorStart = (i == 0) ? Built.Name.Len() : Hits[i - 1].CloseIdx;
			const int32 ConnectorLen   = H.OpenIdx - ConnectorStart;
			if (ConnectorLen > 0)
			{
				P.Connector = Head.Mid(ConnectorStart, ConnectorLen).TrimStartAndEnd();
			}

			// Inner: <name [choices]: type>
			FString Inner = H.Inner;

			// Pull off type hint after ": " (last colon, in case names contain colons).
			int32 ColonIdx = INDEX_NONE;
			Inner.FindLastChar(TEXT(':'), ColonIdx);
			if (ColonIdx != INDEX_NONE)
			{
				P.Type = ParseTypeWireWord(Inner.RightChop(ColonIdx + 1).TrimStartAndEnd());
				Inner = Inner.Left(ColonIdx).TrimEnd();
			}
			else
			{
				P.Type = EConvaiActionParamType::Auto;
			}

			// Pull off choices in [..|..|..].
			{
				const FRegexPattern ChoicePattern(TEXT("\\[([^\\]]+)\\]"));
				FRegexMatcher M(ChoicePattern, Inner);
				if (M.FindNext())
				{
					const FString ChoicesBlob = M.GetCaptureGroup(1);
					ChoicesBlob.ParseIntoArray(P.Choices, TEXT("|"), true);
					for (FString& C : P.Choices) { C.TrimStartAndEndInline(); }
					Inner = (Inner.Left(M.GetMatchBeginning()) + Inner.RightChop(M.GetMatchEnding())).TrimEnd();
				}
			}

			P.Name = Inner.TrimStartAndEnd();
			if (P.Name.IsEmpty())
			{
				return false;
			}
			Built.Parameters.Add(MoveTemp(P));
		}
	}

	// Tail: split sentences, route by `<paramName>: ...` prefix into per-param descriptions
	// or — when no name match — accumulate into the action's Description.
	if (!Tail.IsEmpty())
	{
		// Strip the trailing period if present.
		FString Body = Tail;
		Body.RemoveFromEnd(TEXT("."));

		TArray<FString> Sentences;
		Body.ParseIntoArray(Sentences, TEXT(". "), true);

		TArray<FString> ActionDescChunks;
		for (FString& S : Sentences)
		{
			S.TrimStartAndEndInline();
			if (S.IsEmpty()) continue;

			int32 ColonIdx = INDEX_NONE;
			if (S.FindChar(TEXT(':'), ColonIdx))
			{
				const FString Lhs = S.Left(ColonIdx).TrimStartAndEnd();
				const FString Rhs = S.RightChop(ColonIdx + 1).TrimStartAndEnd();
				bool bMatched = false;
				for (FConvaiActionParam& Pm : Built.Parameters)
				{
					if (Pm.Name.Equals(Lhs, ESearchCase::IgnoreCase))
					{
						Pm.Description = Rhs;
						bMatched = true;
						break;
					}
				}
				if (!bMatched)
				{
					ActionDescChunks.Add(S);
				}
			}
			else
			{
				ActionDescChunks.Add(S);
			}
		}

		Built.Description = FString::Join(ActionDescChunks, TEXT(". "));
	}

	Out = MoveTemp(Built);
	return true;
}

// ── Named movement points → sub-objects ──────────────────────────────

FString FConvaiObjectEntry::NormalizeMovementPointName(const FString& Raw)
{
	FString Out;
	Out.Reserve(Raw.Len());
	bool bPendingSpace = false;
	for (const TCHAR C : Raw)
	{
		if (FChar::IsWhitespace(C))
		{
			bPendingSpace = Out.Len() > 0; // leading whitespace never emits
			continue;
		}
		if (bPendingSpace)
		{
			Out.AppendChar(TEXT(' '));
			bPendingSpace = false;
		}
		Out.AppendChar(C);
	}
	return Out;
}

FString FConvaiObjectEntry::EffectiveMovementPointName(const FConvaiMovementPoint& Point)
{
	return Point.bCreatesSeparateDestination
		? NormalizeMovementPointName(Point.Name) : FString();
}

void FConvaiObjectEntry::CollectMovementPointSubNames(
	const TArray<const FConvaiObjectEntry*>& MemberEntries,
	TArray<FString>& OutDisplayNames)
{
	OutDisplayNames.Reset();
	TArray<FString> SeenKeys; // lower-cased, parallel to OutDisplayNames
	for (const FConvaiObjectEntry* Member : MemberEntries)
	{
		if (!Member)
		{
			continue;
		}
		for (const FConvaiMovementPoint& Point : Member->MovementPoints)
		{
			if (!Point.bEnabled) // disabled points never create identities
			{
				continue;
			}
			// Create Separate Destination is an explicit opt-in: a ticked,
			// named point ALWAYS expands — no dormancy heuristics needed.
			const FString Display = EffectiveMovementPointName(Point);
			if (Display.IsEmpty())
			{
				continue;
			}
			const FString Key = Display.ToLower();
			if (!SeenKeys.Contains(Key))
			{
				SeenKeys.Add(Key);
				OutDisplayNames.Add(Display);
			}
		}
	}
}

void FConvaiObjectEntry::FilterMovementPointsToObjectItself()
{
	MovementPoints.RemoveAll([](const FConvaiMovementPoint& P)
		{ return !EffectiveMovementPointName(P).IsEmpty(); });
}

void FConvaiObjectEntry::FilterMovementPointsToName(const FString& DisplayName)
{
	const FString Key = NormalizeMovementPointName(DisplayName).ToLower();
	MovementPoints.RemoveAll([&Key](const FConvaiMovementPoint& P)
		{ return EffectiveMovementPointName(P).ToLower() != Key; });
}

FString FConvaiObjectEntry::MovementPointDestinationDescription(
	const FString& BaseName,
	const TArray<const FConvaiObjectEntry*>& MemberEntries,
	const FString& SubDisplayName)
{
	const FString Key = NormalizeMovementPointName(SubDisplayName).ToLower();
	bool bHasWorldFixedPoint = false;
	bool bHasRelativePoint = false;
	for (const FConvaiObjectEntry* Member : MemberEntries)
	{
		if (!Member)
		{
			continue;
		}
		for (const FConvaiMovementPoint& Point : Member->MovementPoints)
		{
			if (!Point.bEnabled || EffectiveMovementPointName(Point).ToLower() != Key)
			{
				continue;
			}
			bHasWorldFixedPoint |=
				Point.Attachment == EConvaiMovementPointAttachment::KeepWorldPosition;
			bHasRelativePoint |=
				Point.Attachment == EConvaiMovementPointAttachment::RelativeToObject;
		}
	}
	if (bHasWorldFixedPoint && !bHasRelativePoint)
	{
		return FString::Printf(
			TEXT("A fixed standing location for accessing %s; it is not %s itself."),
			*BaseName, *BaseName);
	}
	if (bHasRelativePoint && !bHasWorldFixedPoint)
	{
		return FString::Printf(
			TEXT("A standing location that moves with %s; it is not %s itself."),
			*BaseName, *BaseName);
	}
	return FString::Printf(
		TEXT("A separate standing location for %s; it is not %s itself."),
		*BaseName, *BaseName);
}

FConvaiObjectEntry FConvaiObjectEntry::MakeMovementPointSubEntry(const FString& SubDisplayName) const
{
	FConvaiObjectEntry Sub = *this;
	Sub.Name = FString::Printf(TEXT("%s %s"), *Name, *SubDisplayName);
	Sub.FilterMovementPointsToName(SubDisplayName);
	const TArray<const FConvaiObjectEntry*> DescriptionMembers{ this };
	Sub.Description = MovementPointDestinationDescription(
		Name, DescriptionMembers, SubDisplayName);
	// A named side must never quietly resolve to the object body — that
	// would defeat "go to the other side".
	Sub.bFallbackToObjectWhenPointsUnreachable = false;
	Sub.bIsMovementPointSubObject = true;
	Sub.MovementPointSubObjectBaseName = Name;
	Sub.MovementPointSubObjectPointName = NormalizeMovementPointName(SubDisplayName);
	Sub.ResolvedComponent = nullptr;
	return Sub;
}

// ── FConvaiObjectEntry component resolver ─────────────────────────────
bool FConvaiObjectEntry::HasComponentFilters() const
{
	return ObjectReference == EConvaiObjectReference::SpecificComponent
		&& !ComponentName.IsEmpty();
}

USceneComponent* FConvaiObjectEntry::ResolveComponent()
{
	AActor* Actor = Ref.Get();
	if (!Actor || !HasComponentFilters())
	{
		ResolvedComponent = nullptr;
		return nullptr;
	}

	if (USceneComponent* Cached = ResolvedComponent.Get())
	{
		if (Cached->GetOwner() == Actor
			&& Cached->GetName().Contains(ComponentName, ESearchCase::IgnoreCase))
		{
			return Cached;
		}
	}
	ResolvedComponent = nullptr;

	TArray<USceneComponent*> All;
	Actor->GetComponents<USceneComponent>(All);

	USceneComponent* FirstMatch = nullptr;
	int32 MatchCount = 0;
	for (USceneComponent* C : All)
	{
		if (!C) { continue; }
		if (!C->GetName().Contains(ComponentName, ESearchCase::IgnoreCase))
		{
			continue;
		}

		if (!FirstMatch) { FirstMatch = C; }
		++MatchCount;
	}

	if (MatchCount > 1)
	{
		// Misconfigured filter silently targeting the wrong sub-component is the most
		// likely failure mode here — warn loudly so designers tighten the text.
		CONVAI_LOG(ConvaiDefinitionsLog, Warning,
			TEXT("FConvaiObjectEntry '%s': %d components on %s match Component Name '%s' — using '%s'. Tighten the text to disambiguate."),
			*Name, MatchCount, *Actor->GetName(), *ComponentName,
			FirstMatch ? *FirstMatch->GetName() : TEXT("<null>"));
	}
	else if (MatchCount == 0)
	{
		CONVAI_LOG(ConvaiDefinitionsLog, Warning,
			TEXT("FConvaiObjectEntry '%s': no component on %s matched Component Name '%s' — falling back to actor origin."),
			*Name, *Actor->GetName(), *ComponentName);
	}

	ResolvedComponent = FirstMatch;
	return FirstMatch;
}

namespace
{
	// Horizontal (X/Y) distance from Probe to the object's bbox footprint:
	// 0 when Probe is directly above / inside the footprint, otherwise the gap
	// to the nearest footprint face. Shared by the reachability and arrival
	// tests so "how far from the object" is measured identically in both.
	float HorizontalDistanceToFootprint(
		const FVector& Probe, const FVector& ObjOrigin, const FVector& ObjExtent)
	{
		const FVector ClosestOnBoxXY(
			FMath::Clamp(Probe.X, ObjOrigin.X - ObjExtent.X, ObjOrigin.X + ObjExtent.X),
			FMath::Clamp(Probe.Y, ObjOrigin.Y - ObjExtent.Y, ObjOrigin.Y + ObjExtent.Y),
			Probe.Z);
		return FVector::Dist2D(Probe, ClosestOnBoxXY);
	}

	// Per-source navigation state hoisted out of the per-goal query so resolving
	// several candidate goals (Movement Points) against one SourceActor sets it
	// up ONCE: nav system, agent props, projected start, and nav data. Mirrors
	// AAIController::BuildPathfindingQuery: pulls nav-agent props + nav-frame
	// location from INavAgentInterface (falling back to actor pivot) and projects
	// the start onto the navmesh so a pawn-pivot-at-feet setup still gets a valid
	// start tile.
	struct FNavQueryContext
	{
		UNavigationSystemV1* NavSys  = nullptr;
		const ANavigationData* NavData = nullptr;
		FNavAgentProperties AgentProps;
		FVector StartLoc = FVector::ZeroVector;
		float SourceHeight = 0.0f; // colliding-bounds height, for vertical tolerance

		bool Init(AActor* SourceActor)
		{
			if (!IsValid(SourceActor))
			{
				return false;
			}
			UWorld* World = SourceActor->GetWorld();
			if (!World)
			{
				return false;
			}
			NavSys = UNavigationSystemV1::GetNavigationSystem(World);
			if (!NavSys)
			{
				return false;
			}

			StartLoc = SourceActor->GetActorLocation();
			if (const INavAgentInterface* NavAgent = Cast<INavAgentInterface>(SourceActor))
			{
				AgentProps = NavAgent->GetNavAgentPropertiesRef();
				StartLoc   = NavAgent->GetNavAgentLocation();
			}

			FNavLocation ProjectedStart;
			if (NavSys->ProjectPointToNavigation(StartLoc, ProjectedStart,
				FVector(100.0f, 100.0f, 200.0f), &AgentProps))
			{
				const float ProjectionDelta = FVector::Dist(StartLoc, ProjectedStart.Location);
				if (ProjectionDelta > 50.0f)
				{
					CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
						TEXT("Reachability: start projected by %.1f uu (%s -> %s) on %s — verify SourceActor is grounded."),
						ProjectionDelta,
						*StartLoc.ToCompactString(), *ProjectedStart.Location.ToCompactString(),
						*SourceActor->GetName());
				}
				StartLoc = ProjectedStart.Location;
			}

			NavData = NavSys->GetNavDataForProps(AgentProps, StartLoc);
			if (!NavData)
			{
				CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
					TEXT("Reachability: no NavData for agent at %s on %s — unreachable."),
					*StartLoc.ToCompactString(), *SourceActor->GetName());
				return false;
			}

			// IMPORTANT: colliding components only — non-colliding scene/billboard/
			// audio components anchored at the pivot would otherwise inflate the
			// vertical span and loosen the Z tolerance below.
			FVector CbOrigin, CbExtent;
			SourceActor->GetActorBounds(/*bOnlyCollidingComponents*/ true, CbOrigin, CbExtent);
			SourceHeight = CbExtent.Z * 2.0f;
			return true;
		}
	};

	// One nav path query against an already-initialised context. Projects the
	// goal seed onto the navmesh (so above-mesh goals — objects on tables /
	// shelves — still snap to where the agent would actually walk), runs a
	// synchronous pathfind with partial paths allowed (path points come back
	// populated even for unreachable goals), and returns the path. Reachability
	// classification is the caller's job — point goals and bbox-footprint goals
	// measure "close enough" differently.
	bool RunNavPathQuery(
		AActor* SourceActor,
		const FNavQueryContext& Ctx,
		const FVector& GoalLoc,
		float GoalSeedZ,
		FVector& OutFinalPoint,
		TArray<FVector>& OutPathPoints,
		float& OutPathLength)
	{
		OutFinalPoint = FVector::ZeroVector;
		OutPathPoints.Reset();
		OutPathLength = 0.0f;

		const FVector GoalSeed(GoalLoc.X, GoalLoc.Y, GoalSeedZ);
		FNavLocation ProjectedGoal;
		FVector QueryEndLoc = GoalLoc;
		const bool bGoalProjected = Ctx.NavSys->ProjectPointToNavigation(
			GoalSeed, ProjectedGoal,
			FVector(200.0f, 200.0f, 1000.0f), &Ctx.AgentProps);
		if (bGoalProjected)
		{
			QueryEndLoc = ProjectedGoal.Location;
		}

		FPathFindingQuery Query(SourceActor, *Ctx.NavData, Ctx.StartLoc, QueryEndLoc);
		Query.SetAllowPartialPaths(true);

		const FPathFindingResult Result = Ctx.NavSys->FindPathSync(Query);
		if (!Result.IsSuccessful() || !Result.Path.IsValid())
		{
			CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
				TEXT("Reachability: FindPathSync failed (Start=%s, Goal=%s, ProjectedEnd=%s, GoalProjected=%d) — unreachable."),
				*Ctx.StartLoc.ToCompactString(), *GoalLoc.ToCompactString(),
				*QueryEndLoc.ToCompactString(), bGoalProjected ? 1 : 0);
			return false;
		}

		const TArray<FNavPathPoint>& Points = Result.Path->GetPathPoints();
		if (Points.Num() == 0)
		{
			return false;
		}

		OutPathPoints.Reserve(Points.Num());
		for (const FNavPathPoint& P : Points)
		{
			OutPathPoints.Add(P.Location);
		}
		OutFinalPoint  = Points.Last().Location;
		OutPathLength  = Result.Path->GetLength();

		// Physical sanity check: while the dynamic navmesh catches up with a
		// moving surface (platform lifting off the ground it was connected to),
		// FindPathSync can briefly return a corridor with a near-vertical jump
		// no agent can walk — which would classify a platform hovering overhead
		// as reachable. Reject any segment that climbs/drops more than the
		// source's own height with almost no horizontal travel, unless it is an
		// authored off-mesh nav link (those legitimately move vertically). Path
		// points stay populated so unreachable paths still debug-draw.
		const float MaxVerticalStep = FMath::Max(Ctx.SourceHeight, 150.0f);
		for (int32 i = 0; i + 1 < Points.Num(); ++i)
		{
			if (FNavMeshNodeFlags(Points[i].Flags).IsNavLink())
			{
				continue;
			}
			const FVector& A = Points[i].Location;
			const FVector& B = Points[i + 1].Location;
			if (FMath::Abs(B.Z - A.Z) > MaxVerticalStep && FVector::Dist2D(A, B) < 150.0f)
			{
				CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
					TEXT("Reachability: path segment %d has a %.0f uu vertical jump over %.0f uu horizontal (no nav link) — rejecting as stale-navmesh corridor."),
					i, FMath::Abs(B.Z - A.Z), FVector::Dist2D(A, B));
				return false;
			}
		}
		return true;
	}

	// Vertical "close enough" shared by both goal shapes: |final-point Z to the
	// nearest Z inside [MinZ, MaxZ]| within the source's own height (150 floor).
	bool WithinVerticalSpan(float PointZ, float MinZ, float MaxZ, float SourceHeight)
	{
		const float NearestZ = FMath::Clamp(PointZ, MinZ, MaxZ);
		return FMath::Abs(PointZ - NearestZ) <= FMath::Max(SourceHeight, 150.0f);
	}

	// Object-fallback reachability (no Movement Points): path + classify against
	// the object's bbox footprint — the final point must reach within
	// max(AcceptanceRadius × 2, 150 uu) of the nearest footprint face (the ×2
	// widens past the AIMoveTo trigger, 150 floors tightly-tuned values), plus
	// the source's vertical span.
	bool ComputeNavReachability(
		AActor* SourceActor,
		const FVector& GoalLoc,
		const FVector& ObjOrigin,
		const FVector& ObjExtent,
		float AcceptanceRadius,
		FVector& OutFinalPoint,
		TArray<FVector>& OutPathPoints,
		float& OutPathLength)
	{
		FNavQueryContext Ctx;
		if (!Ctx.Init(SourceActor))
		{
			OutFinalPoint = FVector::ZeroVector;
			OutPathPoints.Reset();
			OutPathLength = 0.0f;
			return false;
		}

		// Seed Z at the bbox bottom so a floor-standing object's seed sits on or
		// just above the navmesh.
		if (!RunNavPathQuery(SourceActor, Ctx, GoalLoc, ObjOrigin.Z - ObjExtent.Z,
			OutFinalPoint, OutPathPoints, OutPathLength))
		{
			return false;
		}

		const float HorizontalDist =
			HorizontalDistanceToFootprint(OutFinalPoint, ObjOrigin, ObjExtent);
		const float HorizTol = FMath::Max(AcceptanceRadius * 2.0f, 150.0f);
		if (HorizontalDist > HorizTol)
		{
			CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
				TEXT("Reachability: path ends %.1f uu from bbox footprint (tol %.1f) — unreachable."),
				HorizontalDist, HorizTol);
			return false;
		}

		const bool bReachable = WithinVerticalSpan(OutFinalPoint.Z,
			ObjOrigin.Z - ObjExtent.Z, ObjOrigin.Z + ObjExtent.Z, Ctx.SourceHeight);
		CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
			TEXT("Reachability: bbox Z=[%.1f, %.1f] FinalPoint.Z=%.1f SourceHeight=%.1f => %s"),
			ObjOrigin.Z - ObjExtent.Z, ObjOrigin.Z + ObjExtent.Z,
			OutFinalPoint.Z, Ctx.SourceHeight,
			bReachable ? TEXT("REACHABLE") : TEXT("unreachable"));
		return bReachable;
	}
} // namespace

FVector FConvaiMovementPoint::ResolveWorldLocation(const AActor* RefActor, const USceneComponent* RefComponent) const
{
	if (Attachment == EConvaiMovementPointAttachment::RelativeToObject)
	{
		// Component-scoped objects anchor their points to the component so the
		// point rides along with a moving sub-part (drawer, turret, door leaf).
		if (IsValid(RefComponent))
		{
			return RefComponent->GetComponentTransform().TransformPosition(Transform.GetLocation());
		}
		if (IsValid(RefActor))
		{
			return RefActor->GetActorTransform().TransformPosition(Transform.GetLocation());
		}
	}
	// KeepWorldPosition — or a relative point with no live anchor, which can
	// only resolve as authored (world) coordinates.
	return Transform.GetLocation();
}

void FConvaiObjectEntry::ResolveGoalLocation(
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
	int32& OutMovementPointIndex)
{
	// Defaults so every output is well-defined regardless of branch taken.
	OutGoalActor          = Ref.Get();
	OutGoalComponent      = nullptr;
	OutGoalLocation       = OptionalPositionVector;
	OutAcceptanceRadius   = AcceptanceRadius;
	bOutMoveToLocation    = (ObjectReference == EConvaiObjectReference::SpecificComponent);
	bOutSuccess           = false;
	bOutAlreadyThere      = false;
	bOutReachable         = false;
	OutPathEndPoint       = FVector::ZeroVector;
	OutPathPoints.Reset();
	OutGoalTravelDistance = 0.0f;
	OutMovementPointIndex = INDEX_NONE;

	AActor* Actor = Ref.Get();
	if (!Actor)
	{
		// No actor — pure position-marker entry. Leave OutGoalLocation at the
		// snapshotted OptionalPositionVector; bOutSuccess stays false because
		// downstream movement consumers can't AIMoveTo a destroyed reference.
		return;
	}

	// Re-resolve the cached sub-component up front so the source-relative outputs
	// below (which read ResolvedComponent for proximity/arrival bbox) see a value
	// that matches the current mode + filters. The cache-validation inside
	// ResolveComponent already rejects stale entries when filters changed, but the
	// Actor-mode branch below never calls ResolveComponent itself, so without this
	// the cache could linger across mode switches.
	ResolveComponent();

	// ── Movement Points ─────────────────────────────────────────────────
	// Authored access points take over movement resolution entirely; the
	// actor/component reference below is only the fallback when none are
	// enabled (or when all are unreachable and the explicit fallback is on).
	TArray<int32, TInlineAllocator<8>> EnabledPoints;
	for (int32 i = 0; i < MovementPoints.Num(); ++i)
	{
		if (MovementPoints[i].bEnabled)
		{
			EnabledPoints.Add(i);
		}
	}

	if (EnabledPoints.Num() > 0)
	{
		// Relative points anchor to the resolved component when the object is
		// component-scoped (so a point on a drawer rides the drawer), else to
		// the actor. ResolveComponent already ran at the top of this function.
		const USceneComponent* PointAnchor =
			(ObjectReference == EConvaiObjectReference::SpecificComponent)
				? ResolvedComponent.Get() : nullptr;
		auto PointWorldLoc = [&](int32 PointIdx)
		{
			return MovementPoints[PointIdx].ResolveWorldLocation(Actor, PointAnchor);
		};
		// Commit a point as the resolved goal (shared by every exit below).
		auto SelectPoint = [&](int32 PointIdx, const FVector& PointLoc)
		{
			OutGoalLocation        = PointLoc;
			OutMovementPointIndex  = PointIdx;
			bOutMoveToLocation     = true;
			OptionalPositionVector = PointLoc;
			bOutSuccess            = true;
		};
		// "Close enough" for both arrival and reachability classification —
		// the 150 floor absorbs navmesh projection offsets, and keeping the
		// two tests identical stops "move until already-there" loops from
		// re-issuing moves forever on points the navmesh can't land exactly on.
		const float PointTolerance = FMath::Max(AcceptanceRadius, 150.0f);

		if (!SourceActor)
		{
			// No source — nothing to measure travel from. Deterministically the
			// first enabled point so snapshot/debug-gate consumers get a stable
			// representative location.
			SelectPoint(EnabledPoints[0], PointWorldLoc(EnabledPoints[0]));
			return;
		}

		const FVector SourceLoc = SourceActor->GetActorLocation();
		FVector CbOrigin, CbExtent;
		SourceActor->GetActorBounds(/*bOnlyCollidingComponents*/ true, CbOrigin, CbExtent);
		const float SourceHeight = CbExtent.Z * 2.0f;

		// Arrival pre-pass — cheap distance checks before any nav query: if the
		// source already stands within acceptance of ANY enabled point, that
		// point is the goal and no pathfinding is needed. Nearest arrived point
		// wins so the reported goal matches where the actor actually is.
		int32 ArrivedIdx = INDEX_NONE;
		FVector ArrivedLoc = FVector::ZeroVector;
		float ArrivedDist2D = TNumericLimits<float>::Max();
		for (int32 PointIdx : EnabledPoints)
		{
			const FVector PointLoc = PointWorldLoc(PointIdx);
			const float Dist2D = FVector::Dist2D(SourceLoc, PointLoc);
			if (Dist2D <= PointTolerance
				&& WithinVerticalSpan(SourceLoc.Z, PointLoc.Z, PointLoc.Z, SourceHeight)
				&& Dist2D < ArrivedDist2D)
			{
				ArrivedIdx    = PointIdx;
				ArrivedLoc    = PointLoc;
				ArrivedDist2D = Dist2D;
			}
		}
		if (ArrivedIdx != INDEX_NONE)
		{
			SelectPoint(ArrivedIdx, ArrivedLoc);
			bOutAlreadyThere = true;
			bOutReachable    = true;
			return;
		}

		// Nav selection — one path query per enabled point against a context
		// initialised ONCE (nav system, agent props, projected start). Winner:
		// reachable point with the shortest path; exact ties keep the lower
		// array index (strict <, iteration order).
		FNavQueryContext Ctx;
		const bool bNavReady = Ctx.Init(SourceActor);

		int32   BestIdx = INDEX_NONE;
		float   BestLength = TNumericLimits<float>::Max();
		FVector BestLoc = FVector::ZeroVector, BestEnd = FVector::ZeroVector;
		TArray<FVector> BestPath;

		for (int32 PointIdx : EnabledPoints)
		{
			const FVector PointLoc = PointWorldLoc(PointIdx);

			FVector FinalPoint = FVector::ZeroVector;
			TArray<FVector> PathPoints;
			float PathLength = 0.0f;
			if (!bNavReady || !RunNavPathQuery(
					SourceActor, Ctx, PointLoc, PointLoc.Z,
					FinalPoint, PathPoints, PathLength))
			{
				continue;
			}

			// Reachable when the path's end lands within the point tolerance and
			// the source's vertical span — point + radius, not object bounds:
			// authored points replace the old bbox heuristics.
			const bool bPointReachable =
				FVector::Dist2D(FinalPoint, PointLoc) <= PointTolerance
				&& WithinVerticalSpan(FinalPoint.Z, PointLoc.Z, PointLoc.Z, Ctx.SourceHeight);
			if (bPointReachable && PathLength < BestLength)
			{
				BestIdx = PointIdx;
				BestLength = PathLength;
				BestLoc = PointLoc;
				BestEnd = FinalPoint;
				BestPath = MoveTemp(PathPoints);
			}
		}

		if (BestIdx != INDEX_NONE)
		{
			SelectPoint(BestIdx, BestLoc);
			bOutReachable         = true;
			OutPathEndPoint       = BestEnd;
			OutPathPoints         = MoveTemp(BestPath);
			OutGoalTravelDistance = BestLength;
			return;
		}

		// Every point unreachable. Default: the object IS unreachable — report
		// it with the first enabled point as the goal so consumers still get a
		// sensible location. Only the explicit opt-in re-runs the object fallback.
		if (!bFallbackToObjectWhenPointsUnreachable)
		{
			SelectPoint(EnabledPoints[0], PointWorldLoc(EnabledPoints[0]));
			return;
		}
		// Fall through to the object fallback below.
	}

	// ── Object fallback position resolution ─────────────────────────────
	// Actor mode — the actor's origin (AIMoveTo uses the actor pin in this mode).
	if (ObjectReference == EConvaiObjectReference::WholeActor)
	{
		const FVector ResolvedLoc = Actor->GetActorLocation();
		OptionalPositionVector = ResolvedLoc;
		OutGoalLocation        = ResolvedLoc;
	}
	else
	{
		// Vector mode — resolve a sub-component if Component Name is set, then pick
		// socket / pivot.
		// (ResolveComponent was called at the top of this function — just read it.)
		USceneComponent* C = ResolvedComponent.Get();
		OutGoalComponent   = C;

		FVector Base;
		if (C)
		{
			if (!SocketOrBoneName.IsNone() && C->DoesSocketExist(SocketOrBoneName))
			{
				Base = C->GetSocketTransform(SocketOrBoneName, RTS_World).GetLocation();
			}
			else
			{
				if (!SocketOrBoneName.IsNone())
				{
					CONVAI_LOG(ConvaiDefinitionsLog, Warning,
						TEXT("FConvaiObjectEntry '%s': socket/bone '%s' not found on %s.%s — using component location."),
						*Name, *SocketOrBoneName.ToString(), *Actor->GetName(), *C->GetName());
				}
				Base = C->GetComponentLocation();
			}
		}
		else
		{
			// No Component Name (or no match) — use the actor's origin as the base.
			Base = Actor->GetActorLocation();
		}

		OptionalPositionVector = Base;
		OutGoalLocation        = Base;
	}

	bOutSuccess = true;

	// ── Source-relative outputs ─────────────────────────────────────────
	// Skipped entirely when SourceActor is null — the source-relative
	// outputs stay at their zero-init defaults so BP graphs that don't pass
	// a source actor see "not computed" rather than a false negative.
	if (SourceActor)
	{
		// Pick the bbox scope that matches the resolved goal. In Vector mode
		// with a component filter, the agent is targeting a specific sub-
		// component — so reachability + arrival should be measured against
		// that component's bounds, not the entire actor's. Otherwise (Actor
		// mode, or Vector mode with no component match) fall back to the
		// actor's colliding bounds.
		//
		// bOnlyCollidingComponents=true on the actor-fallback tightens the
		// bbox to the physical mesh — non-colliding scene/billboard/audio
		// components anchored at the actor's pivot would otherwise extend
		// the bbox down to floor level for an object on a high platform,
		// making the reachability + arrival Z checks falsely succeed
		// (FinalPoint at floor reads as "at the bbox bottom" because the
		// bbox bottom IS at floor).
		FVector ObjOrigin, ObjExtent;
		USceneComponent* GoalComp = ResolvedComponent.Get();
		if (GoalComp && !GoalComp->Bounds.BoxExtent.IsNearlyZero())
		{
			// Primitive (mesh) components fill their FBoxSphereBounds with
			// the rendered/colliding bounds; non-primitive scene components
			// leave it as a zero-extent point and fall back below.
			ObjOrigin = GoalComp->Bounds.Origin;
			ObjExtent = GoalComp->Bounds.BoxExtent;
		}
		else
		{
			Actor->GetActorBounds(/*bOnlyCollidingComponents*/ true, ObjOrigin, ObjExtent);
			if (ObjExtent.IsNearlyZero())
			{
				// GetActorBounds inspects primitive components only; an actor with
				// none returns (0,0,0) origin regardless of its world transform.
				// Anchor to the actor's location so an empty marker still reads as
				// "at the marker," not "at world origin."
				ObjOrigin = Actor->GetActorLocation();
			}
		}

		// Arrival check first — cheap (just transforms + bounds), and lets
		// designers gate AI Move To on "do I even need to move?" without
		// paying for the nav query.
		// Horizontal: distance to the nearest point on the object's footprint —
		// the actor stops AT the object's edge, so measure to the edge, not the
		// centre. Tolerance is max(AcceptanceRadius × 2, 150 uu): the ×2 widens
		// past the AIMoveTo trigger, 150 floors tight values.
		// Vertical: |Z delta to nearest face of object's bbox| within Source
		// Actor's own height. Mirrors the reachability vertical tolerance so
		// a Source Actor "at" a tall object reads as arrived even when the
		// goal pivot is up at the centre.
		const FVector SourceLoc = SourceActor->GetActorLocation();

		const float FootprintDist =
			HorizontalDistanceToFootprint(SourceLoc, ObjOrigin, ObjExtent);
		const bool bHorizArrived =
			FootprintDist <= FMath::Max(AcceptanceRadius * 2.0f, 150.0f);

		FVector CbOrigin, CbExtent;
		SourceActor->GetActorBounds(/*bOnlyCollidingComponents*/ true, CbOrigin, CbExtent);
		const float SourceHeight = CbExtent.Z * 2.0f;

		bOutAlreadyThere = bHorizArrived
			&& WithinVerticalSpan(SourceLoc.Z,
				ObjOrigin.Z - ObjExtent.Z, ObjOrigin.Z + ObjExtent.Z, SourceHeight);

		bOutReachable = ComputeNavReachability(SourceActor, OutGoalLocation,
			ObjOrigin, ObjExtent, AcceptanceRadius,
			OutPathEndPoint, OutPathPoints, OutGoalTravelDistance);
	}
}

void FConvaiObjectEntry::RefreshSnapshot()
{
	// Lightweight wrapper that runs the position math without the nav query —
	// callers (param resolution, etc.) only want OptionalPositionVector +
	// ResolvedComponent up to date and would pay for nav reachability they
	// never read.
	AActor* DA = nullptr; USceneComponent* DC = nullptr;
	FVector DL = FVector::ZeroVector; float DR = 0.0f;
	bool DM = false;
	bool DS = false, DThere = false, DReach = false;
	FVector DPE = FVector::ZeroVector;
	TArray<FVector> DPP;
	float DTD = 0.0f; int32 DPI = INDEX_NONE;
	ResolveGoalLocation(/*SourceActor*/ nullptr,
		DA, DC, DL, DR, DM, DS, DThere, DReach, DPE, DPP, DTD, DPI);
}

const TMap<EEmotionIntensity, float> FConvaiEmotionState::ScoreMultipliers =
{
	{EEmotionIntensity::None, 0.0},
	{EEmotionIntensity::LessIntense, 0.25},
	{EEmotionIntensity::Basic, 0.6},
	{EEmotionIntensity::MoreIntense, 1}
};

FConvaiConnectionParams FConvaiConnectionParams::Create(convai::ConvaiClient* InClient, const FString& InCharacterID, UConvaiConnectionSessionProxy* SessionProxy)
{
	FConvaiConnectionParams Params;
	Params.Client = InClient;
	Params.CharacterID = InCharacterID;
	Params.LLMProvider = UConvaiUtils::GetLLMProvider();

	// Get interface once and reuse it
	IConvaiConnectionInterface* Interface = nullptr;
	if (SessionProxy)
	{
		if (const TScriptInterface<IConvaiConnectionInterface> InterfaceScriptInterface = SessionProxy->GetConnectionInterface(); InterfaceScriptInterface.GetObject())
		{
			Interface = InterfaceScriptInterface.GetInterface();
		}
	}
	
	// Determine connection type
	Params.ConnectionType = UConvaiUtils::GetConnectionType();
	if (UConvaiUtils::IsAlwaysAllowVisionEnabled())
	{
		Params.ConnectionType = TEXT("video");
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Always allow vision is enabled, using video connection type for character ID: %s"), *InCharacterID);
	}
	else if (Interface && Interface->IsVisionSupported())
	{
		Params.ConnectionType = TEXT("video");
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Vision is supported by proxy, using video connection type for character ID: %s"), *InCharacterID);
	}
	
	// Determine blendshape provider and format based on lip sync mode
	Params.BlendshapeProvider = TEXT("not_provided");
	Params.BlendshapeFormat = TEXT("");

	// Check if the lip sync component requires precomputed face data from the server
	// If not (e.g., OVR LipSync generates data locally from audio), skip setting blendshape provider

	if (const bool bRequiresPrecomputedFaceData = Interface ? Interface->RequiresPrecomputedFaceData() : true)
	{
		// Helper lambda to set provider and format for a specific mode
		auto SetBlendshapeParamsForMode = [&Params](const EC_LipSyncMode Mode)
		{
			switch (Mode)
			{
			case EC_LipSyncMode::VisemeBased:
				Params.BlendshapeProvider = TEXT("ovr");
				Params.BlendshapeFormat = TEXT("");
				break;
			case EC_LipSyncMode::BS_MHA:
				Params.BlendshapeProvider = TEXT("neurosync");
				Params.BlendshapeFormat = TEXT("mha");
				break;
			case EC_LipSyncMode::BS_ARKit:
				Params.BlendshapeProvider = TEXT("neurosync");
				Params.BlendshapeFormat = TEXT("arkit");
				break;
			case EC_LipSyncMode::BS_CC4_Extended:
				Params.BlendshapeProvider = TEXT("neurosync");
				Params.BlendshapeFormat = TEXT("cc4_extended");
				break;
			case EC_LipSyncMode::Off:
			default:
				Params.BlendshapeProvider = TEXT("not_provided");
				Params.BlendshapeFormat = TEXT("");
				break;
			}
		};

		switch (const EC_LipSyncMode GlobalMode = UConvaiUtils::GetLipSyncMode())
		{
		case EC_LipSyncMode::Off:
			Params.BlendshapeProvider = TEXT("not_provided");
			break;

		case EC_LipSyncMode::Auto:
			if (Interface)
			{
				SetBlendshapeParamsForMode(Interface->GetLipSyncMode());
			}
			break;

		case EC_LipSyncMode::VisemeBased:
		case EC_LipSyncMode::BS_MHA:
		case EC_LipSyncMode::BS_ARKit:
			SetBlendshapeParamsForMode(GlobalMode);
			break;
		}
	}
	
	// Set emotion provider based on blendshape provider
	Params.EmotionProvider = UConvaiUtils::GetEmotionsProvider();

	// Get End User ID, Metadata, and optional action_config from interface
	if (Interface)
	{
		Params.EndUserID = Interface->GetEndUserID();
		Params.EndUserMetadata = Interface->GetEndUserMetadata();
		Params.ActionConfigJson = Interface->GetActionConfigJson();
	}
	
	// If End User ID is not provided, use device unique identifier as fallback
	if (Params.EndUserID.IsEmpty())
	{
		Params.EndUserID = UConvaiUtils::GetDeviceUniqueIdentifier();
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("End User ID not provided, using Device ID: %s"), *Params.EndUserID);
	}
	else
	{
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Using End User ID: %s"), *Params.EndUserID);
	}
	
	if (!Params.EndUserMetadata.IsEmpty())
	{
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Using End User Metadata: %s"), *Params.EndUserMetadata);
	}

	Params.ChunkSize = UConvaiUtils::GetChunkSize();
	Params.OutputFPS = UConvaiUtils::GetOutputFPS();
	Params.FramesBufferDuration = UConvaiUtils::GetFramesBufferDuration();

	// VAD: project-settings struct is the single source of truth. When bUseServerDefault is
	// true (the default), VAD fields stay at -1.0f and the subsystem omits the vad_params
	// pointer from the /connect request, so the server applies its own defaults.
	if (const UConvaiSettings* ConvaiSettings = Convai::Get().GetConvaiSettings())
	{
		const FConvaiVADSettings& VAD = ConvaiSettings->VADSettings;
		if (!VAD.bUseServerDefault)
		{
			Params.VADConfidence = VAD.Confidence;
			Params.VADStartSecs  = VAD.StartSecs;
			Params.VADStopSecs   = VAD.StopSecs;
			Params.VADMinVolume  = VAD.MinVolume;
		}
	}

	return Params;
}
