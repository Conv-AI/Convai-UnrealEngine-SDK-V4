// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiObjectComponent.h"

#include "ConvaiSubsystem.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiUtils.h"
#include "Utility/ConvaiPropertyReflection.h"
#include "Utility/ConvaiContextFormat.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/LineBatchComponent.h"
#include "HAL/PlatformTime.h"
#include "UObject/UnrealType.h"
// Moved from Engine to CoreUObject in later engine versions.
#if __has_include("StructUtils/UserDefinedStruct.h")
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#if WITH_EDITOR
#include "Components/ArrowComponent.h"
#endif

DEFINE_LOG_CATEGORY(ConvaiObjectComponentLog);

#if WITH_TESTS
namespace ConvaiObjectPrivate
{
	/** StateKeySuffix calls that walked a property path. */
	int32 StateKeyWalksForTests = 0;
}
#endif

namespace
{
	/** A Blueprint struct stores a member as "<Name>_<N>_<32 hex digits>". Name,
	 *  or empty when Stored has no such suffix. */
	FString StripStructMemberSuffix(const FString& Stored)
	{
		constexpr int32 GuidLength = 32;
		const int32 GuidStart = Stored.Len() - GuidLength;
		if (GuidStart < 4 || Stored[GuidStart - 1] != TEXT('_'))
		{
			return FString();
		}
		for (int32 Index = GuidStart; Index < Stored.Len(); ++Index)
		{
			if (!FChar::IsHexDigit(Stored[Index]))
			{
				return FString();
			}
		}
		int32 IdStart = GuidStart - 1;
		while (IdStart > 0 && FChar::IsDigit(Stored[IdStart - 1]))
		{
			--IdStart;
		}
		if (IdStart == GuidStart - 1 || IdStart < 2 || Stored[IdStart - 1] != TEXT('_'))
		{
			return FString();
		}
		return Stored.Left(IdStart - 1);
	}

	/** What a tracked-property key calls one path segment. A Blueprint struct
	 *  member reads as its name without the stored suffix in every build: the
	 *  engine's authored name is the editor's display name in the editor
	 *  ("MemberVar_0" for a member never renamed) and the stripped name in a
	 *  packaged build, so keys would differ between PIE and the shipped game.
	 *  Two members that strip to the same name keep their stored names. */
	FString KeySegmentName(const FProperty& Property)
	{
		FString Stored = Property.GetName();
		const UStruct* Owner = Property.GetOwnerStruct();
		if (!Owner || !Owner->IsA<UUserDefinedStruct>())
		{
			return Stored;
		}
		FString Stripped = StripStructMemberSuffix(Stored);
		if (Stripped.IsEmpty())
		{
			return Stored;
		}
		for (TFieldIterator<FProperty> It(Owner); It; ++It)
		{
			if (*It != &Property && StripStructMemberSuffix(It->GetName()).Equals(Stripped, ESearchCase::IgnoreCase))
			{
				return Stored;
			}
		}
		return Stripped;
	}
}

UConvaiObjectComponent::UConvaiObjectComponent()
{
	// Change detection runs on the subsystem's shared clock, not on this component's tick.
	PrimaryComponentTick.bCanEverTick = false;
}

void UConvaiObjectComponent::PostLoad()
{
	Super::PostLoad();

	// Canonicalize assets saved before Never implied normal delivery and no
	// immediate flush. Do not dirty the package; the healed values persist on
	// the next ordinary save.
	for (FConvaiTrackedProperty& Property : TrackedProperties)
	{
		Property.NormalizeResponseSettings();
	}
	MovementAwareness.NormalizeResponseSettings();
}

#if WITH_EDITOR

void UConvaiObjectComponent::OnRegister()
{
	Super::OnRegister();

	// Heal maps saved while the old arrow-marker system was active: those
	// markers were transient editor-only UArrowComponents, but actor
	// reconstruction could bake copies into the saved actor. Match their
	// exact fingerprint (editor-only + instance-created + the old marker
	// dimensions) and remove them — the visualizer owns all movement-point
	// drawing now, and never creates components.
	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!GIsEditor || !World || World->IsGameWorld() || !IsValid(Owner))
	{
		return;
	}

	TArray<UArrowComponent*> Arrows;
	Owner->GetComponents<UArrowComponent>(Arrows);
	for (UArrowComponent* Arrow : Arrows)
	{
		if (Arrow && Arrow->bIsEditorOnly
			&& Arrow->CreationMethod == EComponentCreationMethod::Instance
			&& Arrow->ArrowLength == 90.0f && Arrow->ArrowSize == 1.4f)
		{
			CONVAI_LOG(ConvaiObjectComponentLog, Log,
				TEXT("Removed stale movement-point arrow marker '%s' from '%s' (left over from a previous plugin version)."),
				*Arrow->GetName(), *Owner->GetName());
			Owner->Modify();
			Arrow->Modify();
			Arrow->DestroyComponent();
		}
	}
}

FTransform UConvaiObjectComponent::GetMovementPointAnchorTransform() const
{
	const AActor* Owner = GetOwner();
	if (!IsValid(Owner))
	{
		return FTransform::Identity;
	}

	// Editor-time: ObjectEntry.Ref only auto-binds at BeginPlay, so resolve the
	// component filter directly against the owner. This is what makes a point
	// anchored to a sub-component (moving platform, door leaf) follow it live
	// in the editor, wherever that component sits in the placed instance.
	if (ObjectEntry.ObjectReference == EConvaiObjectReference::SpecificComponent
		&& !ObjectEntry.ComponentName.IsEmpty())
	{
		TArray<USceneComponent*> All;
		Owner->GetComponents<USceneComponent>(All);
		for (USceneComponent* C : All)
		{
			if (C && C->GetName().Contains(ObjectEntry.ComponentName, ESearchCase::IgnoreCase))
			{
				return C->GetComponentTransform();
			}
		}
	}
	// Actor-scoped (or unresolved filter): the actor transform, exactly what
	// FConvaiMovementPoint::ResolveWorldLocation anchors to at runtime.
	return Owner->GetActorTransform();
}

#endif // WITH_EDITOR

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

FString UConvaiObjectComponent::BuildStateKey(const FConvaiTrackedProperty& Prop) const
{
	return BuildStateKey(Prop.PropertyPath, Prop.Alias);
}

