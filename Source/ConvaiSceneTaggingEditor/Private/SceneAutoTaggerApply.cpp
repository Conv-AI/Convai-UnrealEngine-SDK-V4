// Copyright Convai Inc. All Rights Reserved.

#include "SceneAutoTaggerApply.h"

#include "ConvaiObjectComponent.h"
#include "SceneObjects/ConvaiMovementPointGenerator.h"

#include "Components/ActorComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Selection.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelUtils.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UnrealEdGlobals.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "SceneAutoTaggerApply"

namespace SceneAutoTaggerApplyPrivate
{
	const FName AutoCreatedComponentBaseName(TEXT("ConvaiAutoTagObject"));
	const FName ManagedComponentMarker(TEXT("ConvaiAutoTag.Managed"));

	TArray<FName> BuildDesiredComponentTags(const UConvaiObjectComponent& Component)
	{
		TArray<FName> DesiredTags;
		DesiredTags.Reserve(Component.ComponentTags.Num() + 1);

		// Preserve all tags owned by the actor/component author. Only replace the
		// namespace this feature explicitly owns.
		for (const FName ExistingTag : Component.ComponentTags)
		{
			if (!FSceneAutoTaggerApply::IsManagedComponentTag(ExistingTag))
			{
				DesiredTags.Add(ExistingTag);
			}
		}

		DesiredTags.Add(ManagedComponentMarker);

		return DesiredTags;
	}

	bool IsAutoCreatedComponent(const UConvaiObjectComponent& Component)
	{
		return Component.ComponentTags.Contains(ManagedComponentMarker);
	}

	UConvaiObjectComponent* FindWholeActorComponentToUpdate(
		AActor& Actor,
		int32& OutComponentScopedCount,
		int32& OutManualWholeActorCount)
	{
		TArray<UConvaiObjectComponent*> Components;
		Actor.GetComponents<UConvaiObjectComponent>(Components);

		UConvaiObjectComponent* PreferredManagedComponent = nullptr;
		for (UConvaiObjectComponent* Component : Components)
		{
			if (!IsValid(Component))
			{
				continue;
			}

			if (Component->ObjectEntry.ObjectReference
				== EConvaiObjectReference::SpecificComponent)
			{
				++OutComponentScopedCount;
				continue;
			}

			if (IsAutoCreatedComponent(*Component))
			{
				if (!PreferredManagedComponent)
				{
					PreferredManagedComponent = Component;
				}
				continue;
			}
			++OutManualWholeActorCount;
		}

		return PreferredManagedComponent;
	}

	int32 CountComponentScopedEntries(AActor& Actor)
	{
		int32 Count = 0;
		TArray<UConvaiObjectComponent*> Components;
		Actor.GetComponents<UConvaiObjectComponent>(Components);
		for (const UConvaiObjectComponent* Component : Components)
		{
			if (IsValid(Component)
				&& Component->ObjectEntry.ObjectReference
					== EConvaiObjectReference::SpecificComponent)
			{
				++Count;
			}
		}
		return Count;
	}

	bool ComponentAlreadyMatches(
		const UConvaiObjectComponent& Component,
		const FSceneAutoTaggerApplyRequest& Request,
		const FString& NormalizedName,
		const FString& NormalizedDescription,
		const TArray<FName>& DesiredTags)
	{
		return Component.ObjectEntry.Name == NormalizedName
			&& Component.ObjectEntry.Description == NormalizedDescription
			&& Component.ObjectEntry.ObjectReference == EConvaiObjectReference::WholeActor
			&& Component.bMergeWithSameNamedObjects == Request.bMergeDuplicates
			&& Component.MergeGroupIndex
				== (Request.bMergeDuplicates ? Request.MergeGroupIndex : 0)
			&& Component.bIncludeInSpatialAwareness == Request.bIncludeInSpatialAwareness
			&& Component.bGazeable == Request.bGazeable
			&& Component.ComponentTags == DesiredTags;
	}

