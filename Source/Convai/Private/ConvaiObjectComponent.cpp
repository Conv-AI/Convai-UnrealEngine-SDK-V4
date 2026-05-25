// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiObjectComponent.h"

#include "ConvaiSubsystem.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiUtils.h"
#include "Utility/ConvaiPropertyReflection.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/LineBatchComponent.h"

DEFINE_LOG_CATEGORY(ConvaiObjectComponentLog);

UConvaiObjectComponent::UConvaiObjectComponent()
{
	// Change detection runs on the subsystem's shared clock, not on this component's tick.
	PrimaryComponentTick.bCanEverTick = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

FString UConvaiObjectComponent::BuildStateKey(const FName& PropertyPath) const
{
	return FString::Printf(TEXT("%s.%s"), *ObjectEntry.Name, *PropertyPath.ToString());
}

void UConvaiObjectComponent::RebuildCache()
{
	Cache.Reset();
	Cache.SetNum(TrackedProperties.Num());

	AActor* Owner = GetOwner();

	for (int32 i = 0; i < TrackedProperties.Num(); ++i)
	{
		const FConvaiTrackedProperty& P = TrackedProperties[i];
		Cache[i].PropertyPath = P.PropertyPath;
		Cache[i].LastFormattedValue.Reset();
		Cache[i].bResolved = false;

		if (!Owner || P.PropertyPath.IsNone())
		{
			continue;
		}

		FConvaiResolvedProperty Resolved;
		if (!ConvaiPropertyReflection::ResolvePath(Owner, P.PropertyPath, Resolved))
		{
			CONVAI_LOG(ConvaiObjectComponentLog, Warning,
				TEXT("ConvaiObjectComponent on %s: tracked property '%s' did not resolve on owner."),
				*Owner->GetName(), *P.PropertyPath.ToString());
			continue;
		}
		if (!Resolved.bSupported)
		{
			CONVAI_LOG(ConvaiObjectComponentLog, Warning,
				TEXT("ConvaiObjectComponent on %s: tracked property '%s' is an unsupported type (object/delegate/etc.). Skipped."),
				*Owner->GetName(), *P.PropertyPath.ToString());
			continue;
		}

		Cache[i].LastFormattedValue = ConvaiPropertyReflection::FormatValue(Resolved);
		Cache[i].bResolved = true;
	}
}

FString UConvaiObjectComponent::ComposeDescriptionForLLM() const
{
	FString Out = ObjectEntry.Description;

	auto AppendRow = [&Out](const FString& Path, const FString& Desc,
		const TArray<FConvaiTrackedPropertyStateValueDesc>* Values)
	{
		Out += FString::Printf(TEXT("\n- %s"), *Path);
		if (!Desc.IsEmpty())
		{
			Out += FString::Printf(TEXT(": %s"), *Desc);
		}
		if (Values && Values->Num() > 0)
		{
			Out += TEXT("\n    Possible values:");
			for (const FConvaiTrackedPropertyStateValueDesc& V : *Values)
			{
				Out += FString::Printf(TEXT("\n      %s: %s"), *V.Value, *V.Description);
			}
		}
	};

	// Decide what we'll print before emitting the "Properties:" header so we
	// don't trail it with zero rows. PropertyPath.IsNone() entries are
	// freshly-added array slots the designer hasn't bound yet — they
	// stringify to "None" and would otherwise leak as `- None` rows.
	//
	// The synthesised Proximity state is deliberately NOT listed here.
	// The LLM already learns about it via the "<ObjectName>.Proximity" state
	// key flowing in through SetContextState, and the band phrases ("close
	// by, in front", "no walking path, behind", etc.) are self-describing —
	// a bare "- Proximity" row would just repeat the key. A designer-authored
	// TrackedProperty named "Proximity" still gets listed by the loop below.
	int32 PrintableTracked = 0;
	for (const FConvaiTrackedProperty& P : TrackedProperties)
	{
		if (P.PropertyPath.IsNone())
		{
			continue;
		}
		++PrintableTracked;
	}

	if (PrintableTracked == 0)
	{
		return Out;
	}

	if (!Out.IsEmpty())
	{
		Out += TEXT("\n\n");
	}
	Out += TEXT("Properties:");

	for (const FConvaiTrackedProperty& P : TrackedProperties)
	{
		if (P.PropertyPath.IsNone())
		{
			continue;
		}
		AppendRow(P.PropertyPath.ToString(), P.Description, &P.StateValueDescriptions);
	}

	return Out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blueprint mutators
// ─────────────────────────────────────────────────────────────────────────────

bool UConvaiObjectComponent::AddTrackedProperty(const FConvaiTrackedProperty& InProperty)
{
	if (InProperty.PropertyPath.IsNone())
	{
		CONVAI_LOG(ConvaiObjectComponentLog, Warning, TEXT("AddTrackedProperty: PropertyPath is None"));
		return false;
	}

	for (const FConvaiTrackedProperty& Existing : TrackedProperties)
	{
		if (Existing.PropertyPath == InProperty.PropertyPath)
		{
			CONVAI_LOG(ConvaiObjectComponentLog, Warning,
				TEXT("AddTrackedProperty: '%s' already tracked. Use UpdateTrackedProperty to modify."),
				*InProperty.PropertyPath.ToString());
			return false;
		}
	}

	AActor* Owner = GetOwner();
	if (Owner)
	{
		FConvaiResolvedProperty Resolved;
		if (!ConvaiPropertyReflection::ResolvePath(Owner, InProperty.PropertyPath, Resolved) || !Resolved.bSupported)
		{
			CONVAI_LOG(ConvaiObjectComponentLog, Warning,
				TEXT("AddTrackedProperty: '%s' did not resolve or is unsupported."),
				*InProperty.PropertyPath.ToString());
			return false;
		}
	}

	TrackedProperties.Add(InProperty);
	RebuildCache();

	if (HasValidObjectName())
	{
		const int32 Idx = TrackedProperties.Num() - 1;
		if (Cache.IsValidIndex(Idx) && Cache[Idx].bResolved)
		{
			BroadcastToAllChatbots(InProperty, BuildStateKey(InProperty.PropertyPath), Cache[Idx].LastFormattedValue, EC_RunLLMOption::Never);
		}
	}
	else
	{
		CONVAI_LOG(ConvaiObjectComponentLog, Warning,
			TEXT("AddTrackedProperty: ObjectEntry.Name is empty on %s; the new property is staged but won't broadcast until it is set."),
			Owner ? *Owner->GetName() : TEXT("<no owner>"));
	}

	return true;
}

bool UConvaiObjectComponent::RemoveTrackedProperty(FName PropertyPath)
{
	const int32 Removed = TrackedProperties.RemoveAll([&](const FConvaiTrackedProperty& P){ return P.PropertyPath == PropertyPath; });
	if (Removed > 0)
	{
		RebuildCache();
		return true;
	}
	return false;
}

bool UConvaiObjectComponent::UpdateTrackedProperty(FName PropertyPath, const FConvaiTrackedProperty& NewSettings)
{
	for (FConvaiTrackedProperty& Existing : TrackedProperties)
	{
		if (Existing.PropertyPath == PropertyPath)
		{
			// Preserve original path; treat update as descriptor refresh.
			Existing.Description = NewSettings.Description;
			Existing.StateValueDescriptions = NewSettings.StateValueDescriptions;
			Existing.ShouldRespond = NewSettings.ShouldRespond;
			return true;
		}
	}
	return false;
}

void UConvaiObjectComponent::GetTrackedProperties(TArray<FConvaiTrackedProperty>& OutProperties) const
{
	OutProperties = TrackedProperties;
}

// ─────────────────────────────────────────────────────────────────────────────
// Gather / evaluate
// ─────────────────────────────────────────────────────────────────────────────

void UConvaiObjectComponent::SeedInitialStateOntoChatbot(UConvaiChatbotComponent* Chatbot) const
{
	if (!IsValid(Chatbot) || !HasValidObjectName())
	{
		return;
	}

	for (int32 i = 0; i < TrackedProperties.Num(); ++i)
	{
		if (!Cache.IsValidIndex(i) || !Cache[i].bResolved)
		{
			continue;
		}
		// Initial-seeding always uses Never regardless of the prop's RunLLMOption,
		// to avoid hammering the LLM on session boot.
		Chatbot->SetContextState(
			BuildStateKey(TrackedProperties[i].PropertyPath),
			Cache[i].LastFormattedValue,
			EC_RunLLMOption::Never,
			/*bFlushImmediately*/ false);
	}
}

void UConvaiObjectComponent::GatherEligibleChatbots(TArray<UConvaiChatbotComponent*>& Out) const
{
	Out.Reset();
	if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		for (UConvaiChatbotComponent* Chatbot : Subsystem->GetAllChatbotComponents())
		{
			if (IsValid(Chatbot))
			{
				Out.AddUnique(Chatbot);
			}
		}
	}
}

void UConvaiObjectComponent::BroadcastToAllChatbots(const FConvaiTrackedProperty& Prop, const FString& StateKey, const FString& Value, EC_RunLLMOption RunLLMOption)
{
	TArray<UConvaiChatbotComponent*> Targets;
	GatherEligibleChatbots(Targets);
	for (UConvaiChatbotComponent* Chatbot : Targets)
	{
		Chatbot->SetContextState(StateKey, Value, RunLLMOption, /*bFlushImmediately*/ false);
	}
}

void UConvaiObjectComponent::EvaluateTrackedProperties()
{
	AActor* Owner = GetOwner();
	if (!IsValid(Owner) || !HasValidObjectName())
	{
		// Owner may be pending-kill mid-teardown (memory still alive but stale); bail
		// before reading reflection off a corpse.
		return;
	}

	// Make sure cache and TrackedProperties are still aligned (defensive: BP mutators may have changed shape).
	if (Cache.Num() != TrackedProperties.Num())
	{
		RebuildCache();
	}

	// Auto-proximity FIRST so a designer-authored TrackedProperty named
	// "Proximity" can override the synthesised value: the tracked-property
	// pass below stages the same "<ObjectName>.Proximity" key into the
	// chatbot's batch later in this tick, and last-write wins inside the
	// debounce window.
	if (bAutoGenerateProximityState)
	{
		EvaluateProximityForAllChatbots();
	}

	for (int32 i = 0; i < TrackedProperties.Num(); ++i)
	{
		const FConvaiTrackedProperty& P = TrackedProperties[i];
		FCachedEntry& C = Cache[i];

		if (!C.bResolved)
		{
			continue;
		}

		FConvaiResolvedProperty Resolved;
		if (!ConvaiPropertyReflection::ResolvePath(Owner, P.PropertyPath, Resolved) || !Resolved.bSupported)
		{
			// The path went bad mid-session (e.g. owner re-init); drop it from the cache silently.
			C.bResolved = false;
			continue;
		}

		const FString Current = ConvaiPropertyReflection::FormatValue(Resolved);
		if (Current == C.LastFormattedValue)
		{
			continue;
		}

		C.LastFormattedValue = Current;

		// Grace window: a property change observed inside InitialGracePeriodSec
		// of BeginPlay is almost always the game's startup cascade mutating
		// state, not a meaningful runtime change. Force Never so the LLM
		// doesn't react to the settle.
		EC_RunLLMOption Effective = P.ShouldRespond;
		if (BeginPlayTimeSec >= 0.0)
		{
			if (UWorld* World = GetWorld())
			{
				const double Elapsed = World->GetTimeSeconds() - BeginPlayTimeSec;
				if (Elapsed < InitialGracePeriodSec)
				{
					Effective = EC_RunLLMOption::Never;
				}
			}
		}

		BroadcastToAllChatbots(P, BuildStateKey(P.PropertyPath), Current, Effective);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Proximity (per-chatbot synthesised state)
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	enum class EProximityBand : uint8 { Nearby, Moderate, Far, Unreachable };

	// Phrase the relative direction in plain English. LocalDelta is the vector
	// from the chatbot to the object expressed in the chatbot's local frame
	// (X+ forward, Y+ right, Z+ up). The 0.35 threshold on the normalised
	// component means an axis only enters the phrase when it's clearly
	// dominant; a near-purely-forward target reads "in front", not
	// "in front and slightly to the right".
	FString MakeDirectionPhrase(const FVector& LocalDelta)
	{
		const float Magnitude = LocalDelta.Size();
		if (Magnitude < KINDA_SMALL_NUMBER)
		{
			return TEXT("right here");
		}

		const FVector N = LocalDelta / Magnitude;
		constexpr float Threshold = 0.35f;

		TArray<FString, TInlineAllocator<3>> Parts;
		if (FMath::Abs(N.X) > Threshold) { Parts.Add(N.X > 0 ? TEXT("in front") : TEXT("behind")); }
		if (FMath::Abs(N.Y) > Threshold) { Parts.Add(N.Y > 0 ? TEXT("to the right") : TEXT("to the left")); }
		if (FMath::Abs(N.Z) > Threshold) { Parts.Add(N.Z > 0 ? TEXT("above") : TEXT("below")); }

		if (Parts.Num() == 0) { return TEXT("nearby"); }
		if (Parts.Num() == 1) { return Parts[0]; }
		if (Parts.Num() == 2) { return FString::Printf(TEXT("%s and %s"), *Parts[0], *Parts[1]); }
		return FString::Printf(TEXT("%s, %s and %s"), *Parts[0], *Parts[1], *Parts[2]);
	}

	FString MakeProximityPhrase(EProximityBand Band, const FString& Direction)
	{
		switch (Band)
		{
			case EProximityBand::Unreachable:
				// "no walking path" names the mechanism (movement-blocked) rather than
				// leaning on "out of reach", which smaller LLMs misread as "too far to
				// grab" or "not in the scene". The description in ComposeDescriptionForLLM
				// reinforces this.
				return FString::Printf(TEXT("no walking path, %s"), *Direction);
			case EProximityBand::Nearby:
				return FString::Printf(TEXT("close by, %s"), *Direction);
			case EProximityBand::Moderate:
				return FString::Printf(TEXT("some distance away, %s"), *Direction);
			case EProximityBand::Far:
			default:
				return FString::Printf(TEXT("far away, %s"), *Direction);
		}
	}

	EProximityBand ClassifyDistance(float DistanceUU, bool bReachable)
	{
		if (!bReachable)             { return EProximityBand::Unreachable; }
		if (DistanceUU < 1000.0f)    { return EProximityBand::Nearby; }
		if (DistanceUU < 4000.0f)    { return EProximityBand::Moderate; }
		return EProximityBand::Far;
	}
} // namespace

void UConvaiObjectComponent::EvaluateProximityForAllChatbots()
{
	if (!HasValidObjectName())
	{
		return;
	}

	// Designer override: if a TrackedProperty named "Proximity" exists, the
	// designer is authoring this state themselves and our synthesised value
	// should not compete. Ordering alone isn't enough — the tracked-property
	// pass change-gates broadcasts (skips unchanged values), so a stable
	// designer value would otherwise be overwritten by us every poll tick.
	static const FName ProximityName(TEXT("Proximity"));
	for (const FConvaiTrackedProperty& Prop : TrackedProperties)
	{
		if (Prop.PropertyPath == ProximityName)
		{
			return;
		}
	}

	// Run on every subsystem poll tick (typically 0.25 s). Per-chatbot
	// stability + pathfind-skip gates inside EvaluateProximityForChatbot
	// keep the actual work bounded.

	AActor* ObjectOwner = GetOwner();
	if (!IsValid(ObjectOwner))
	{
		return;
	}

	// Hoist per-pass: resolved object location is identical for every chatbot
	// in this tick. Call with SourceActor=nullptr so the (cheap) position math
	// runs but the nav query is skipped — that part is done per-chatbot below
	// inside its pathfind-skip gate. ResolveGoalLocation respects ObjectEntry's
	// Actor/Vector move mode + step-onto-bounds offset, so the "current object
	// location" matches what AI Move To would target — symmetry the LLM can
	// reason about.
	AActor* HA = nullptr; USceneComponent* HC = nullptr;
	FVector ObjectLoc = FVector::ZeroVector; float HR = 0.0f;
	EConvaiMoveTarget HM = EConvaiMoveTarget::Actor;
	bool HS = false, HThere = false, HReach = false;
	FVector HPE = FVector::ZeroVector; TArray<FVector> HPP;
	ObjectEntry.ResolveGoalLocation(/*SourceActor*/ nullptr, /*bForceRefresh*/ false,
		HA, HC, ObjectLoc, HR, HM, HS, HThere, HReach, HPE, HPP);

	// Prune cache entries whose chatbot was destroyed since last pass.
	for (auto It = ProximityCaches.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	// Use the same fan-out helper as BroadcastToAllChatbots so every chatbot
	// the subsystem knows about gets the proximity update.
	TArray<UConvaiChatbotComponent*> Targets;
	GatherEligibleChatbots(Targets);
	bool bAnyRecomputed = false;
	for (UConvaiChatbotComponent* Chatbot : Targets)
	{
		if (EvaluateProximityForChatbot(Chatbot, ObjectLoc))
		{
			bAnyRecomputed = true;
		}
	}

	// Debug-draw refresh: only when a chatbot actually recomputed its path on
	// this pass. Otherwise the previous persistent lines stay on screen,
	// which matches the user-facing "stays until the next evaluation" promise.
	if (bDebugDrawProximityPaths && bAnyRecomputed)
	{
		RefreshProximityDebugDraw();
	}
}

bool UConvaiObjectComponent::EvaluateProximityForChatbot(UConvaiChatbotComponent* Chatbot,
	const FVector& ObjectLoc)
{
	AActor* ChatbotOwner = Chatbot->GetOwner();
	if (!IsValid(ChatbotOwner))
	{
		return false;
	}

	const FVector ChatbotLoc = ChatbotOwner->GetActorLocation();
	const FQuat   ChatbotRot = ChatbotOwner->GetActorQuat();
	// Local name distinct from this class's `Cache` (tracked-property cache)
	// to avoid MSVC C4458 (warning-as-error in UE 5.6+).
	FProximityCache& ProxCache = ProximityCaches.FindOrAdd(Chatbot);

	// ── Stability gate ──────────────────────────────────────────────
	// Skip this tick entirely when chatbot OR object moved/rotated since
	// the previous tick — snapshots taken mid-motion go stale within the
	// poll interval anyway, and the LLM doesn't need a stream of "now
	// I'm here, now here, now here" updates. Once the chatbot settles
	// we emit one fresh value.
	//
	// Safety cap: after ProximityMaxDeferrals consecutive deferred ticks
	// (default 10 × 0.25 s = 2.5 s of sustained motion) we force-emit
	// anyway, so a chatbot in non-stop motion still hears about its
	// surroundings periodically.
	constexpr float StableMoveThresholdUU    = 50.0f;
	constexpr float StableRotateThresholdDeg = 10.0f;
	constexpr int32 ProximityMaxDeferrals    = 10;

	bool bStableSinceLastTick = true;
	if (ProxCache.bHasTickSample)
	{
		const float ChatbotMove = FVector::Dist(ChatbotLoc, ProxCache.TickChatbotLoc);
		const float ObjectMove  = FVector::Dist(ObjectLoc,  ProxCache.TickObjectLoc);
		const float ChatbotRotDeg = FMath::RadiansToDegrees(
			ChatbotRot.AngularDistance(ProxCache.TickChatbotRot));
		bStableSinceLastTick =
			ChatbotMove   < StableMoveThresholdUU &&
			ObjectMove    < StableMoveThresholdUU &&
			ChatbotRotDeg < StableRotateThresholdDeg;
	}

	// Always update the tick sample, so the next tick measures motion
	// from "now" — independent of whether we proceed past this gate.
	ProxCache.TickChatbotLoc = ChatbotLoc;
	ProxCache.TickObjectLoc  = ObjectLoc;
	ProxCache.TickChatbotRot = ChatbotRot;
	ProxCache.bHasTickSample = true;

	if (!bStableSinceLastTick)
	{
		if (ProxCache.DeferralCount < ProximityMaxDeferrals)
		{
			ProxCache.DeferralCount += 1;
			return false;
		}
		// Force-emit: sustained motion can't keep the LLM in the dark forever.
	}
	ProxCache.DeferralCount = 0;

	// ── Pathfind-skip gate ──────────────────────────────────────────
	// Once stable, still skip the expensive nav pathfind when neither end
	// has moved >100 uu since the last full eval (10000 = 100 uu squared,
	// kept squared to skip the sqrt). Direction recomputes regardless
	// because it depends on chatbot rotation, which can drift slightly
	// even within the stability threshold above.
	bool bRecomputePath = !ProxCache.bInitialized;
	if (ProxCache.bInitialized)
	{
		const float ChatbotDeltaSq = FVector::DistSquared(ChatbotLoc, ProxCache.LastChatbotLocation);
		const float ObjectDeltaSq  = FVector::DistSquared(ObjectLoc,  ProxCache.LastObjectLocation);
		bRecomputePath = (ChatbotDeltaSq >= 10000.0f) || (ObjectDeltaSq >= 10000.0f);
	}

	EProximityBand Band;
	if (bRecomputePath)
	{
		// Single call now does both the position resolution (which matches the
		// hoisted ObjectLoc — same struct, same inputs) AND the source-relative
		// outputs (arrival check + nav query + path-point capture) used by the
		// debug-draw refresh below.
		AActor* GA = nullptr; USceneComponent* GC = nullptr;
		FVector ResolvedLoc = FVector::ZeroVector; float AR = 0.0f;
		EConvaiMoveTarget Mode = EConvaiMoveTarget::Actor;
		bool bResolved = false, bAlreadyThere = false, bReachable = false;
		FVector PathEnd = FVector::ZeroVector;
		TArray<FVector> PathPoints;
		ObjectEntry.ResolveGoalLocation(ChatbotOwner, /*bForceRefresh*/ false,
			GA, GC, ResolvedLoc, AR, Mode, bResolved,
			bAlreadyThere, bReachable, PathEnd, PathPoints);

		ProxCache.LastChatbotLocation = ChatbotLoc;
		ProxCache.LastObjectLocation  = ObjectLoc;
		ProxCache.bAlreadyThere       = bAlreadyThere;
		ProxCache.bReachable          = bReachable;
		ProxCache.LastPathPoints      = MoveTemp(PathPoints);
		const float Distance          = FVector::Dist(ObjectLoc, ChatbotLoc);
		Band                          = ClassifyDistance(Distance, ProxCache.bReachable);
		ProxCache.LastBand            = static_cast<uint8>(Band);
		ProxCache.bInitialized        = true;
	}
	else
	{
		Band = static_cast<EProximityBand>(ProxCache.LastBand);
	}

	// Direction always recomputes — it's a few dot products + a small string
	// build, and it lets a chatbot turning in place see "in front" flip to
	// "behind" without waiting on a translation > 100 uu.
	const FVector WorldDelta = ObjectLoc - ChatbotLoc;
	const FVector LocalDelta = ChatbotOwner->GetActorTransform()
		.InverseTransformVectorNoScale(WorldDelta);
	const FString Direction  = MakeDirectionPhrase(LocalDelta);
	const FString NewValue   = MakeProximityPhrase(Band, Direction);

	if (NewValue != ProxCache.LastPublishedValue)
	{
		ProxCache.LastPublishedValue = NewValue;
		const FString StateKey = FString::Printf(TEXT("%s.Proximity"), *ObjectEntry.Name);
		Chatbot->SetContextState(StateKey, NewValue, EC_RunLLMOption::Never, /*bFlushImmediately*/ false);
	}

	// Whether or not the published value changed, a recompute means the cached
	// path is fresh and the debug draw should pick it up on the next refresh.
	return bRecomputePath;
}

void UConvaiObjectComponent::RefreshProximityDebugDraw()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Lazily spin up a per-component line batcher so the world-wide persistent
	// line buffer stays untouched (and so other ConvaiObjectComponents with
	// the same toggle on don't cross-flush each other).
	if (!ProximityDebugBatcher)
	{
		ProximityDebugBatcher = NewObject<ULineBatchComponent>(this);
		ProximityDebugBatcher->RegisterComponentWithWorld(World);
	}

	// Clear THIS component's prior lines, then redraw every chatbot's cached
	// path. Skip-gated chatbots still have their LastPathPoints from the last
	// recompute, so they remain on-screen until their own next recompute.
	ProximityDebugBatcher->Flush();

	// Per-state colour palette. Ordered by precedence — bAlreadyThere wins
	// over bReachable since "you're at the goal" is the operationally
	// interesting state (no move needed) regardless of whether nav says
	// reachable. Choices:
	//   - Cyan  → already there: no AI Move To would fire; the path is a
	//             vestigial nub but the colour signals "arrived".
	//   - Green → reachable, not arrived: a valid path exists; AIMoveTo
	//             would succeed.
	//   - Red   → unreachable: no nav path; AIMoveTo would fail or partial.
	// The proximity band (Nearby/Moderate/Far) is intentionally NOT folded
	// into the colour — distance is already conveyed by the path length on
	// screen, and adding it would dilute the more actionable reachable /
	// arrived distinction.
	for (const auto& Pair : ProximityCaches)
	{
		const FProximityCache& C = Pair.Value;
		if (!Pair.Key.IsValid() || C.LastPathPoints.Num() < 2)
		{
			continue;
		}

		FLinearColor LineColor;
		if (C.bAlreadyThere)         { LineColor = FLinearColor(0.0f, 1.0f, 1.0f); }    // Cyan
		else if (C.bReachable)       { LineColor = FLinearColor::Green; }
		else                         { LineColor = FLinearColor::Red; }

		for (int32 i = 0; i + 1 < C.LastPathPoints.Num(); ++i)
		{
			ProximityDebugBatcher->DrawLine(
				C.LastPathPoints[i], C.LastPathPoints[i + 1],
				LineColor, SDPG_World, /*Thickness*/ 2.0f, /*Lifetime*/ -1.0f);
		}

		// Path-point markers — DrawPoint renders as a camera-facing quad,
		// which reads clearly even for paths that hug the floor where the
		// line itself can clip into geometry. Start (chatbot side) is
		// yellow, end (final navpoint — partial-path stop or goal) is
		// magenta; intermediate corners use the line colour so the path's
		// reachability state stays legible while the endpoints pop.
		const int32 LastIdx = C.LastPathPoints.Num() - 1;
		for (int32 i = 0; i < C.LastPathPoints.Num(); ++i)
		{
			FLinearColor PtColor;
			float        PtSize;
			if (i == 0)              { PtColor = FLinearColor(1.0f, 1.0f, 0.0f); PtSize = 16.0f; } // Yellow start
			else if (i == LastIdx)   { PtColor = FLinearColor(1.0f, 0.0f, 1.0f); PtSize = 16.0f; } // Magenta end
			else                     { PtColor = LineColor;                     PtSize =  8.0f; }
			ProximityDebugBatcher->DrawPoint(
				C.LastPathPoints[i], PtColor, PtSize, SDPG_World, /*Lifetime*/ -1.0f);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// UActorComponent lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void UConvaiObjectComponent::BeginPlay()
{
	Super::BeginPlay();

	// Auto-bind Ref to the owning Actor so the user doesn't have to set it explicitly;
	// the whole point of the component is to be ON the actor it represents. Only fill
	// when unset so users who deliberately point Ref at another actor are respected.
	if (!ObjectEntry.Ref.IsValid())
	{
		ObjectEntry.Ref = GetOwner();
	}

	RebuildCache();

	// Stamp the start of the grace window. Tracked-property changes the
	// shared poll observes within InitialGracePeriodSec are broadcast as
	// Never regardless of the prop's ShouldRespond — game-startup logic
	// often mutates state on or right after BeginPlay (door auto-locks,
	// gameplay variables initialised, etc.) and the LLM shouldn't react
	// to that initial settle. The seed itself is already Never via
	// SeedInitialStateOntoChatbot below; this gate covers the window
	// between the seed and "steady state".
	if (UWorld* World = GetWorld())
	{
		BeginPlayTimeSec = World->GetTimeSeconds();
	}

	UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this);

	if (Subsystem)
	{
		// Subsystem warns and refuses on empty ObjectEntry.Name, and auto-disambiguates
		// on collision (mutating ObjectEntry.Name here if needed).
		Subsystem->RegisterObjectComponent(this);
	}

	if (!HasValidObjectName() || !Subsystem)
	{
		return;
	}

	// Push into chatbots that already exist (e.g. we spawned mid-session).
	// Chatbots that haven't called StartSession yet will re-gather us themselves
	// via UConvaiSubsystem::GetAllObjectComponents() and produce the same state.
	for (UConvaiChatbotComponent* Chatbot : Subsystem->GetAllChatbotComponents())
	{
		if (!IsValid(Chatbot))
		{
			continue;
		}
		Chatbot->AddOrUpdateObjectFromComponent(this);
		SeedInitialStateOntoChatbot(Chatbot);
	}
}

void UConvaiObjectComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Broadcast removal of everything this object pushed onto chatbots BEFORE
	// the subsystem unregister so eligibility resolution still works. Four
	// scopes need to be cleared, because the object lives in four places:
	//   - Each chatbot's DynamicContextTracker (one entry per tracked-property
	//     state key + the synthesised Proximity key), via RemoveContextState.
	//   - Each chatbot's EnvironmentData.Objects (added by
	//     AddOrUpdateObjectFromComponent at registration), via RemoveObject.
	//   - Each chatbot's EnvironmentData.CurrentAttentionObject when it
	//     happens to point at THIS object — otherwise the chatbot's attention
	//     slot keeps referencing a destroyed entity until something else
	//     reassigns it. Cleared via SetObjectInAttention(empty) which routes
	//     through the same debounce path BP uses.
	//   - The subsystem's RegisteredObjectComponents (handled below).
	// Without these removals the LLM would still see the object in scene
	// metadata / action_config and (worse) keep "attending to" it after it
	// was destroyed.
	if (HasValidObjectName())
	{
		TArray<UConvaiChatbotComponent*> Targets;
		GatherEligibleChatbots(Targets);
		for (UConvaiChatbotComponent* Chatbot : Targets)
		{
			for (const FConvaiTrackedProperty& Prop : TrackedProperties)
			{
				if (Prop.PropertyPath.IsNone())
				{
					continue;
				}
				Chatbot->RemoveContextState(BuildStateKey(Prop.PropertyPath));
			}
			if (bAutoGenerateProximityState)
			{
				Chatbot->RemoveContextState(
					FString::Printf(TEXT("%s.Proximity"), *ObjectEntry.Name));
			}
			Chatbot->RemoveObject(ObjectEntry.Name);

			// Attention slot: clear unconditionally when it points at us, regardless
			// of source (Gaze or Explicit). Match by Name since Ref may already be
			// stale by the time the destroyed object's EndPlay runs.
			if (Chatbot->EnvironmentData.CurrentAttentionObject.Name == ObjectEntry.Name)
			{
				// Direct mirror write first: SetObjectInAttention early-returns when
				// EnvironmentData.bEnableActions is false (the network path is a
				// no-op then by design), but the local mirror still feeds the
				// action_config payload on the NEXT reconnect — so if actions get
				// re-enabled later, a stale name would resurface. Clear here
				// unconditionally, then route through SetObjectInAttention for the
				// debounce/network side-effects when actions are on.
				Chatbot->EnvironmentData.CurrentAttentionObject = FConvaiObjectEntry();
				Chatbot->SetObjectInAttention(FConvaiObjectEntry(),
					/*Text*/        TEXT(""),
					/*ShouldRespond*/ EC_RunLLMOption::Never,
					/*bFlushImmediately*/ false);
			}
		}
	}

	if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		Subsystem->UnregisterObjectComponent(this);
	}

	// Tear down the debug batcher so its persistent lines don't outlive the
	// component (and its registration doesn't keep the line batcher alive
	// after world teardown).
	if (ProximityDebugBatcher)
	{
		ProximityDebugBatcher->Flush();
		ProximityDebugBatcher->UnregisterComponent();
		ProximityDebugBatcher = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

USceneComponent* UConvaiObjectComponent::GetResolvedComponent(bool bForceRefresh)
{
	// FConvaiObjectEntry::ResolveComponent caches into ObjectEntry.ResolvedComponent on first
	// call and on bForceRefresh. Returns nullptr for whole-actor scope (no ComponentName filter).
	return ObjectEntry.ResolveComponent(bForceRefresh);
}

// ─────────────────────────────────────────────────────────────────────────────
// Gaze attention fan-out
// ─────────────────────────────────────────────────────────────────────────────

void UConvaiObjectComponent::NotifyGazeBegin(UConvaiPlayerComponent* Player)
{
	OnGazedIn.Broadcast(this, Player);
}

void UConvaiObjectComponent::NotifyGazeEnd(UConvaiPlayerComponent* Player)
{
	OnGazedOut.Broadcast(this, Player);
}

void UConvaiObjectComponent::NotifyGazeAttentionBegin(UConvaiPlayerComponent* Player,
	const FString& Text, EC_RunLLMOption ShouldRespond)
{
	if (!HasValidObjectName())
	{
		// [GAZE-DEBUG] Remove once attention flow is verified end-to-end.
		CONVAI_LOG(ConvaiObjectComponentLog, Warning,
			TEXT("[Gaze] NotifyGazeAttentionBegin called on '%s' with empty ObjectEntry.Name — fan-out skipped (chatbot needs the Name to resolve attention server-side)."),
			*GetName());
		OnAttentionGained.Broadcast(this, Player);
		return;
	}

	// Single fan-out path shared with state-tracking — every chatbot the subsystem knows about.
	TArray<UConvaiChatbotComponent*> Targets;
	GatherEligibleChatbots(Targets);

	// [GAZE-DEBUG] Verbose; bump back to Log when debugging fan-out target counts.
	CONVAI_LOG(ConvaiObjectComponentLog, Verbose,
		TEXT("[Gaze] NotifyGazeAttentionBegin name='%s' EligibleChatbots=%d"),
		*ObjectEntry.Name, Targets.Num());

	for (UConvaiChatbotComponent* Chatbot : Targets)
	{
		Chatbot->TrySetObjectInAttentionFromGaze(ObjectEntry, Text, ShouldRespond, /*bFlushImmediately=*/ false);
	}

	OnAttentionGained.Broadcast(this, Player);
}

void UConvaiObjectComponent::NotifyGazeAttentionEnd(UConvaiPlayerComponent* Player)
{
	if (!HasValidObjectName())
	{
		OnAttentionLost.Broadcast(this, Player);
		return;
	}

	TArray<UConvaiChatbotComponent*> Targets;
	GatherEligibleChatbots(Targets);
	for (UConvaiChatbotComponent* Chatbot : Targets)
	{
		Chatbot->TryClearObjectInAttentionFromGaze(ObjectEntry);
	}

	OnAttentionLost.Broadcast(this, Player);
}

#if WITH_EDITOR
void UConvaiObjectComponent::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedChainEvent)
{
	Super::PostEditChangeChainProperty(PropertyChangedChainEvent);

	// Validate any PropertyPath fields the user just touched.
	if (AActor* Owner = GetOwner())
	{
		for (FConvaiTrackedProperty& P : TrackedProperties)
		{
			if (P.PropertyPath.IsNone())
			{
				continue;
			}
			FConvaiResolvedProperty Resolved;
			const bool bOk = ConvaiPropertyReflection::ResolvePath(Owner, P.PropertyPath, Resolved) && Resolved.bSupported;
			if (!bOk)
			{
				CONVAI_LOG(ConvaiObjectComponentLog, Warning,
					TEXT("ConvaiObjectComponent on %s: tracked property '%s' is unresolvable or unsupported."),
					*Owner->GetName(), *P.PropertyPath.ToString());
			}
		}
	}

	// Clear leftover debug lines as soon as the toggle is turned off, so
	// designers don't have to wait for the next proximity poll to see the
	// editor stop drawing.
	if (!bDebugDrawProximityPaths && ProximityDebugBatcher)
	{
		ProximityDebugBatcher->Flush();
	}
}
#endif