FString UConvaiObjectComponent::BuildStateKey(FName PropertyPath, const FString& Alias) const
{
	return BuildStateKey(ObjectEntry.Name, PropertyPath, Alias);
}

FString UConvaiObjectComponent::BuildStateKey(const FString& ObjectName, FName PropertyPath, const FString& Alias) const
{
	return FString::Printf(TEXT("%s.%s"), *ObjectName, *StateKeySuffix(PropertyPath, Alias));
}

FString UConvaiObjectComponent::StateKeySuffix(FName PropertyPath, const FString& Alias) const
{
	if (!Alias.IsEmpty())
	{
		return Alias;
	}
#if WITH_TESTS
	++ConvaiObjectPrivate::StateKeyWalksForTests;
#endif
	// A Blueprint struct member is stored as "<Name>_<N>_<GUID>"; the AI reads the
	// name the designer gave it. The walk follows declared types, so past a hop
	// into a variable only a subclass declares, segments stay as written.
	const AActor* Owner = GetOwner();
	const UStruct* Struct = Owner ? Owner->GetClass() : nullptr;
	TArray<FString> Segments;
	for (const FName Token : ConvaiPropertyReflection::TokenizePath(PropertyPath))
	{
		const FProperty* Property = Struct ? Struct->FindPropertyByName(Token) : nullptr;
		Segments.Add(Property ? KeySegmentName(*Property) : Token.ToString());
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			Struct = StructProperty->Struct;
		}
		else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			Struct = ObjectProperty->PropertyClass;
		}
		else
		{
			Struct = nullptr;
		}
	}
	return Segments.IsEmpty() ? PropertyPath.ToString() : FString::Join(Segments, TEXT("."));
}

