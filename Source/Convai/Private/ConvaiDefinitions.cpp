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
#include "AI/Navigation/NavAgentInterface.h"
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

// ── FConvaiObjectEntry component resolver ─────────────────────────────
bool FConvaiObjectEntry::HasComponentFilters() const
{
	return MoveTargetMode == EConvaiMoveTarget::Vector
		&& !ComponentName.IsEmpty();
}

USceneComponent* FConvaiObjectEntry::ResolveComponent(bool bForceRefresh)
{
	AActor* Actor = Ref.Get();
	if (!Actor || !HasComponentFilters())
	{
		ResolvedComponent = nullptr;
		return nullptr;
	}

	if (!bForceRefresh)
	{
		if (USceneComponent* Cached = ResolvedComponent.Get())
		{
			if (Cached->GetOwner() == Actor
				&& Cached->GetName().Contains(ComponentName, ESearchCase::IgnoreCase))
			{
				return Cached;
			}
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
	// "On the footprint" epsilon (uu) for the Step Onto Bounds reachability test.
	// Step-onto reachability isn't a distance threshold — it's a containment
	// check: the path's final point is either ON the object's footprint or it
	// isn't. This epsilon only absorbs float noise / minor navmesh quantisation
	// so a point sitting essentially on a footprint edge still counts as "on it".
	constexpr float FootprintContainmentEpsilon = 1.0f;

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

	// Reachability query lifted out of UConvaiObjectComponent so any consumer of
	// FConvaiObjectEntry can ask "can SourceActor walk to this entry's bounds?"
	// without re-implementing the navmesh dance. Mirrors AAIController::
	// BuildPathfindingQuery: pulls nav-agent props + nav-frame location from
	// INavAgentInterface (falling back to actor pivot), projects both endpoints
	// onto the navmesh so the snap doesn't fail on above-mesh goals (objects on
	// tables / shelves) or sub-mesh starts (pawn-pivot-at-feet quirks), and
	// classifies the result against ObjBounds. Horizontal: Step Onto Bounds —
	// the final point must land ON the footprint (containment); otherwise it
	// must reach within max(AcceptanceRadius × 2, 150 uu) of the nearest face.
	// Vertical: within SourceActor's own span. Partial paths are allowed so the
	// path points always come back populated even for unreachable goals.
	bool ComputeNavReachability(
		AActor* SourceActor,
		const FVector& GoalLoc,
		const FVector& ObjOrigin,
		const FVector& ObjExtent,
		bool bStepOnto,
		float AcceptanceRadius,
		FVector& OutFinalPoint,
		TArray<FVector>& OutPathPoints)
	{
		OutFinalPoint = FVector::ZeroVector;
		OutPathPoints.Reset();

		if (!IsValid(SourceActor))
		{
			return false;
		}

		UWorld* World = SourceActor->GetWorld();
		if (!World)
		{
			return false;
		}

		UNavigationSystemV1* NavSys = UNavigationSystemV1::GetNavigationSystem(World);
		if (!NavSys)
		{
			return false;
		}

		FNavAgentProperties AgentProps;
		FVector StartLoc = SourceActor->GetActorLocation();
		if (const INavAgentInterface* NavAgent = Cast<INavAgentInterface>(SourceActor))
		{
			AgentProps = NavAgent->GetNavAgentPropertiesRef();
			StartLoc   = NavAgent->GetNavAgentLocation();
		}

		// Project the start onto the navmesh so a pawn-pivot-at-feet setup (where
		// APawn::GetNavAgentLocation subtracts half-height regardless) still gets
		// a valid start tile. Large deltas log Verbose so off-nav agents stay
		// debuggable.
		{
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
		}

		const ANavigationData* NavData = NavSys->GetNavDataForProps(AgentProps, StartLoc);
		if (!NavData)
		{
			CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
				TEXT("Reachability: no NavData for agent at %s on %s — unreachable."),
				*StartLoc.ToCompactString(), *SourceActor->GetName());
			return false;
		}

		// Seed projection at the resolved GOAL's X/Y (not the actor's bbox
		// centre) so component-targeted goals route to the component, not to
		// some midpoint between the component and the actor pivot. Z stays
		// at the bbox bottom so a floor-standing object's seed sits on or
		// just above the navmesh — projection then snaps where the agent
		// would actually walk.
		const FVector GoalSeed(GoalLoc.X, GoalLoc.Y, ObjOrigin.Z - ObjExtent.Z);
		FNavLocation ProjectedGoal;
		FVector QueryEndLoc = GoalLoc;
		const bool bGoalProjected = NavSys->ProjectPointToNavigation(
			GoalSeed, ProjectedGoal,
			FVector(200.0f, 200.0f, 1000.0f), &AgentProps);
		if (bGoalProjected)
		{
			QueryEndLoc = ProjectedGoal.Location;
		}

		FPathFindingQuery Query(SourceActor, *NavData, StartLoc, QueryEndLoc);
		Query.SetAllowPartialPaths(true);

		const FPathFindingResult Result = NavSys->FindPathSync(Query);
		if (!Result.IsSuccessful() || !Result.Path.IsValid())
		{
			CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
				TEXT("Reachability: FindPathSync failed (Start=%s, Goal=%s, ProjectedEnd=%s, GoalProjected=%d) — unreachable."),
				*StartLoc.ToCompactString(), *GoalLoc.ToCompactString(),
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
		OutFinalPoint = Points.Last().Location;

		// Horizontal classification:
		//  - Step Onto Bounds: containment, not distance. The path's final point
		//    must land ON the object's footprint — if the navmesh can't put the
		//    endpoint on the object, the object can't be stepped onto. No real
		//    tolerance (being on it IS the goal); the epsilon only absorbs float
		//    noise / navmesh quantisation.
		//  - Otherwise: the final point must reach within max(AcceptanceRadius ×
		//    2, 150 uu) of the nearest footprint face — the ×2 widens past the
		//    AIMoveTo trigger, 150 floors tightly-tuned values. (At the default
		//    AcceptanceRadius of 150 this is 300 — the old hardcoded value.)
		const float HorizontalDist =
			HorizontalDistanceToFootprint(OutFinalPoint, ObjOrigin, ObjExtent);
		const float HorizTol = bStepOnto
			? FootprintContainmentEpsilon
			: FMath::Max(AcceptanceRadius * 2.0f, 150.0f);
		if (HorizontalDist > HorizTol)
		{
			CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
				TEXT("Reachability: path ends %.1f uu from bbox footprint (tol %.1f, StepOnto=%d) — unreachable."),
				HorizontalDist, HorizTol, bStepOnto ? 1 : 0);
			return false;
		}

		// Vertical tolerance: distance from final-point Z to the NEAREST bbox face
		// along Z (not the centre — tall objects would otherwise false-negative).
		// Compare against SourceActor's own height so it "reaches" anything within
		// its vertical span.
		//
		// IMPORTANT: source bounds use bOnlyCollidingComponents=true; the caller
		// passes ObjOrigin/ObjExtent computed the same way. Otherwise non-
		// colliding scene/billboard/audio components anchored at an actor's
		// pivot can drag the bbox bottom down to floor level for an object
		// that's actually on a high platform, making every floor-level
		// FinalPoint falsely register as "at the nearest face".
		FVector CbOrigin, CbExtent;
		SourceActor->GetActorBounds(/*bOnlyCollidingComponents*/ true, CbOrigin, CbExtent);
		const float SourceHeight = CbExtent.Z * 2.0f;
		const float NearestObjZ  = FMath::Clamp(OutFinalPoint.Z,
			ObjOrigin.Z - ObjExtent.Z, ObjOrigin.Z + ObjExtent.Z);
		const float ZDelta       = FMath::Abs(OutFinalPoint.Z - NearestObjZ);

		const bool bReachable = ZDelta <= FMath::Max(SourceHeight, 150.0f);
		CONVAI_LOG(ConvaiDefinitionsLog, Verbose,
			TEXT("Reachability: bbox Z=[%.1f, %.1f] FinalPoint.Z=%.1f NearestFace.Z=%.1f ZDelta=%.1f SourceHeight=%.1f => %s"),
			ObjOrigin.Z - ObjExtent.Z, ObjOrigin.Z + ObjExtent.Z,
			OutFinalPoint.Z, NearestObjZ, ZDelta, SourceHeight,
			bReachable ? TEXT("REACHABLE") : TEXT("unreachable"));
		return bReachable;
	}
} // namespace

void FConvaiObjectEntry::ResolveGoalLocation(
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
	TArray<FVector>& OutPathPoints)
{
	// Defaults so every output is well-defined regardless of branch taken.
	OutGoalActor        = Ref.Get();
	OutGoalComponent    = nullptr;
	OutGoalLocation     = OptionalPositionVector;
	OutAcceptanceRadius = AcceptanceRadius;
	OutMode             = MoveTargetMode;
	bOutSuccess         = false;
	bOutAlreadyThere    = false;
	bOutReachable       = false;
	OutPathEndPoint     = FVector::ZeroVector;
	OutPathPoints.Reset();

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
	ResolveComponent(bForceRefresh);

	// ── Position resolution ─────────────────────────────────────────────
	// Actor mode — normally just the actor's origin (AIMoveTo uses the actor pin).
	// Exception: Step Onto Bounds promotes Actor mode into a position-based goal,
	// so the AI walks ONTO the actor (top of its bounds) instead of stopping at
	// its edge. We flip OutMode to Vector so the BP graph picks the location pin.
	if (MoveTargetMode == EConvaiMoveTarget::Actor)
	{
		FVector ResolvedLoc = Actor->GetActorLocation();
		if (bStepOntoBounds)
		{
			FVector AOrigin, AExtent;
			// Colliding components only — keep the bbox tied to the physical mesh
			// so non-colliding billboard/audio/debug-arrow components don't drag
			// the top-of-bounds Z below the visible mesh top.
			Actor->GetActorBounds(/*bOnlyCollidingComponents*/ true, AOrigin, AExtent);
			// Degenerate-bounds guard: with no colliding primitives, GetActorBounds
			// returns (0,0,0)/(0,0,0) and the two-arg FBox ctor below would still
			// produce an IsValid=true box at world origin — warping the goal there.
			// Leave ResolvedLoc at the actor's location in that case.
			if (!AExtent.IsNearlyZero())
			{
				const FBox WorldBounds(AOrigin - AExtent, AOrigin + AExtent);
				if (WorldBounds.IsValid)
				{
					const FVector Center = WorldBounds.GetCenter();
					ResolvedLoc = FVector(Center.X, Center.Y, WorldBounds.Max.Z);
					OutMode = EConvaiMoveTarget::Vector;
				}
			}
		}
		OptionalPositionVector = ResolvedLoc;
		OutGoalLocation        = ResolvedLoc;
	}
	else
	{
		// Vector mode — resolve a sub-component if Component Name is set, then pick
		// socket / pivot / top-of-bounds.
		// (ResolveComponent was called at the top of this function — just read it.)
		USceneComponent* C = ResolvedComponent.Get();
		OutGoalComponent   = C;

		FVector Base;
		if (C)
		{
			// Step Onto Bounds overrides the socket selection — bounds projection
			// operates on the whole component volume, not a single socket point.
			if (!bStepOntoBounds && !SocketOrBoneName.IsNone() && C->DoesSocketExist(SocketOrBoneName))
			{
				Base = C->GetSocketTransform(SocketOrBoneName, RTS_World).GetLocation();
			}
			else
			{
				if (!bStepOntoBounds && !SocketOrBoneName.IsNone())
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

		if (bStepOntoBounds)
		{
			FBox WorldBounds(ForceInit);
			if (C)
			{
				WorldBounds = C->Bounds.GetBox();
			}
			else
			{
				FVector AOrigin, AExtent;
				// Mirror the Actor-mode branch and the reachability / arrival checks
				// (lines 501, 673): colliding components only, so audio / billboard /
				// debug-arrow components don't drag the bbox off the physical mesh.
				Actor->GetActorBounds(/*bOnlyCollidingComponents*/ true, AOrigin, AExtent);
				// Degenerate-bounds guard (same hazard as Actor mode): if no colliding
				// primitives, leave WorldBounds at its ForceInit invalid state so the
				// IsValid check below skips and Base stays at the actor location.
				if (!AExtent.IsNearlyZero())
				{
					WorldBounds = FBox(AOrigin - AExtent, AOrigin + AExtent);
				}
			}

			if (WorldBounds.IsValid)
			{
				const FVector Center = WorldBounds.GetCenter();
				Base = FVector(Center.X, Center.Y, WorldBounds.Max.Z);
			}
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
		// Horizontal — measured with the same geometry as the reachability test:
		//  - Step Onto Bounds ON: 2D distance to the object's FOOTPRINT (0 when
		//    the actor stands over it). Measuring to the top-face CENTRE instead
		//    false-negatives wide objects: the actor stops at the edge, half a
		//    width from the centre, so the centre never falls within the raw
		//    AcceptanceRadius. Footprint distance matches the reachability half
		//    (so arrival follows reachability), with the raw AcceptanceRadius
		//    (no x2, no 150 floor) preserving the same designer-controlled
		//    step-onto tightness instead of widening it back out.
		//  - Step Onto Bounds OFF: distance to the nearest point on the object's
		//    footprint — the actor stops AT the object's edge, so measure to the
		//    edge, not the centre. Tolerance is max(AcceptanceRadius × 2, 150 uu):
		//    the ×2 widens past the AIMoveTo trigger, 150 floors tight values.
		// Vertical: |Z delta to nearest face of object's bbox| within Source
		// Actor's own height. Mirrors the reachability vertical tolerance so
		// a Source Actor "at" a tall object reads as arrived even when the
		// goal pivot is up at the centre. Left unchanged for Step Onto Bounds:
		// the actor pivot sits ~half-height above the surface it stands on, so
		// a tight Z test would false-negative an actor genuinely on the plate.
		const FVector SourceLoc = SourceActor->GetActorLocation();

		bool bHorizArrived;
		if (bStepOntoBounds)
		{
			const float FootprintDist =
				HorizontalDistanceToFootprint(SourceLoc, ObjOrigin, ObjExtent);
			bHorizArrived = FootprintDist <= AcceptanceRadius;
		}
		else
		{
			const float FootprintDist =
				HorizontalDistanceToFootprint(SourceLoc, ObjOrigin, ObjExtent);
			bHorizArrived = FootprintDist <= FMath::Max(AcceptanceRadius * 2.0f, 150.0f);
		}

		FVector CbOrigin, CbExtent;
		SourceActor->GetActorBounds(/*bOnlyCollidingComponents*/ true, CbOrigin, CbExtent);
		const float SourceHeight = CbExtent.Z * 2.0f;
		const float NearestObjZ  = FMath::Clamp(SourceLoc.Z,
			ObjOrigin.Z - ObjExtent.Z, ObjOrigin.Z + ObjExtent.Z);
		const float ZDelta       = FMath::Abs(SourceLoc.Z - NearestObjZ);

		bOutAlreadyThere = bHorizArrived
			&& (ZDelta <= FMath::Max(SourceHeight, 150.0f));

		bOutReachable = ComputeNavReachability(SourceActor, OutGoalLocation,
			ObjOrigin, ObjExtent, bStepOntoBounds, AcceptanceRadius,
			OutPathEndPoint, OutPathPoints);
	}
}

void FConvaiObjectEntry::RefreshSnapshot(bool bForceRefresh)
{
	// Lightweight wrapper that runs the position math without the nav query —
	// callers (param resolution, etc.) only want OptionalPositionVector +
	// ResolvedComponent up to date and would pay for nav reachability they
	// never read.
	AActor* DA = nullptr; USceneComponent* DC = nullptr;
	FVector DL = FVector::ZeroVector; float DR = 0.0f;
	EConvaiMoveTarget DM = EConvaiMoveTarget::Actor;
	bool DS = false, DThere = false, DReach = false;
	FVector DPE = FVector::ZeroVector;
	TArray<FVector> DPP;
	ResolveGoalLocation(/*SourceActor*/ nullptr, bForceRefresh,
		DA, DC, DL, DR, DM, DS, DThere, DReach, DPE, DPP);
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
