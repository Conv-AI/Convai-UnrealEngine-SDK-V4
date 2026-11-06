// Copyright Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UConvaiObjectComponent;

/** One reviewed scene-object proposal that should be persisted onto a placed actor. */
struct FSceneAutoTaggerApplyRequest
{
	/** The editor-world actor that was reviewed and accepted. */
	TWeakObjectPtr<AActor> Actor;

	/** Speakable, AI-facing object identity written to FConvaiObjectEntry::Name. */
	FString Name;

	/** Plain-language object description written to FConvaiObjectEntry::Description. */
	FString Description;

	/** Merge same-named, merge-enabled Convai objects into one logical scene object. */
	bool bMergeDuplicates = false;

	/** Stable identity for this verified duplicate set. Ignored when merging is disabled. */
	int32 MergeGroupIndex = 0;

	/** Include this object in Convai's spatial-awareness context. */
	bool bIncludeInSpatialAwareness = true;

	/** Allow the Convai player gaze pipeline to focus this object. */
	bool bGazeable = true;

	/**
	 * Update an existing whole-actor Convai Object Component when possible. When false,
	 * a new whole-actor component is added. Component-scoped Convai entries are never
	 * selected by this option and are always preserved.
	 */
	bool bUpdateExistingWholeActorComponent = true;
};

enum class ESceneAutoTaggerApplyOutcome : uint8
{
	CreatedComponent,
	UpdatedComponent,
	UnchangedComponent,
	InvalidActor,
	NonEditorActor,
	EmptyObjectName,
	ManualComponentConflict,
	ComponentCreationFailed,
	MustRunOnGameThread,
};

/** Result for one request. There is always exactly one outcome per input request. */
struct FSceneAutoTaggerApplyActorOutcome
{
	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<UConvaiObjectComponent> Component;
	ESceneAutoTaggerApplyOutcome Outcome = ESceneAutoTaggerApplyOutcome::InvalidActor;
	FString ActorLabel;
	FString ActorPath;
	FText Message;
	int32 PreservedComponentScopedEntries = 0;
	bool WasApplied() const
	{
		return Outcome == ESceneAutoTaggerApplyOutcome::CreatedComponent
			|| Outcome == ESceneAutoTaggerApplyOutcome::UpdatedComponent
			|| Outcome == ESceneAutoTaggerApplyOutcome::UnchangedComponent;
	}

	bool ChangedEditorState() const
	{
		return Outcome == ESceneAutoTaggerApplyOutcome::CreatedComponent
			|| Outcome == ESceneAutoTaggerApplyOutcome::UpdatedComponent;
	}
};

/** Aggregate result for a single transactional batch. */
struct FSceneAutoTaggerApplyResult
{
	TArray<FSceneAutoTaggerApplyActorOutcome> ActorOutcomes;
	int32 CreatedComponents = 0;
	int32 UpdatedComponents = 0;
	int32 UnchangedComponents = 0;
	int32 FailedRequests = 0;
	/** Best-effort movement-point authoring requested at the same Apply boundary. */
	bool bMovementPointGenerationRequested = false;
	int32 GeneratedMovementPointComponents = 0;
	int32 GeneratedMovementPoints = 0;
	int32 MovementPointSkippedComponents = 0;
	int32 MovementPointExistingComponents = 0;
	int32 MovementPointMissingNavigationComponents = 0;
	int32 MovementPointNoValidCandidateComponents = 0;

	bool ChangedEditorState() const
	{
		return CreatedComponents > 0 || UpdatedComponents > 0
			|| GeneratedMovementPointComponents > 0;
	}
};

/** One explicitly selected Convai Object Component to remove from a placed actor. */
struct FSceneAutoTaggerRemoveRequest
{
	TWeakObjectPtr<UConvaiObjectComponent> Component;

	/**
	 * False restricts removal to the exact Auto Tagger ownership marker. The UI
	 * sets this only after an unmarked component was deliberately revealed,
	 * checked, and confirmed by the user.
	 */
	bool bAllowUnmarkedComponent = false;
};

enum class ESceneAutoTaggerRemoveOutcome : uint8
{
	Removed,
	InvalidComponent,
	NonEditorActor,
	OwnershipMarkerRequired,
	ProtectedComponent,
	MustRunOnGameThread,
};

struct FSceneAutoTaggerRemoveComponentOutcome
{
	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<UConvaiObjectComponent> Component;
	bool bWasAutoTaggerManaged = false;
	bool bWasWholeActor = false;
	ESceneAutoTaggerRemoveOutcome Outcome = ESceneAutoTaggerRemoveOutcome::InvalidComponent;
	FString ActorLabel;
	FString ActorPath;
	FString ObjectName;
	FText Message;
};

struct FSceneAutoTaggerRemoveResult
{
	TArray<FSceneAutoTaggerRemoveComponentOutcome> ComponentOutcomes;
	int32 RemovedComponents = 0;
	int32 RemovedActors = 0;
	int32 SkippedComponents = 0;

	bool ChangedEditorState() const { return RemovedComponents > 0; }
};

/**
 * Persists reviewed scene metadata as instance-owned UConvaiObjectComponents.
 *
 * This intentionally targets placed actors rather than Blueprint templates: vision results
 * describe a particular scene instance, and adding an instance component avoids silently
 * changing every instance of a Blueprint class. All mutations in a batch share one undo step.
 */
class FSceneAutoTaggerApply final
{
public:
	static FSceneAutoTaggerApplyResult ApplyAcceptedRequests(
		const TArray<FSceneAutoTaggerApplyRequest>& Requests,
		bool bGenerateMovementPoints = false);

	/** Removes explicitly selected instance components in one undoable transaction. */
	static FSceneAutoTaggerRemoveResult RemoveComponents(
		const TArray<FSceneAutoTaggerRemoveRequest>& Requests);

	/** Exact, deletion-safe provenance check (unlike the broad legacy tag-prefix matcher). */
	static bool HasAutoTaggerOwnershipMarker(const UConvaiObjectComponent& Component);

	/** True only for an instance-owned component the editor can safely delete. */
	static bool IsRemovableInstanceComponent(const UConvaiObjectComponent& Component);

	/** Full editor deletion guard shared by the manager and transactional backend. */
	static bool CanRemoveComponent(const UConvaiObjectComponent& Component, FText& OutReason);

	/** Prefix reserved for ownership metadata and cleanup of legacy Auto Tagger tags. */
	static const FString& GetManagedComponentTagPrefix();

	/** True for the ownership marker and legacy Auto Tagger ComponentTags. */
	static bool IsManagedComponentTag(FName Tag);
};
