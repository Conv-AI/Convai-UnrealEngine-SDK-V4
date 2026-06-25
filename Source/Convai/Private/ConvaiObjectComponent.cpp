// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiObjectComponent.h"

#include "ConvaiSubsystem.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiUtils.h"
#include "Utility/ConvaiPropertyReflection.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

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

	// Proximity is synthesised centrally by UConvaiContextSubsystem's spatial pass —
	// as Context Facts that also cover other characters, the player, and inter-entity
	// relations — not per-object here. bIncludeInSpatialAwareness is still honoured:
	// the subsystem reads it to decide whether this object participates.

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
	// Unregister from the object registry FIRST so the merged-set sibling lookup below
	// sees only the SURVIVORS (this component excluded). Chatbot eligibility
	// (GatherEligibleChatbots) depends only on the chatbot registry, so unregistering
	// before the chatbot cleanup is safe.
	if (UConvaiSubsystem* UnregSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		UnregSubsystem->UnregisterObjectComponent(this);
	}

	// Now reconcile everything this object pushed onto chatbots — without this the LLM
	// would still see it in scene metadata / action_config and keep "attending to" it:
	//   - Each chatbot's DynamicContextTracker (tracked-property state keys), via RemoveContextState.
	//   - Each chatbot's EnvironmentData.Objects, via RemoveObject — UNLESS a same-named
	//     sibling survives (a merged set), in which case the logical object lives on and
	//     we rebuild its entry from a surviving member instead.
	//   - Each chatbot's EnvironmentData.CurrentAttentionObject when it points at THIS
	//     object: cleared when we were the last member, else its Ref repointed to the
	//     surviving member.
	if (HasValidObjectName())
	{
		// Merged set: find a still-registered sibling sharing our final name with a LIVE
		// owner. 'this' is already unregistered above, so it won't appear here. If a
		// sibling exists the logical object outlives us (rebuild its entry from it);
		// otherwise we were the last member (remove it). EndPlay runs sequentially on the
		// game thread, so the last surviving member is the one that actually removes it.
		UConvaiObjectComponent* SurvivingSibling = nullptr;
		if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this))
		{
			TArray<UConvaiObjectComponent*> Members;
			Subsystem->GetObjectGroupMembers(this, Members);
			for (UConvaiObjectComponent* Member : Members)
			{
				if (Member != this && IsValid(Member) && IsValid(Member->GetOwner()))
				{
					SurvivingSibling = Member;
					break;
				}
			}
		}

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

			if (SurvivingSibling)
			{
				// Logical object survives: rebuild its entry from the live sibling so its
				// nav fields / description / Ref no longer reference this destroyed member
				// (the rebuild groups the survivors — 'this' is already unregistered), and
				// repoint the attention slot's Ref if it points at this object.
				Chatbot->AddOrUpdateObjectFromComponent(SurvivingSibling);
				if (Chatbot->EnvironmentData.CurrentAttentionObject.Name == ObjectEntry.Name)
				{
					Chatbot->EnvironmentData.CurrentAttentionObject.Ref = SurvivingSibling->GetOwner();
				}
				continue;
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
					/*bAddAttentionEvent*/ false,
					/*bFlushImmediately*/ false);
			}
		}
	}

	// (Unregistration from the object registry happened at the top of EndPlay, before
	// the chatbot cleanup, so merged-set sibling resolution sees only the survivors.)

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
		Chatbot->TrySetObjectInAttentionFromGaze(ObjectEntry, Text, ShouldRespond, /*bAddAttentionEvent=*/ true, /*bFlushImmediately=*/ false);
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

}
#endif
