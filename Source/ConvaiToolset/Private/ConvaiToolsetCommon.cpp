// Copyright Convai Inc. All Rights Reserved.

#include "ConvaiToolsetCommon.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"

namespace ConvaiToolsetCommon
{
	UBlueprint* LoadBlueprintByPath(const FString& BlueprintPath, FString& OutError)
	{
		if (BlueprintPath.IsEmpty())
		{
			OutError = TEXT("Blueprint path is empty.");
			return nullptr;
		}

		FString CleanPath = BlueprintPath;
		if (CleanPath.StartsWith(TEXT("/All/")))
		{
			CleanPath = CleanPath.Mid(4);
		}

		// Accept both "/Game/Foo/BP_Bar" and "/Game/Foo/BP_Bar.BP_Bar".
		UObject* Loaded = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *CleanPath);
		if (!Loaded)
		{
			// Try appending the object-name suffix if the caller passed a bare package path.
			const FString ShortName = FPackageName::GetShortName(CleanPath);
			const FString WithSuffix = CleanPath + TEXT(".") + ShortName;
			Loaded = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *WithSuffix);
		}

		UBlueprint* Blueprint = Cast<UBlueprint>(Loaded);
		if (!Blueprint)
		{
			OutError = FString::Printf(TEXT("Could not load a Blueprint at '%s'."), *BlueprintPath);
			return nullptr;
		}
		return Blueprint;
	}

	bool CompileAndSaveBlueprint(UBlueprint* Blueprint, bool bStructural, FString& OutError)
	{
		if (!Blueprint)
		{
			OutError = TEXT("Null Blueprint.");
			return false;
		}

		if (bStructural)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		Blueprint->MarkPackageDirty();

		if (UPackage* Package = Blueprint->GetOutermost())
		{
			const FString FileName = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;

			if (!UPackage::SavePackage(Package, Blueprint, *FileName, SaveArgs))
			{
				OutError = FString::Printf(TEXT("Compiled '%s' but the package save failed."), *Blueprint->GetName());
				return false;
			}
		}
		return true;
	}

	USCS_Node* FindSCSNodeOfClass(UBlueprint* Blueprint, UClass* ComponentClass)
	{
		if (!Blueprint || !Blueprint->SimpleConstructionScript || !ComponentClass)
		{
			return nullptr;
		}

		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf(ComponentClass))
			{
				return Node;
			}
		}
		return nullptr;
	}

	USCS_Node* AddSCSComponentNode(UBlueprint* Blueprint, UClass* ComponentClass, const FString& BaseName)
	{
		if (!Blueprint || !ComponentClass)
		{
			return nullptr;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			return nullptr;
		}

		SCS->Modify();
		USCS_Node* NewNode = SCS->CreateNode(ComponentClass, FName(*BaseName));
		if (!NewNode)
		{
			return nullptr;
		}

		// Attach under the default scene root when one exists; otherwise add as a top-level node.
		// (CreateNode + AddNode is the same flow the Convai content-menu uses for FloatingPawnMovement.)
		SCS->AddNode(NewNode);
		return NewNode;
	}

	void NotifyPropertyChanged(UObject* Object, FName PropertyName)
	{
		if (!Object || PropertyName.IsNone())
		{
			return;
		}

		FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			return;
		}

		Object->PreEditChange(Property);
		FPropertyChangedEvent Event(Property);
		Object->PostEditChangeProperty(Event);
	}

	int32 SetTemplatePropertyAndPropagate(UObject* Template, UObject* ActualArchetype, FName PropName, TFunctionRef<void()> MutateTemplate)
	{
		if (!Template || PropName.IsNone())
		{
			// Still let the caller mutate so behaviour degrades gracefully.
			MutateTemplate();
			return 0;
		}

		FProperty* Property = Template->GetClass()->FindPropertyByName(PropName);
		if (!Property)
		{
			MutateTemplate();
			return 0;
		}

		// If no separate archetype was supplied, the Template IS the archetype.
		if (!ActualArchetype)
		{
			ActualArchetype = Template;
		}

		// 1. CAPTURE the template's OLD value BEFORE mutating, into a temp buffer.
		void* OldValue = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(OldValue);
		Property->CopyCompleteValue(OldValue, Property->ContainerPtrToValuePtr<void>(Template));

		// 2. Gather affected archetype instances: those whose current value still equals the
		//    template's OLD value (preserving per-instance overrides). Gather BEFORE mutating.
		TArray<UObject*> Instances;
		ActualArchetype->GetArchetypeInstances(Instances);

		TArray<UObject*> Affected;
		Affected.Reserve(Instances.Num());
		for (UObject* Inst : Instances)
		{
			if (!Inst || Inst == Template)
			{
				continue;
			}
			const void* InstVal = Property->ContainerPtrToValuePtr<void>(Inst);
			if (Property->Identical(InstVal, OldValue))
			{
				Affected.Add(Inst);
			}
		}

		// 3. Mutate the template (writes the NEW value) with proper edit notifications.
		Template->Modify();
		Template->PreEditChange(Property);
		MutateTemplate();

		// 4. Capture the NEW value (post-mutation) so we can copy it onto affected instances.
		const void* NewValue = Property->ContainerPtrToValuePtr<void>(Template);

		// 5. Propagate to each affected instance, copying the template's NEW value.
		int32 Count = 0;
		for (UObject* Inst : Affected)
		{
			Inst->Modify();
			Inst->PreEditChange(Property);
			Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Inst), NewValue);
			FPropertyChangedEvent InstEvent(Property, EPropertyChangeType::ValueSet);
			Inst->PostEditChangeProperty(InstEvent);
			Inst->MarkPackageDirty();
			++Count;
		}

		// 6. Finalize the template edit.
		FPropertyChangedEvent TemplateEvent(Property, EPropertyChangeType::ValueSet);
		Template->PostEditChangeProperty(TemplateEvent);

		Property->DestroyValue(OldValue);
		FMemory::Free(OldValue);

		return Count;
	}

	int32 SetTemplatePropertyAndPropagate(UBlueprint* Blueprint, USCS_Node* Node, FName PropName, TFunctionRef<void()> MutateTemplate)
	{
		UObject* Template = Node ? Node->ComponentTemplate : nullptr;
		UObject* Archetype = Template;

		if (Blueprint && Node)
		{
			if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
			{
				if (UActorComponent* Actual = Node->GetActualComponentTemplate(BPGC))
				{
					Archetype = Actual;
				}
			}
		}

		return SetTemplatePropertyAndPropagate(Template, Archetype, PropName, MutateTemplate);
	}

	int32 ApplyToPlacedInstances(UBlueprint* Blueprint, TFunctionRef<void(AActor*)> Fixup)
	{
		if (!Blueprint || !Blueprint->GeneratedClass || !GEditor)
		{
			return 0;
		}

		int32 Count = 0;
		for (const FWorldContext& Context : GEditor->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::Editor)
			{
				continue;
			}
			UWorld* World = Context.World();
			if (!World)
			{
				continue;
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor && Actor->IsA(Blueprint->GeneratedClass))
				{
					Actor->Modify();
					Fixup(Actor);
					Actor->MarkPackageDirty();
					++Count;
				}
			}
		}
		return Count;
	}
}