void UConvaiObjectComponent::RetireStateKeys(const FString& ObjectName, TArray<FString> Keys)
{
	UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this);
	if (!Subsystem || Keys.IsEmpty())
	{
		return;
	}
	for (FString& Key : Keys)
	{
		Key = ConvaiContextFormat::SanitizeKey(Key);
	}

	// Merged members share one name, so members tracking the same property write the
	// same key; the logical object keeps that state while a live member can read it.
	// Members' caches, not their arrays: those are the values a re-seed sends.
	// A member belongs to the name it carries now, the one it writes under and
	// grouping uses, not the one it registered under.
	TArray<const UConvaiObjectComponent*> Producers;
	TArray<TPair<FString, FCachedEntry*>> Unreadable;
	for (UConvaiObjectComponent* Member : Subsystem->GetAllObjectComponents())
	{
		if (Member == this || !IsValid(Member) || !IsValid(Member->GetOwner())
			|| !Member->ObjectEntry.Name.Equals(ObjectName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		for (FCachedEntry& Entry : Member->Cache)
		{
			if (Entry.PropertyPath.IsNone())
			{
				continue;
			}
			const FString Key = ConvaiContextFormat::SanitizeKey(Member->BuildStateKey(ObjectName, Entry.PropertyPath, Entry.Alias));
			if (!Entry.bResolved)
			{
				Unreadable.Emplace(Key, &Entry);
			}
			else if (Keys.Remove(Key) > 0)
			{
				Producers.AddUnique(Member);
			}
		}
	}
	// A key no member can read now goes; the first member to read it again seeds it.
	for (const TPair<FString, FCachedEntry*>& Entry : Unreadable)
	{
		if (Keys.Contains(Entry.Key))
		{
			Entry.Value->bEverResolved = false;
		}
	}

	TArray<UConvaiChatbotComponent*> Targets;
	GatherEligibleChatbots(Targets);
	for (UConvaiChatbotComponent* Chatbot : Targets)
	{
		for (const FString& Key : Keys)
		{
			Chatbot->RemoveContextState(Key);
		}
		// The chatbot may hold the departing member's value for a kept key.
		for (const UConvaiObjectComponent* Producer : Producers)
		{
			Producer->SeedInitialStateOntoChatbot(Chatbot);
		}
	}
}

FString UConvaiObjectComponent::BuildMovementStateKey() const
{
	return BuildMovementStateKey(ObjectEntry.Name);
}

FString UConvaiObjectComponent::BuildMovementStateKey(const FString& ObjectName)
{
	return FString::Printf(TEXT("%s.Movement"), *ObjectName);
}

bool UConvaiObjectComponent::HasMovementStateKeyCollision() const
{
	if (!HasValidObjectName())
	{
		return false;
	}
	const FString MovementKey = ConvaiContextFormat::SanitizeKey(BuildMovementStateKey());
	// Every poll asks, so a current cache stands in for StateKeySuffix's reflection walk.
	// A path keys as one segment per token, so a longer path can't match; a one-token path
	// keys as its token, since a property the walk finds is named that, ignoring case.
	if (!IsCacheStale())
	{
		for (const FCachedEntry& Entry : Cache)
		{
			FString Suffix = Entry.Alias;
			if (Suffix.IsEmpty() && Entry.PathTokens.Num() == 1)
			{
				Suffix = Entry.PathTokens[0].ToString();
			}
			if (!Entry.PropertyPath.IsNone() && !Suffix.IsEmpty()
				&& ConvaiContextFormat::SanitizeKey(ObjectEntry.Name + TEXT(".") + Suffix) == MovementKey)
			{
				return true;
			}
		}
		return false;
	}
	for (const FConvaiTrackedProperty& Prop : TrackedProperties)
	{
		if (!Prop.PropertyPath.IsNone()
			&& ConvaiContextFormat::SanitizeKey(BuildStateKey(Prop)) == MovementKey)
		{
			return true;
		}
	}
	return false;
}

void UConvaiObjectComponent::AppendTrackedPropertyContextKeys(TArray<FString>& OutKeys) const
{
	// Unbound array slots (no path picked yet) and unnamed objects would
	// synthesize keys like "FrontDoor.None" that arm but can never fire.
	if (!HasValidObjectName())
	{
		return;
	}
	for (const FConvaiTrackedProperty& Prop : TrackedProperties)
	{
		if (!Prop.PropertyPath.IsNone())
		{
			OutKeys.AddUnique(ConvaiContextFormat::SanitizeKey(BuildStateKey(Prop)));
		}
	}
	if (MovementAwareness.IsMovementStateEnabled() && !HasMovementStateKeyCollision())
	{
		OutKeys.AddUnique(ConvaiContextFormat::SanitizeKey(BuildMovementStateKey()));
	}
}

void UConvaiObjectComponent::RebuildCache()
{
#if WITH_EDITOR
	// Re-latch paths whose Blueprint variables were renamed before resolving.
	HealTrackedPropertyPaths();
#endif
	for (FConvaiTrackedProperty& Property : TrackedProperties)
	{
		Property.NormalizeResponseSettings();
	}
	MovementAwareness.NormalizeResponseSettings();

	AActor* Owner = GetOwner();
	if (MovementAwareness.IsMovementStateEnabled() && HasMovementStateKeyCollision())
	{
		CONVAI_LOG(ConvaiObjectComponentLog, Warning,
			TEXT("ConvaiObjectComponent on %s: Add Movement State is disabled at runtime because a tracked property already uses '%s'. Choose another Alias."),
			Owner ? *Owner->GetName() : TEXT("<no owner>"), *BuildMovementStateKey());
	}

	TArray<FCachedEntry> Previous = MoveTemp(Cache);
	TBitArray<> Carried(false, Previous.Num());
	Cache.SetNum(TrackedProperties.Num());
	TArray<FString> RetiredKeys;
	TArray<int32> Rekeyed;

	for (int32 i = 0; i < TrackedProperties.Num(); ++i)
	{
		const FConvaiTrackedProperty& P = TrackedProperties[i];
		FCachedEntry& C = Cache[i];

		int32 Match = INDEX_NONE;
		for (int32 j = 0; j < Previous.Num() && Match == INDEX_NONE; ++j)
		{
			if (!Carried[j] && Previous[j].PropertyPath == P.PropertyPath)
			{
				Match = j;
			}
		}
		if (Match != INDEX_NONE)
		{
			Carried[Match] = true;
			C = Previous[Match];
			const FString OldKey = BuildStateKey(C.PropertyPath, C.Alias);
			C.Alias = P.Alias;
			if (!P.PropertyPath.IsNone()
				&& ConvaiContextFormat::SanitizeKey(OldKey) != ConvaiContextFormat::SanitizeKey(BuildStateKey(P)))
			{
				RetiredKeys.Add(OldKey);
				Rekeyed.Add(i);
			}
			continue;
		}

		C.PropertyPath = P.PropertyPath;
		C.PathTokens = ConvaiPropertyReflection::TokenizePath(P.PropertyPath);
		C.Alias = P.Alias;
		if (!Owner || P.PropertyPath.IsNone())
		{
			continue;
		}
		FConvaiResolvedProperty Resolved;
		if (!ConvaiPropertyReflection::ResolvePath(Owner, P.PropertyPath, Resolved))
		{
			// Not terminal — object hops are legitimately null early (spawn
			// order); the poll keeps retrying until the path resolves.
			CONVAI_LOG(ConvaiObjectComponentLog, Warning,
				TEXT("ConvaiObjectComponent on %s: tracked property '%s' does not resolve on owner (yet) — will keep retrying on the poll."),
				*Owner->GetName(), *P.PropertyPath.ToString());
		}
		else if (!Resolved.bSupported)
		{
			CONVAI_LOG(ConvaiObjectComponentLog, Warning,
				TEXT("ConvaiObjectComponent on %s: tracked property '%s' is an unsupported type (object/delegate/etc.). Skipped."),
				*Owner->GetName(), *P.PropertyPath.ToString());
		}
	}
	for (int32 j = 0; j < Previous.Num(); ++j)
	{
		if (!Carried[j] && !Previous[j].PropertyPath.IsNone())
		{
			RetiredKeys.Add(BuildStateKey(Previous[j].PropertyPath, Previous[j].Alias));
		}
	}

	// Only a registered object has keys on the chatbots; registration seeds the rest.
	if ((RetiredKeys.Num() > 0 || Rekeyed.Num() > 0) && HasValidObjectName() && IsRegisteredWithConvai())
	{
		// The keys carry the live name, so another object of that name keeps a key it
		// still produces.
		RetireStateKeys(ObjectEntry.Name, MoveTemp(RetiredKeys));
		for (const int32 i : Rekeyed)
		{
			if (Cache[i].bResolved)
			{
				BroadcastToAllChatbots(TrackedProperties[i], BuildStateKey(TrackedProperties[i]),
					Cache[i].LastFormattedValue, EC_RunLLMOption::Never);
			}
			else
			{
				// The poll seeds the new key once the path resolves, even with an unchanged value.
				Cache[i].bEverResolved = false;
			}
		}
	}

	// Duplicate effective keys (alias collisions) silently overwrite each other
	// on every chatbot — warn so the designer fixes the alias.
	TSet<FString> SeenKeys;
	for (const FConvaiTrackedProperty& P : TrackedProperties)
	{
		if (P.PropertyPath.IsNone())
		{
			continue;
		}
		const FString EffectiveKey = ConvaiContextFormat::SanitizeKey(BuildStateKey(P));
		bool bAlreadySeen = false;
		SeenKeys.Add(EffectiveKey, &bAlreadySeen);
		if (bAlreadySeen)
		{
			CONVAI_LOG(ConvaiObjectComponentLog, Warning,
				TEXT("ConvaiObjectComponent on %s: two tracked properties share the effective state key '%s' (check Alias fields) — they will overwrite each other."),
				Owner ? *Owner->GetName() : TEXT("<no owner>"), *EffectiveKey);
		}
	}
}

bool UConvaiObjectComponent::IsCacheStale() const
{
	if (Cache.Num() != TrackedProperties.Num())
	{
		return true;
	}
	for (int32 i = 0; i < Cache.Num(); ++i)
	{
		if (Cache[i].PropertyPath != TrackedProperties[i].PropertyPath
			|| Cache[i].Alias != TrackedProperties[i].Alias)
		{
			return true;
		}
	}
	return false;
}

void UConvaiObjectComponent::RecordCurrentValues()
{
	if (IsCacheStale())
	{
		RebuildCache();
	}
	// Every value is read before any is recorded: a function leaf runs Blueprint, which
	// can edit the tracked properties and so rebuild the cache. An entry it adds stays
	// unseeded, for the poll to seed.
	TArray<FName> Paths;
	for (const FCachedEntry& Entry : Cache)
	{
		Paths.Add(Entry.PropertyPath);
	}
	AActor* Owner = GetOwner();
	TMap<FName, FString> Values;
	for (const FName Path : Paths)
	{
		FConvaiResolvedProperty Resolved;
		if (Owner && !Path.IsNone() && ConvaiPropertyReflection::ResolvePath(Owner, Path, Resolved)
			&& Resolved.bSupported)
		{
			Values.Add(Path, ConvaiPropertyReflection::FormatValue(Resolved));
		}
	}
	// A leaf that wrote the array directly left the cache stale: registration seeds only
	// what is still tracked.
	if (IsCacheStale())
	{
		RebuildCache();
	}
	for (FCachedEntry& Entry : Cache)
	{
		const FString* Value = Values.Find(Entry.PropertyPath);
		Entry.LastFormattedValue = Value ? *Value : FString();
		Entry.bResolved = Value != nullptr;
		Entry.bEverResolved = Value != nullptr;
	}
}

void UConvaiObjectComponent::RefreshEnvironmentEntries()
{
	// A merged set's single entry is rebuilt from the whole set. An object the chatbot
	// already had at connect reaches the server at the next connect, as Add Object documents.
	TArray<UConvaiChatbotComponent*> Targets;
	GatherEligibleChatbots(Targets);
	for (UConvaiChatbotComponent* Chatbot : Targets)
	{
		Chatbot->AddOrUpdateObjectFromComponent(this);
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
		// Label each row with the key suffix the LLM will actually see in state
		// updates: the alias when set, sanitized the same way the chatbot
		// sanitizes state keys.
		const FString RowLabel = ConvaiContextFormat::SanitizeKey(StateKeySuffix(P.PropertyPath, P.Alias));
		AppendRow(RowLabel, P.Description, &P.StateValueDescriptions);
	}

	return Out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blueprint mutators
// ─────────────────────────────────────────────────────────────────────────────

bool UConvaiObjectComponent::AddTrackedProperty(const FConvaiTrackedProperty& InProperty)
{
	FConvaiTrackedProperty Property = InProperty;
	Property.NormalizeResponseSettings();

	if (Property.PropertyPath.IsNone())
	{
		CONVAI_LOG(ConvaiObjectComponentLog, Warning, TEXT("AddTrackedProperty: PropertyPath is None"));
		return false;
	}
	if (MovementAwareness.IsMovementStateEnabled() && HasValidObjectName()
		&& ConvaiContextFormat::SanitizeKey(BuildStateKey(Property))
			== ConvaiContextFormat::SanitizeKey(BuildMovementStateKey()))
	{
		CONVAI_LOG(ConvaiObjectComponentLog, Warning,
			TEXT("AddTrackedProperty: '%s' would collide with the reserved movement state key '%s'. Choose another Alias or turn off Add Movement State."),
			*Property.PropertyPath.ToString(), *BuildMovementStateKey());
		return false;
	}

	for (const FConvaiTrackedProperty& Existing : TrackedProperties)
	{
		if (Existing.PropertyPath == Property.PropertyPath)
		{
			CONVAI_LOG(ConvaiObjectComponentLog, Warning,
				TEXT("AddTrackedProperty: '%s' already tracked. Use UpdateTrackedProperty to modify."),
				*Property.PropertyPath.ToString());
			return false;
		}
	}

	AActor* Owner = GetOwner();
	FConvaiResolvedProperty Resolved;
	const bool bResolvesNow = Owner && ConvaiPropertyReflection::ResolvePath(Owner, Property.PropertyPath, Resolved);
	if (bResolvesNow && !Resolved.bSupported)
	{
		// The path exists but points at a type we can never format —
		// that's a hard error, unlike a hop that's merely null right now.
		CONVAI_LOG(ConvaiObjectComponentLog, Warning,
			TEXT("AddTrackedProperty: '%s' is an unsupported type."),
			*Property.PropertyPath.ToString());
		return false;
	}
	if (Owner && !bResolvesNow)
	{
		CONVAI_LOG(ConvaiObjectComponentLog, Log,
			TEXT("AddTrackedProperty: '%s' does not resolve right now — added anyway; it starts broadcasting once it resolves (object-reference hops may still be null)."),
			*Property.PropertyPath.ToString());
	}

	TrackedProperties.Add(Property);
	RebuildCache();

	if (!HasValidObjectName())
	{
		CONVAI_LOG(ConvaiObjectComponentLog, Warning,
			TEXT("AddTrackedProperty: ObjectEntry.Name is empty on %s; the new property is staged but won't broadcast until it is set."),
			Owner ? *Owner->GetName() : TEXT("<no owner>"));
	}
	else if (IsRegisteredWithConvai())
	{
		// Pushed now rather than at the next poll; the rebuild left the new entry unseeded.
		// A disabled object is seeded when it registers.
		if (bResolvesNow)
		{
			// A function leaf runs Blueprint, which can edit the tracked properties (moving
			// the cache), clear this object's name, disable it or destroy its actor.
			const FString Value = ConvaiPropertyReflection::FormatValue(Resolved);
			if (!IsRegisteredWithConvai() || !HasValidObjectName())
			{
				return true;
			}
			// A leaf that wrote the array directly left the cache stale; the next poll rebuilds
			// it and seeds what is still tracked.
			FCachedEntry* Entry = IsCacheStale() ? nullptr : Cache.FindByPredicate([&Property](const FCachedEntry& Candidate)
			{
				return Candidate.PropertyPath == Property.PropertyPath;
			});
			if (Entry)
			{
				Entry->LastFormattedValue = Value;
				Entry->bResolved = true;
				Entry->bEverResolved = true;
				BroadcastToAllChatbots(Property, BuildStateKey(Entry->PropertyPath, Entry->Alias), Value,
					EC_RunLLMOption::Never);
			}
		}
		RefreshEnvironmentEntries();
	}

	return true;
}

bool UConvaiObjectComponent::RemoveTrackedProperty(FName PropertyPath)
{
	if (TrackedProperties.RemoveAll([&](const FConvaiTrackedProperty& P){ return P.PropertyPath == PropertyPath; }) == 0)
	{
		return false;
	}
	// Retires the removed property's key on every chatbot.
	RebuildCache();
	if (HasValidObjectName() && IsRegisteredWithConvai())
	{
		RefreshEnvironmentEntries();
	}
	return true;
}

bool UConvaiObjectComponent::UpdateTrackedProperty(FName PropertyPath, const FConvaiTrackedProperty& NewSettings)
{
	for (int32 i = 0; i < TrackedProperties.Num(); ++i)
	{
		FConvaiTrackedProperty& Existing = TrackedProperties[i];
		if (Existing.PropertyPath != PropertyPath)
		{
			continue;
		}

		FConvaiTrackedProperty Candidate = NewSettings;
		Candidate.PropertyPath = Existing.PropertyPath;
		Candidate.NormalizeResponseSettings();
		if (MovementAwareness.IsMovementStateEnabled() && HasValidObjectName()
			&& ConvaiContextFormat::SanitizeKey(BuildStateKey(Candidate))
				== ConvaiContextFormat::SanitizeKey(BuildMovementStateKey()))
		{
			CONVAI_LOG(ConvaiObjectComponentLog, Warning,
				TEXT("UpdateTrackedProperty: the Alias would collide with the reserved movement state key '%s'."),
				*BuildMovementStateKey());
			return false;
		}

		// Preserve original path; treat update as descriptor refresh.
		Existing.Description = Candidate.Description;
		Existing.StateValueDescriptions = Candidate.StateValueDescriptions;
		Existing.ShouldRespond = Candidate.ShouldRespond;
		Existing.Delivery = Candidate.Delivery;
		Existing.bFlushImmediately = Candidate.bFlushImmediately;
		Existing.Alias = Candidate.Alias;

		// An Alias change re-keys the state: the rebuild retires the old key and
		// re-seeds the last value under the new one with Never.
		if (IsCacheStale())
		{
			RebuildCache();
		}
		if (HasValidObjectName() && IsRegisteredWithConvai())
		{
			RefreshEnvironmentEntries();
		}
		return true;
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

bool UConvaiObjectComponent::KeepsHeldUpdate(UConvaiChatbotComponent& Chatbot, const FString& Key) const
{
	const FString* Held = Chatbot.HeldContext.StateValues.Find(Key);
	FString Known;
	// A reconnect that will not replay what its drop left waiting downgrades it to Never at the join, so the seed sends the
	// value now, as beta.33 did. A session the game stopped is not reconnected: its next Start Session clears the drop.
	const bool bWillDowngrade = Chatbot.DropAtSeconds >= 0.0 && Chatbot.DropReason != EC_DisconnectReason::Explicit
		&& !UConvaiChatbotComponent::ShouldReplayAfterGap(FPlatformTime::Seconds() - Chatbot.DropAtSeconds, Chatbot.DropReason);
	if (!Held || bWillDowngrade || !Chatbot.DynamicContextTracker.GetStateValue(Key, Known))
	{
		return false;
	}
	const UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this);
	if (!Subsystem)
	{
		return false;
	}
	for (const UConvaiObjectComponent* Member : Subsystem->GetAllObjectComponents())
	{
		if (!IsValid(Member) || !IsValid(Member->GetOwner())
			|| !Member->ObjectEntry.Name.Equals(ObjectEntry.Name, ESearchCase::IgnoreCase))
		{
			continue;
		}
		for (const FCachedEntry& Other : Member->Cache)
		{
			if (Other.bResolved && Other.LastFormattedValue == *Held
				&& ConvaiContextFormat::SanitizeKey(Member->BuildStateKey(Other.PropertyPath, Other.Alias)) == Key)
			{
				return true;
			}
		}
	}
	return false;
}

void UConvaiObjectComponent::SeedInitialStateOntoChatbot(UConvaiChatbotComponent* Chatbot) const
{
	if (!IsValid(Chatbot) || !HasValidObjectName())
	{
		return;
	}

	// An update held for the conversation's next pause is the newest write to its key, and
	// asks for an answer; a normal write here would drop it and send the key now, as Never,
	// or as Always when a Watch Property promoted it. It stays while the character already
	// has the key and a live member of this object (merged members can disagree) still
	// writes the held value. A key the character lacks, as at a first session start, is
	// seeded now rather than left for the held update to introduce at the pause, and so is
	// one held across a drop the reconnect will not replay: it would reach the character
	// only at the pause, as Never. KeepsHeldUpdate holds the rule, which BroadcastToAllChatbots
	// applies to the other seeds.
	// ponytail: the gap is judged when the seed runs (for a reconnect, as its connect is
	// prepared); one that reaches 30 s before the character joins still downgrades a kept
	// update. Judge it at the join if that matters.

	// The cache's own keys: between a direct array write and the next poll they still
	// name the values cached with them.
	for (const FCachedEntry& Entry : Cache)
	{
		if (!Entry.bResolved)
		{
			continue;
		}
		const FString Key = BuildStateKey(Entry.PropertyPath, Entry.Alias);
		if (KeepsHeldUpdate(*Chatbot, ConvaiContextFormat::SanitizeKey(Key)))
		{
			continue;
		}
		// Initial-seeding always uses Never regardless of the prop's RunLLMOption,
		// to avoid hammering the LLM on session boot. A seed is not a change, so
		// it leaves an armed Watch Property waiting: re-seeding a merged set's
		// survivor over the departed member's value would otherwise fire it.
		Chatbot->SetContextStateInternal(
			Key,
			Entry.LastFormattedValue,
			EC_RunLLMOption::Never,
			EConvaiContextDelivery::SendNormally,
			/*bFlushImmediately*/ false,
			/*bOmitPreviousValue*/ false,
			/*bEvaluateWatches*/ false);
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

void UConvaiObjectComponent::BroadcastToAllChatbots(const FConvaiTrackedProperty& Prop,
	const FString& StateKey, const FString& Value, EC_RunLLMOption RunLLMOption,
	bool bFlushImmediately)
{
	// A Never write is a seed (a first resolution, Add Tracked Property, a re-key), and keeps
	// an update held for the conversation's next pause by KeepsHeldUpdate's rule. The poll
	// sends a change, which replaces one, itself.
	const FString Key = ConvaiContextFormat::SanitizeKey(StateKey);
	TArray<UConvaiChatbotComponent*> Targets;
	GatherEligibleChatbots(Targets);
	for (UConvaiChatbotComponent* Chatbot : Targets)
	{
		if (RunLLMOption == EC_RunLLMOption::Never && KeepsHeldUpdate(*Chatbot, Key))
		{
			continue;
		}
		Chatbot->SetContextState(StateKey, Value, RunLLMOption,
			Prop.GetEffectiveDelivery(),
			bFlushImmediately && Prop.GetEffectiveFlushImmediately());
	}
}

void UConvaiObjectComponent::EvaluateTrackedProperties()
{
#if WITH_TESTS
	if (EvaluateTestHook)
	{
		EvaluateTestHook();
	}
#endif
	// Blueprint can write the public array directly, bypassing Add/Update.
	// Keep the serialized/runtime representation canonical as well as clamping
	// the values at the eventual send site.
	for (FConvaiTrackedProperty& Property : TrackedProperties)
	{
		Property.NormalizeResponseSettings();
	}
	MovementAwareness.NormalizeResponseSettings();

	AActor* Owner = GetOwner();
	if (!IsValid(Owner) || !HasValidObjectName())
	{
		// Owner may be pending-kill mid-teardown (memory still alive but stale); bail
		// before reading reflection off a corpse.
		return;
	}

	// Blueprint can also add, remove, re-path or re-alias entries by writing the array.
	if (IsCacheStale())
	{
		RebuildCache();
	}

	// Proximity is synthesised centrally by UConvaiContextSubsystem's spatial pass —
	// as Context Facts that also cover other characters, the player, and inter-entity
	// relations — not per-object here. bIncludeInSpatialAwareness is still honoured:
	// the subsystem reads it to decide whether this object participates.

	for (int32 i = 0; i < TrackedProperties.Num(); ++i)
	{
		const FName Path = TrackedProperties[i].PropertyPath;
		if (Path.IsNone())
		{
			continue;
		}

		FConvaiResolvedProperty Resolved;
		if (!ConvaiPropertyReflection::ResolveTokens(Owner, Cache[i].PathTokens, Resolved) || !Resolved.bSupported)
		{
			// Unresolvable right now (null sub-actor hop, owner re-init) — NOT
			// terminal: retried next poll, recovered via the branch below.
			Cache[i].bResolved = false;
			continue;
		}

		const FString Current = ConvaiPropertyReflection::FormatValue(Resolved);
		// A function leaf runs Blueprint, which can edit the tracked properties (moving both
		// arrays), clear this object's name, disable it or destroy its actor. The next poll
		// reads what is left.
		if (Resolved.LeafFunction && (!IsRegisteredWithConvai() || !HasValidObjectName()
			|| IsCacheStale() || !TrackedProperties.IsValidIndex(i)
			|| TrackedProperties[i].PropertyPath != Path))
		{
			return;
		}
		const FConvaiTrackedProperty& P = TrackedProperties[i];
		FCachedEntry& C = Cache[i];

		if (!C.bResolved)
		{
			// First resolution (late-spawning sub-actor) or recovery: broadcast
			// as a seed (Never). bEverResolved makes the first-ever seed fire
			// even when the value equals the empty cache default.
			C.bResolved = true;
			if (!C.bEverResolved || Current != C.LastFormattedValue)
			{
				C.bEverResolved = true;
				C.LastFormattedValue = Current;
				BroadcastToAllChatbots(P, BuildStateKey(P), Current, EC_RunLLMOption::Never);
			}
			continue;
		}

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

		// A change replaces an update held for the pause, so it does not go through
		// BroadcastToAllChatbots, whose Never writes are seeds that keep one.
		const FString Key = BuildStateKey(P);
		TArray<UConvaiChatbotComponent*> Targets;
		GatherEligibleChatbots(Targets);
		for (UConvaiChatbotComponent* Chatbot : Targets)
		{
			Chatbot->SetContextState(Key, Current, Effective, P.GetEffectiveDelivery(),
				Effective != EC_RunLLMOption::Never && P.GetEffectiveFlushImmediately());
		}
	}

	// Debug-only: refresh the nav-path visualisation on the same poll cadence
	// the tracked properties use. When the toggle goes off at runtime, clear
	// the leftover persistent lines on the next poll (PostEditChangeChainProperty
	// covers the editor-details-panel case immediately).
	if (bDebugDrawProximityPaths)
	{
		UpdateProximityDebugPaths();
	}
	else if (ProximityDebugBatcher)
	{
		ProximityDebugBatcher->Flush();
		ProximityDebugCaches.Reset();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Proximity debug draw
// ─────────────────────────────────────────────────────────────────────────────

void UConvaiObjectComponent::UpdateProximityDebugPaths()
{
	AActor* ObjectOwner = GetOwner();
	if (!IsValid(ObjectOwner))
	{
		return;
	}

	// Hoist per-pass: resolved object location is identical for every chatbot
	// in this tick. Call with SourceActor=nullptr so the (cheap) position math
	// runs but the nav query is skipped — that part is done per-chatbot below
	// inside its pathfind-skip gate. With Movement Points authored this returns
	// the first enabled point (deterministic representative), so the movement
	// gate below keys on a stable location.
	AActor* HA = nullptr; USceneComponent* HC = nullptr;
	FVector ObjectLoc = FVector::ZeroVector; float HR = 0.0f;
	bool HM = false;
	bool HS = false, HThere = false, HReach = false;
	FVector HPE = FVector::ZeroVector; TArray<FVector> HPP;
	float HTD = 0.0f; int32 HPI = INDEX_NONE;
	ObjectEntry.ResolveGoalLocation(/*SourceActor*/ nullptr,
		HA, HC, ObjectLoc, HR, HM, HS, HThere, HReach, HPE, HPP, HTD, HPI);

	// Prune cache entries whose chatbot was destroyed since last pass.
	for (auto It = ProximityDebugCaches.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	TArray<UConvaiChatbotComponent*> Targets;
	GatherEligibleChatbots(Targets);
	bool bAnyRecomputed = false;
	for (UConvaiChatbotComponent* Chatbot : Targets)
	{
		AActor* ChatbotOwner = Chatbot->GetOwner();
		if (!IsValid(ChatbotOwner))
		{
			continue;
		}

		const FVector ChatbotLoc = ChatbotOwner->GetActorLocation();
		FProximityDebugCache& ProxCache = ProximityDebugCaches.FindOrAdd(Chatbot);

		// Skip the expensive nav pathfind while neither end has moved >100 uu
		// since the last recompute (10000 = 100 uu squared, kept squared to
		// skip the sqrt). The previous persistent lines stay on screen.
		if (ProxCache.bInitialized &&
			FVector::DistSquared(ChatbotLoc, ProxCache.LastChatbotLocation) < 10000.0f &&
			FVector::DistSquared(ObjectLoc,  ProxCache.LastObjectLocation)  < 10000.0f)
		{
			continue;
		}

		// Single call does both the position resolution AND the source-relative
		// outputs (arrival check + nav query + path-point capture) the draw
		// below consumes.
		AActor* GA = nullptr; USceneComponent* GC = nullptr;
		FVector ResolvedLoc = FVector::ZeroVector; float AR = 0.0f;
		bool Mode = false;
		bool bResolved = false, bAlreadyThere = false, bReachable = false;
		FVector PathEnd = FVector::ZeroVector;
		TArray<FVector> PathPoints;
		float TravelDist = 0.0f; int32 PointIdx = INDEX_NONE;
		ObjectEntry.ResolveGoalLocation(ChatbotOwner,
			GA, GC, ResolvedLoc, AR, Mode, bResolved,
			bAlreadyThere, bReachable, PathEnd, PathPoints, TravelDist, PointIdx);

		ProxCache.LastChatbotLocation = ChatbotLoc;
		ProxCache.LastObjectLocation  = ObjectLoc;
		ProxCache.bAlreadyThere       = bAlreadyThere;
		ProxCache.bReachable          = bReachable;
		ProxCache.LastPathPoints      = MoveTemp(PathPoints);
		ProxCache.bInitialized        = true;
		bAnyRecomputed                = true;
	}

	// Redraw only when a chatbot actually recomputed its path on this pass.
	// Otherwise the previous persistent lines stay on screen, which matches
	// the user-facing "stays until the next evaluation" promise.
	if (bAnyRecomputed)
	{
		RefreshProximityDebugDraw();
	}
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
	for (const auto& Pair : ProximityDebugCaches)
	{
		const FProximityDebugCache& C = Pair.Value;
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

	if (bConvaiEnabled)
	{
		RegisterWithConvai();
	}
}

void UConvaiObjectComponent::SetConvaiObjectEnabled(bool bEnabled)
{
	if (bConvaiEnabled == bEnabled)
	{
		return;
	}
	bConvaiEnabled = bEnabled;

	// Before BeginPlay the flag is just data — BeginPlay honors it.
	if (!HasBegunPlay())
	{
		return;
	}
	if (bEnabled)
	{
		RegisterWithConvai();
	}
	else
	{
		UnregisterFromConvai();
	}
}

void UConvaiObjectComponent::RegisterWithConvai()
{
	// No chatbot holds this object's keys yet (first registration, or the disable
	// removed them), so the current values are the seed, not a cache from before.
	RecordCurrentValues();
	// Reading can run a function leaf's Blueprint, which may have disabled this object or
	// destroyed it or its actor; its End Play found nothing registered to tear down.
	if (!bConvaiEnabled || !IsValid(this) || !IsValid(GetOwner()))
	{
		return;
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

void UConvaiObjectComponent::UnregisterFromConvai()
{
	// Not registered: End Play after Set Convai Object Enabled(false), or a name the
	// subsystem refused. Another object may hold the name by now.
	if (!IsRegisteredWithConvai())
	{
		return;
	}
	// Teardown keys on the name this object registered under, not on ObjectEntry.Name,
	// which Blueprint can have rewritten since.
	const FString Name = RegisteredName;

	// Unregister from the object registry FIRST so the merged-set sibling lookup below
	// sees only the SURVIVORS (this component excluded). Chatbot eligibility
	// (GatherEligibleChatbots) depends only on the chatbot registry, so unregistering
	// before the chatbot cleanup is safe.
	if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		Subsystem->UnregisterObjectComponent(this);
	}

	RetireFromChatbots(Name);
	// After a rename, the poll, the tracked-property mutators and the environment
	// refresh wrote under the new name as well.
	if (HasValidObjectName() && !ObjectEntry.Name.Equals(Name, ESearchCase::IgnoreCase))
	{
		RetireFromChatbots(ObjectEntry.Name);
	}
}

void UConvaiObjectComponent::RetireFromChatbots(const FString& Name)
{
	// Reconcile everything this object pushed onto chatbots under Name — without this the LLM
	// would still see it in scene metadata / action_config and keep "attending to" it:
	//   - Each chatbot's DynamicContextTracker (tracked-property state keys), via RetireStateKeys,
	//     which keeps a key a surviving merged member can still read.
	//   - Each chatbot's EnvironmentData.Objects, via RemoveObject — UNLESS a same-named
	//     sibling survives (a merged set), in which case the logical object lives on and
	//     we rebuild its entry from a surviving member instead.
	//   - Each chatbot's EnvironmentData.CurrentAttentionObject when it points at THIS
	//     object: cleared when we were the last member, else its Ref repointed to the
	//     surviving member.

	// Merged set: find a still-registered sibling called Name now (its entry is rebuilt
	// under that name) with a LIVE owner. 'this' is already unregistered, so it won't
	// appear here. If a sibling exists the logical object outlives us (rebuild its entry
	// from it); otherwise we were the last member (remove it). EndPlay runs sequentially
	// on the game thread, so the last surviving member is the one that actually removes it.
	// A member registered under Name and renamed by Blueprint since leaves Name as well, and
	// goes on under the name it carries now.
	UConvaiObjectComponent* SurvivingSibling = nullptr;
	TArray<UConvaiObjectComponent*> Renamed;
	if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		for (UConvaiObjectComponent* Member : Subsystem->GetAllObjectComponents())
		{
			if (!IsValid(Member) || !IsValid(Member->GetOwner()))
			{
				continue;
			}
			if (Member->ObjectEntry.Name.Equals(Name, ESearchCase::IgnoreCase))
			{
				if (!SurvivingSibling)
				{
					SurvivingSibling = Member;
				}
			}
			else if (Member->RegisteredName.Equals(Name, ESearchCase::IgnoreCase))
			{
				Renamed.Add(Member);
			}
		}
	}

	// What this object, and each renamed member at its registration, wrote under Name.
	TArray<FString> Keys;
	auto AddKeysUnderName = [&Keys, &Name](const UConvaiObjectComponent& Writer)
	{
		for (const FCachedEntry& Entry : Writer.Cache)
		{
			if (!Entry.PropertyPath.IsNone())
			{
				Keys.Add(Writer.BuildStateKey(Name, Entry.PropertyPath, Entry.Alias));
			}
		}
	};
	AddKeysUnderName(*this);
	for (const UConvaiObjectComponent* Member : Renamed)
	{
		AddKeysUnderName(*Member);
	}
	RetireStateKeys(Name, MoveTemp(Keys));

	TArray<UConvaiChatbotComponent*> Targets;
	GatherEligibleChatbots(Targets);
	for (UConvaiChatbotComponent* Chatbot : Targets)
	{
		// The central movement cache normally retires this synthetic key on
		// its next poll. When the last object unregisters, however, that poll
		// clock stops immediately, so clean the logical object's key here.
		if (!SurvivingSibling)
		{
			Chatbot->RemoveContextState(BuildMovementStateKey(Name));
		}

		if (SurvivingSibling)
		{
			// Logical object survives: rebuild its entry from the live sibling so its
			// nav fields / description / Ref no longer reference this destroyed member
			// (the rebuild groups the survivors — 'this' is already unregistered), and
			// repoint the attention slot's Ref if it points at this object.
			Chatbot->AddOrUpdateObjectFromComponent(SurvivingSibling);
			if (Chatbot->EnvironmentData.CurrentAttentionObject.Name == Name)
			{
				Chatbot->EnvironmentData.CurrentAttentionObject.Ref = SurvivingSibling->GetOwner();
			}
			continue;
		}

		Chatbot->RemoveObject(Name);

		// Attention slot: clear unconditionally when it points at us, regardless
		// of source (Gaze or Explicit). Match by Name since Ref may already be
		// stale by the time the destroyed object's EndPlay runs.
		if (Chatbot->EnvironmentData.CurrentAttentionObject.Name == Name)
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
				/*Delivery*/    EConvaiContextDelivery::SendNormally,
				/*bAddAttentionEvent*/ false,
				/*bFlushImmediately*/ false);
		}
	}

	// A renamed member's entry and values under the name it carries now, built once Name's own
	// entry is settled so a shared description does not point at a removed one ("Same as ...").
	for (UConvaiObjectComponent* Member : Renamed)
	{
		// A value it cannot read now is seeded under the new name once it can, as RetireStateKeys arranges for a survivor.
		for (FCachedEntry& Entry : Member->Cache)
		{
			if (!Entry.bResolved)
			{
				Entry.bEverResolved = false;
			}
		}
	}
	for (UConvaiChatbotComponent* Chatbot : Targets)
	{
		for (UConvaiObjectComponent* Member : Renamed)
		{
			Chatbot->AddOrUpdateObjectFromComponent(Member);
			Member->SeedInitialStateOntoChatbot(Chatbot);
		}
	}
}

void UConvaiObjectComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// A no-op when Set Convai Object Enabled(false) already unregistered it.
	UnregisterFromConvai();

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

USceneComponent* UConvaiObjectComponent::GetResolvedComponent()
{
	// FConvaiObjectEntry::ResolveComponent caches into ObjectEntry.ResolvedComponent and
	// revalidates the cache every call. Returns nullptr for whole-actor scope (no
	// ComponentName filter).
	return ObjectEntry.ResolveComponent();
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
	const FString& Text, EC_RunLLMOption ShouldRespond, EConvaiContextDelivery Delivery)
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
		Chatbot->TrySetObjectInAttentionFromGaze(ObjectEntry, Text, ShouldRespond, Delivery, /*bAddAttentionEvent=*/ true, /*bFlushImmediately=*/ false);
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
void UConvaiObjectComponent::HealTrackedPropertyPaths()
{
	UObject* Root = GetOwner();
	if (!Root)
	{
		// Blueprint-stored component template (SCS): no owning actor instance.
		// Heal against the owning class's CDO — same layout, including BP vars.
		if (UClass* OwningClass = GetTypedOuter<UClass>())
		{
			Root = OwningClass->GetDefaultObject();
		}
	}
	if (!Root)
	{
		return;
	}

	for (FConvaiTrackedProperty& P : TrackedProperties)
	{
		if (P.PropertyPath.IsNone())
		{
			continue;
		}
		FName Healed;
		if (ConvaiPropertyReflection::HealPath(Root, P.PropertyPath, P.PropertyPathGuids, Healed))
		{
			CONVAI_LOG(ConvaiObjectComponentLog, Log,
				TEXT("ConvaiObjectComponent on %s: tracked property path re-latched after variable rename: '%s' -> '%s'."),
				*Root->GetName(), *P.PropertyPath.ToString(), *Healed.ToString());
			P.PropertyPath = Healed;
		}
	}
}

void UConvaiObjectComponent::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedChainEvent)
{
	// Flipping a point's Attachment reinterprets the SAME stored transform in
	// the other space, teleporting the point. Rebase it so the point stays
	// exactly where it was in the world — Attachment has only two values, so
	// the previous space is always the other one.
	if (PropertyChangedChainEvent.GetPropertyName()
			== GET_MEMBER_NAME_CHECKED(FConvaiMovementPoint, Attachment))
	{
		const int32 Index = PropertyChangedChainEvent.GetArrayIndex(
			GET_MEMBER_NAME_CHECKED(FConvaiObjectEntry, MovementPoints).ToString());
		if (ObjectEntry.MovementPoints.IsValidIndex(Index))
		{
			FConvaiMovementPoint& Point = ObjectEntry.MovementPoints[Index];
			const FTransform Anchor = GetMovementPointAnchorTransform();
			const FTransform Rebased =
				(Point.Attachment == EConvaiMovementPointAttachment::KeepWorldPosition)
					? Point.Transform * Anchor                      // was relative → bake to world
					: Point.Transform.GetRelativeTransform(Anchor); // was world → re-anchor
			// Location/rotation only — stored scale is inert at runtime (same
			// convention as the visualizer's gizmo writes).
			Point.Transform.SetTranslation(Rebased.GetTranslation());
			Point.Transform.SetRotation(Rebased.GetRotation());
		}
	}

	// Delivery and flushing have no meaning for a silent tracked property or
	// when both movement-edge policies are Never.
	// Normalize before Super so the whole editor transaction (including undo)
	// captures the dependent values together with Should Respond.
	for (FConvaiTrackedProperty& Property : TrackedProperties)
	{
		Property.NormalizeResponseSettings();
	}
	MovementAwareness.NormalizeResponseSettings();

	Super::PostEditChangeChainProperty(PropertyChangedChainEvent);

	// Re-latch renamed Blueprint variables (and backfill segment GUIDs for
	// freshly-bound paths) before validating.
	HealTrackedPropertyPaths();

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
