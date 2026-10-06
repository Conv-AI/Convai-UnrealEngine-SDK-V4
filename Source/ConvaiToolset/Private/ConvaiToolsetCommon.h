// Copyright Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class USCS_Node;
class AActor;

/**
 * Small shared helpers used by the Convai toolset .cpp files. These intentionally
 * REPLICATE (rather than depend on) the Private helpers in ConvaiEditor so we don't
 * have to widen ConvaiEditor's public API. The pawn-movement logic in particular is
 * a faithful re-implementation of FConvaiContentBrowserContextMenu's defaults.
 */
namespace ConvaiToolsetCommon
{
	/** Loads a UBlueprint from an object path like "/Game/Foo/BP_Bar".
	 *  Strips an optional "/All" prefix and a trailing ".BP_Bar" object suffix. */
	UBlueprint* LoadBlueprintByPath(const FString& BlueprintPath, FString& OutError);

	/** Compiles the Blueprint and saves its package to disk. Returns false (with OutError set)
	 *  if the package save failed; compilation itself is best-effort. */
	bool CompileAndSaveBlueprint(UBlueprint* Blueprint, bool bStructural, FString& OutError);

	/** Finds the first SCS node on this Blueprint whose component template is (a subclass of)
	 *  ComponentClass. Returns nullptr if none. Only searches THIS Blueprint's own SCS. */
	USCS_Node* FindSCSNodeOfClass(UBlueprint* Blueprint, UClass* ComponentClass);

	/** Adds a new SCS node for ComponentClass with the given (unique) base name, attached at the
	 *  root. Returns the created node, or nullptr on failure. Marks the SCS modified. */
	USCS_Node* AddSCSComponentNode(UBlueprint* Blueprint, UClass* ComponentClass, const FString& BaseName);

	/** Notify the editor/archetype machinery that a UPROPERTY on this object changed, so CDO
	 *  archetype-delta tracking captures struct edits (mirrors ConvaiEditor's NotifyPropertyChanged). */
	void NotifyPropertyChanged(UObject* Object, FName PropertyName);

	/** Runs Fixup on every already-placed actor of Blueprint's generated class across all open
	 *  EDITOR worlds, wrapping each in Modify()/MarkPackageDirty(). Changing a BP/component-template
	 *  default does NOT auto-propagate to pre-existing instances (only newly-added components appear),
	 *  so setup tools call this to push the same values onto live level instances. Returns the count. */
	int32 ApplyToPlacedInstances(UBlueprint* Blueprint, TFunctionRef<void(AActor*)> Fixup);

	/**
	 * GENERAL archetype-property propagation. Edits a single UPROPERTY on a component template
	 * AND pushes the new value onto every already-placed archetype instance that still holds the
	 * template's OLD value (preserving per-instance overrides).
	 *
	 * Flow (verified vs UE 5.8 source): capture the template's OLD value for PropName BEFORE
	 * mutating -> gather GetArchetypeInstances() of ActualArchetype -> run MutateTemplate() (which
	 * sets the new value on the Template) -> for each instance whose current value is Identical to
	 * the captured OLD value, Modify()/PreEditChange/CopyCompleteValue(new)/PostEditChangeProperty.
	 * The template itself gets a matching PreEditChange/PostEditChangeProperty so editor/archetype
	 * delta tracking captures the edit.
	 *
	 * @param Template         The object whose property is being edited (usually USCS_Node::ComponentTemplate
	 *                         or the actual SCS archetype). MutateTemplate must write the new value here.
	 * @param ActualArchetype  The object to enumerate archetype instances from. For an SCS component this
	 *                         is USCS_Node::GetActualComponentTemplate(BPGC); pass the same as Template when
	 *                         Template already IS the archetype (e.g. a CDO subobject).
	 * @param PropName         The UPROPERTY name on Template's class to capture/compare/propagate.
	 * @param MutateTemplate   Callback that applies the new value to Template (and only Template).
	 * @return number of placed instances that were updated.
	 *
	 * IMPORTANT: call this BEFORE FKismetEditorUtilities::CompileBlueprint and do not reuse the passed
	 * pointers after a compile (reinstancing invalidates them).
	 */
	int32 SetTemplatePropertyAndPropagate(UObject* Template, UObject* ActualArchetype, FName PropName, TFunctionRef<void()> MutateTemplate);

	/** Convenience overload: derives the actual archetype from a USCS_Node + owning Blueprint via
	 *  USCS_Node::GetActualComponentTemplate(Blueprint->GeneratedClass), then forwards to the above.
	 *  Falls back to Node->ComponentTemplate as both Template and Archetype if the actual archetype
	 *  can't be resolved. */
	int32 SetTemplatePropertyAndPropagate(UBlueprint* Blueprint, USCS_Node* Node, FName PropName, TFunctionRef<void()> MutateTemplate);
}