	void ConfigureComponent(
		UConvaiObjectComponent& Component,
		const FSceneAutoTaggerApplyRequest& Request,
		const FString& NormalizedName,
		const FString& NormalizedDescription)
	{
		const TArray<FName> DesiredTags = BuildDesiredComponentTags(Component);

		Component.SetFlags(RF_Transactional);
		Component.Modify();
		Component.PreEditChange(nullptr);

		Component.ObjectEntry.Name = NormalizedName;
		Component.ObjectEntry.Description = NormalizedDescription;
		Component.ObjectEntry.ObjectReference = EConvaiObjectReference::WholeActor;
		Component.bMergeWithSameNamedObjects = Request.bMergeDuplicates;
		Component.MergeGroupIndex = Request.bMergeDuplicates ? Request.MergeGroupIndex : 0;
		Component.bIncludeInSpatialAwareness = Request.bIncludeInSpatialAwareness;
		Component.bGazeable = Request.bGazeable;
		Component.ComponentTags = DesiredTags;

		// Do not write ObjectEntry.Ref. UConvaiObjectComponent::BeginPlay binds an
		// unset Ref to its owner, avoiding a redundant serialized actor reference.
		Component.PostEditChange();
	}

	void FillActorIdentity(FSceneAutoTaggerApplyActorOutcome& Outcome, AActor& Actor)
	{
		Outcome.Actor = &Actor;
		Outcome.ActorLabel = Actor.GetActorLabel();
		Outcome.ActorPath = Actor.GetPathName();
	}

	UConvaiObjectComponent* ResolveComponentAfterEditorReconstruction(
		AActor& Actor,
		UConvaiObjectComponent* OriginalComponent,
		const FName OriginalObjectName,
		const FString& DesiredConvaiName)
	{
		if (IsValid(OriginalComponent)
			&& OriginalComponent->GetOwner() == &Actor
			&& Actor.OwnsComponent(OriginalComponent))
		{
			return OriginalComponent;
		}

		// Editing an SCS-created component legitimately reruns the actor's construction
		// scripts. The old component can be destroyed and replaced during PostEditChange,
		// so never dereference it afterwards; locate the reconstructed counterpart.
		TArray<UConvaiObjectComponent*> Components;
		Actor.GetComponents<UConvaiObjectComponent>(Components);
		UConvaiObjectComponent* MatchingConvaiName = nullptr;
		for (UConvaiObjectComponent* Candidate : Components)
		{
			if (!IsValid(Candidate)
				|| Candidate->ObjectEntry.ObjectReference != EConvaiObjectReference::WholeActor)
			{
				continue;
			}

			if (Candidate->GetFName() == OriginalObjectName)
			{
				return Candidate;
			}
			if (!MatchingConvaiName && Candidate->ObjectEntry.Name == DesiredConvaiName)
			{
				MatchingConvaiName = Candidate;
			}
		}
		return MatchingConvaiName;
	}

	void MarkActorAndLevelPackagesDirty(AActor& Actor, UConvaiObjectComponent* Component)
	{
		if (IsValid(Component))
		{
			Component->MarkPackageDirty();
		}
		Actor.MarkPackageDirty();
		if (ULevel* Level = Actor.GetLevel())
		{
			// Actor may live in an external actor package (World Partition), so dirty
			// both the serialized actor/component owner and the containing level.
			Level->MarkPackageDirty();
		}
	}
}

const FString& FSceneAutoTaggerApply::GetManagedComponentTagPrefix()
{
	static const FString Prefix(TEXT("ConvaiAutoTag."));
	return Prefix;
}

bool FSceneAutoTaggerApply::IsManagedComponentTag(const FName Tag)
{
	return Tag.ToString().StartsWith(GetManagedComponentTagPrefix(), ESearchCase::IgnoreCase);
}

bool FSceneAutoTaggerApply::HasAutoTaggerOwnershipMarker(
	const UConvaiObjectComponent& Component)
{
	return Component.ComponentTags.Contains(SceneAutoTaggerApplyPrivate::ManagedComponentMarker);
}

bool FSceneAutoTaggerApply::IsRemovableInstanceComponent(
	const UConvaiObjectComponent& Component)
{
	const AActor* Owner = Component.GetOwner();
	return IsValid(Owner)
		&& Component.CreationMethod == EComponentCreationMethod::Instance
		&& Owner->GetInstanceComponents().Contains(&Component);
}

bool FSceneAutoTaggerApply::CanRemoveComponent(
	const UConvaiObjectComponent& Component,
	FText& OutReason)
{
	OutReason = FText::GetEmpty();
	const AActor* Owner = Component.GetOwner();
	const UWorld* World = IsValid(Owner) ? Owner->GetWorld() : nullptr;
	if (!IsRemovableInstanceComponent(Component) || !IsValid(Owner)
		|| Owner->IsTemplate() || !World || World->WorldType != EWorldType::Editor
		|| !Owner->GetLevel())
	{
		OutReason = LOCTEXT(
			"ProtectedComponentSource",
			"This Blueprint, native, construction-script, or non-editor component must be edited at its source.");
		return false;
	}
	if (FLevelUtils::IsLevelLocked(Owner->GetLevel()))
	{
		OutReason = LOCTEXT(
			"ProtectedLockedLevel",
			"Unlock this actor's level before removing its Convai component.");
		return false;
	}
	if (!GUnrealEd)
	{
		OutReason = LOCTEXT(
			"ProtectedEditorUnavailable",
			"The Unreal Editor deletion service is not available.");
		return false;
	}
	return GUnrealEd->CanDeleteComponent(&Component, &OutReason);
}

FSceneAutoTaggerApplyResult FSceneAutoTaggerApply::ApplyAcceptedRequests(
	const TArray<FSceneAutoTaggerApplyRequest>& Requests,
	const bool bGenerateMovementPoints)
{
	FSceneAutoTaggerApplyResult Result;
	Result.bMovementPointGenerationRequested = bGenerateMovementPoints;
	Result.ActorOutcomes.Reserve(Requests.Num());

	if (Requests.IsEmpty())
	{
		return Result;
	}

	if (!IsInGameThread())
	{
		for (const FSceneAutoTaggerApplyRequest& Request : Requests)
		{
			FSceneAutoTaggerApplyActorOutcome& Outcome = Result.ActorOutcomes.AddDefaulted_GetRef();
			Outcome.Actor = Request.Actor;
			Outcome.Outcome = ESceneAutoTaggerApplyOutcome::MustRunOnGameThread;
			Outcome.Message = LOCTEXT(
				"MustRunOnGameThread",
				"Scene metadata can only be applied from Unreal's game/editor thread.");
			++Result.FailedRequests;
		}
		return Result;
	}

	const FText TransactionDescription = FText::Format(
		LOCTEXT("ApplyTransaction", "Apply Convai metadata to {0} scene object(s)"),
		FText::AsNumber(Requests.Num()));
	FScopedTransaction Transaction(TransactionDescription);

	for (const FSceneAutoTaggerApplyRequest& Request : Requests)
	{
		FSceneAutoTaggerApplyActorOutcome& Outcome = Result.ActorOutcomes.AddDefaulted_GetRef();
		Outcome.Actor = Request.Actor;

		AActor* Actor = Request.Actor.Get();
		if (!IsValid(Actor))
		{
			Outcome.Outcome = ESceneAutoTaggerApplyOutcome::InvalidActor;
			Outcome.Message = LOCTEXT(
				"InvalidActor", "The reviewed actor no longer exists; nothing was applied.");
			++Result.FailedRequests;
			continue;
		}

		SceneAutoTaggerApplyPrivate::FillActorIdentity(Outcome, *Actor);
		UWorld* World = Actor->GetWorld();
		if (Actor->IsTemplate() || !World || World->WorldType != EWorldType::Editor
			|| !Actor->GetLevel())
		{
			Outcome.Outcome = ESceneAutoTaggerApplyOutcome::NonEditorActor;
			Outcome.Message = LOCTEXT(
				"NonEditorActor",
				"Only placed actors in an editor world can receive persistent scene metadata.");
			++Result.FailedRequests;
			continue;
		}

		const FString NormalizedName =
			FConvaiObjectEntry::NormalizeMovementPointName(Request.Name);
		if (NormalizedName.IsEmpty())
		{
			Outcome.Outcome = ESceneAutoTaggerApplyOutcome::EmptyObjectName;
			Outcome.Message = LOCTEXT(
				"EmptyObjectName", "Convai object names cannot be empty; nothing was applied.");
			++Result.FailedRequests;
			continue;
		}

		const FString NormalizedDescription = Request.Description.TrimStartAndEnd();
		UConvaiObjectComponent* Component = nullptr;
		int32 ManualWholeActorComponents = 0;
		if (Request.bUpdateExistingWholeActorComponent)
		{
			Component = SceneAutoTaggerApplyPrivate::FindWholeActorComponentToUpdate(
				*Actor, Outcome.PreservedComponentScopedEntries, ManualWholeActorComponents);
			if (!Component && ManualWholeActorComponents > 0)
			{
				Outcome.Outcome = ESceneAutoTaggerApplyOutcome::ManualComponentConflict;
				Outcome.Message = LOCTEXT(
					"ManualComponentConflict",
					"A manually authored whole-actor Convai component was added or already exists; review it explicitly instead of overwriting it.");
				++Result.FailedRequests;
				continue;
			}
		}
		else
		{
			Outcome.PreservedComponentScopedEntries =
				SceneAutoTaggerApplyPrivate::CountComponentScopedEntries(*Actor);
		}

		if (Component)
		{
			const TArray<FName> DesiredComponentTags =
				SceneAutoTaggerApplyPrivate::BuildDesiredComponentTags(*Component);

			if (SceneAutoTaggerApplyPrivate::ComponentAlreadyMatches(
				*Component,
				Request,
				NormalizedName,
				NormalizedDescription,
				DesiredComponentTags))
			{
				Outcome.Component = Component;
				Outcome.Outcome = ESceneAutoTaggerApplyOutcome::UnchangedComponent;
				Outcome.Message = LOCTEXT(
					"UnchangedComponent", "The existing whole-actor Convai component already matches.");
				++Result.UnchangedComponents;
				continue;
			}

			Actor->Modify();
			const FName OriginalComponentObjectName = Component->GetFName();
			SceneAutoTaggerApplyPrivate::ConfigureComponent(
				*Component,
				Request,
				NormalizedName,
				NormalizedDescription);
			Component = SceneAutoTaggerApplyPrivate::ResolveComponentAfterEditorReconstruction(
				*Actor, Component, OriginalComponentObjectName, NormalizedName);
			SceneAutoTaggerApplyPrivate::MarkActorAndLevelPackagesDirty(*Actor, Component);

			Outcome.Component = Component;
			Outcome.Outcome = ESceneAutoTaggerApplyOutcome::UpdatedComponent;
			Outcome.Message = LOCTEXT(
				"UpdatedComponent", "Updated the existing whole-actor Convai component.");
			++Result.UpdatedComponents;
			continue;
		}

		Actor->Modify();
		const FName ComponentName = MakeUniqueObjectName(
			Actor,
			UConvaiObjectComponent::StaticClass(),
			SceneAutoTaggerApplyPrivate::AutoCreatedComponentBaseName);
		Component = NewObject<UConvaiObjectComponent>(
			Actor, UConvaiObjectComponent::StaticClass(), ComponentName, RF_Transactional);
		if (!Component)
		{
			Outcome.Outcome = ESceneAutoTaggerApplyOutcome::ComponentCreationFailed;
			Outcome.Message = LOCTEXT(
				"ComponentCreationFailed", "Unreal could not create a Convai Object Component.");
			++Result.FailedRequests;
			continue;
		}

		// Match the engine's placed-instance component path: serialize it on the actor,
		// route creation, configure before registration, then register in the editor world.
		Actor->AddInstanceComponent(Component);
		Component->OnComponentCreated();
		SceneAutoTaggerApplyPrivate::ConfigureComponent(
			*Component,
			Request,
			NormalizedName,
			NormalizedDescription);
		Component->RegisterComponent();
		SceneAutoTaggerApplyPrivate::MarkActorAndLevelPackagesDirty(*Actor, Component);

		Outcome.Component = Component;
		Outcome.Outcome = ESceneAutoTaggerApplyOutcome::CreatedComponent;
		Outcome.Message = LOCTEXT(
			"CreatedComponent", "Added and configured a whole-actor Convai Object Component.");
		++Result.CreatedComponents;
	}

	if (bGenerateMovementPoints)
	{
		TArray<TWeakObjectPtr<UConvaiObjectComponent>> AppliedComponents;
		AppliedComponents.Reserve(Result.ActorOutcomes.Num());
		for (const FSceneAutoTaggerApplyActorOutcome& Outcome : Result.ActorOutcomes)
		{
			if (Outcome.WasApplied() && Outcome.Component.IsValid())
			{
				AppliedComponents.Add(Outcome.Component);
			}
		}

		const FConvaiMovementPointGenerationResult MovementResult =
			FConvaiMovementPointGenerator::Apply(
				AppliedComponents,
				EConvaiMovementPointGenerationOperation::Generate,
				// Metadata and generated points intentionally share this Apply undo step.
				false);
		Result.GeneratedMovementPointComponents = MovementResult.ChangedComponents;
		Result.GeneratedMovementPoints = MovementResult.GeneratedPoints;
		Result.MovementPointSkippedComponents = MovementResult.SkippedComponents;
		Result.MovementPointExistingComponents = MovementResult.ExistingPointComponents;
		Result.MovementPointMissingNavigationComponents =
			MovementResult.MissingNavigationComponents;
		Result.MovementPointNoValidCandidateComponents =
			MovementResult.NoValidCandidateComponents;
	}

	if (!Result.ChangedEditorState())
	{
		// Avoid leaving an empty undo record when every request was invalid or idempotent.
		Transaction.Cancel();
	}
	else if (GEditor)
	{
		GEditor->BroadcastLevelActorListChanged();
		GEditor->RedrawLevelEditingViewports();
	}

	return Result;
}

FSceneAutoTaggerRemoveResult FSceneAutoTaggerApply::RemoveComponents(
	const TArray<FSceneAutoTaggerRemoveRequest>& Requests)
{
	FSceneAutoTaggerRemoveResult Result;
	Result.ComponentOutcomes.Reserve(Requests.Num());
	if (Requests.IsEmpty())
	{
		return Result;
	}

	if (!IsInGameThread())
	{
		for (const FSceneAutoTaggerRemoveRequest& Request : Requests)
		{
			FSceneAutoTaggerRemoveComponentOutcome& Outcome =
				Result.ComponentOutcomes.AddDefaulted_GetRef();
			Outcome.Component = Request.Component;
			Outcome.Outcome = ESceneAutoTaggerRemoveOutcome::MustRunOnGameThread;
			Outcome.Message = LOCTEXT(
				"RemoveMustRunOnGameThread",
				"Scene metadata can only be removed from Unreal's game/editor thread.");
			++Result.SkippedComponents;
		}
		return Result;
	}

	struct FValidatedRemoval
	{
		TWeakObjectPtr<UConvaiObjectComponent> Component;
		TWeakObjectPtr<AActor> Actor;
		int32 OutcomeIndex = INDEX_NONE;
	};

	TArray<FValidatedRemoval> Validated;
	Validated.Reserve(Requests.Num());
	TSet<TWeakObjectPtr<UConvaiObjectComponent>> SeenComponents;
	for (const FSceneAutoTaggerRemoveRequest& Request : Requests)
	{
		FSceneAutoTaggerRemoveComponentOutcome& Outcome =
			Result.ComponentOutcomes.AddDefaulted_GetRef();
		Outcome.Component = Request.Component;
		UConvaiObjectComponent* Component = Request.Component.Get();
		if (!IsValid(Component) || SeenComponents.Contains(Component))
		{
			Outcome.Outcome = ESceneAutoTaggerRemoveOutcome::InvalidComponent;
			Outcome.Message = LOCTEXT(
				"InvalidRemoveComponent",
				"The selected component no longer exists or was selected more than once.");
			++Result.SkippedComponents;
			continue;
		}
		SeenComponents.Add(Component);

		AActor* Actor = Component->GetOwner();
		Outcome.Actor = Actor;
		Outcome.bWasAutoTaggerManaged = HasAutoTaggerOwnershipMarker(*Component);
		Outcome.bWasWholeActor = Component->ObjectEntry.ObjectReference
			== EConvaiObjectReference::WholeActor;
		Outcome.ObjectName = Component->ObjectEntry.Name;
		if (IsValid(Actor))
		{
			Outcome.ActorLabel = Actor->GetActorLabel();
			Outcome.ActorPath = Actor->GetPathName();
		}

		UWorld* World = IsValid(Actor) ? Actor->GetWorld() : nullptr;
		if (!IsValid(Actor) || Actor->IsTemplate() || !World
			|| World->WorldType != EWorldType::Editor || !Actor->GetLevel())
		{
			Outcome.Outcome = ESceneAutoTaggerRemoveOutcome::NonEditorActor;
			Outcome.Message = LOCTEXT(
				"RemoveNonEditorActor",
				"Only components on placed actors in an editor world can be removed.");
			++Result.SkippedComponents;
			continue;
		}

		if (!Outcome.bWasAutoTaggerManaged
			&& !Request.bAllowUnmarkedComponent)
		{
			Outcome.Outcome = ESceneAutoTaggerRemoveOutcome::OwnershipMarkerRequired;
			Outcome.Message = LOCTEXT(
				"OwnershipMarkerRequired",
				"This component is not marked as Auto Tagger-managed.");
			++Result.SkippedComponents;
			continue;
		}

		FText CannotDeleteReason;
		if (!CanRemoveComponent(*Component, CannotDeleteReason))
		{
			Outcome.Outcome = ESceneAutoTaggerRemoveOutcome::ProtectedComponent;
			Outcome.Message = CannotDeleteReason.IsEmpty()
				? LOCTEXT(
					"ProtectedRemoveComponent",
					"This component or its level is protected; unlock the level or edit the component at its source.")
				: CannotDeleteReason;
			++Result.SkippedComponents;
			continue;
		}

		Validated.Add({ Component, Actor, Result.ComponentOutcomes.Num() - 1 });
	}

	if (Validated.IsEmpty())
	{
		return Result;
	}

	const FText TransactionDescription = FText::Format(
		LOCTEXT("RemoveTransaction", "Remove {0} Convai scene object component(s)"),
		FText::AsNumber(Validated.Num()));
	FScopedTransaction Transaction(TransactionDescription);

	TArray<UActorComponent*> ComponentsToDelete;
	ComponentsToDelete.Reserve(Validated.Num());
	for (const FValidatedRemoval& Removal : Validated)
	{
		AActor* Actor = Removal.Actor.Get();
		UConvaiObjectComponent* Component = Removal.Component.Get();
		if (!IsValid(Actor) || !IsValid(Component))
		{
			continue;
		}
		Actor->Modify();
		Component->SetFlags(RF_Transactional);
		Component->Modify();
		SceneAutoTaggerApplyPrivate::MarkActorAndLevelPackagesDirty(
			*Actor,
			Component);
		ComponentsToDelete.Add(Component);
	}

	UTypedElementSelectionSet* SelectionSet = GEditor && GEditor->GetSelectedComponents()
		? GEditor->GetSelectedComponents()->GetElementSelectionSet()
		: nullptr;
	const bool bDeletionRan = GUnrealEd && SelectionSet
		&& GUnrealEd->DeleteComponents(ComponentsToDelete, SelectionSet, true);

	TSet<FString> ChangedActorPaths;
	for (const FValidatedRemoval& Removal : Validated)
	{
		FSceneAutoTaggerRemoveComponentOutcome& Outcome =
			Result.ComponentOutcomes[Removal.OutcomeIndex];
		AActor* Actor = Removal.Actor.Get();
		UConvaiObjectComponent* Component = Removal.Component.Get();
		const bool bRemoved = bDeletionRan
			&& (!IsValid(Component) || !IsValid(Actor) || !Actor->OwnsComponent(Component));
		if (bRemoved)
		{
			Outcome.Outcome = ESceneAutoTaggerRemoveOutcome::Removed;
			Outcome.Message = LOCTEXT("RemovedComponent", "Removed the selected component.");
			++Result.RemovedComponents;
			ChangedActorPaths.Add(Outcome.ActorPath);
		}
		else
		{
			Outcome.Outcome = ESceneAutoTaggerRemoveOutcome::ProtectedComponent;
			Outcome.Message = LOCTEXT(
				"RemoveChangedBeforeCommit",
				"The component changed before removal and was left in place.");
			++Result.SkippedComponents;
		}
	}
	Result.RemovedActors = ChangedActorPaths.Num();

	if (!Result.ChangedEditorState())
	{
		Transaction.Cancel();
	}
	else
	{
		if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
		{
			FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"))
				.BroadcastComponentsEdited();
		}
		if (GEditor)
		{
			GEditor->BroadcastLevelActorListChanged();
			GEditor->RedrawLevelEditingViewports();
		}
	}

	return Result;
}

#undef LOCTEXT_NAMESPACE
