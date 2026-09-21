// Copyright 2025 Convai Inc. All Rights Reserved.

#include "Workspace/ConvaiAvatarBlueprintSetup.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiFaceSync.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"


namespace
{
	/**
	 * Where the Convai SDK's content lives, e.g. `/ConvAI/`.
	 *
	 * Asked of the plugin rather than hardcoded because the SDK's own toolset hardcodes `/Convai/`
	 * and disagrees with the `.uplugin` it ships beside; package names are case-insensitive so both
	 * happen to load, which is exactly how a mismatch survives unnoticed.
	 */
	FString ConvaiContentRoot()
	{
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ConvAI")))
		{
			return Plugin->GetMountedAssetPath();
		}
		return TEXT("/ConvAI/");
	}

	template <typename TBlueprint>
	UClass* LoadGeneratedClass(const FString& AssetPath)
	{
		if (const TBlueprint* Blueprint = Cast<TBlueprint>(StaticLoadObject(TBlueprint::StaticClass(), nullptr, *AssetPath)))
		{
			return Blueprint->GeneratedClass;
		}
		return LoadObject<UClass>(nullptr, *(AssetPath + TEXT("_C")));
	}

	bool SCSHasComponent(const USimpleConstructionScript* SCS, const UClass* ComponentClass)
	{
		if (!SCS)
		{
			return false;
		}
		for (const USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf(ComponentClass))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Whether the blueprint carries a component of this class, however it got there.
	 *
	 * Three places have to be looked at, and only the first is obvious: a component a C++ parent
	 * declares never appears in any SCS, and a creator childing one of Convai's sample blueprints
	 * inherits theirs from a parent blueprint's SCS. Missing either would re-add a component that is
	 * already inherited, and the duplicate is what a creator would then have to debug.
	 */
	bool HasComponent(const UBlueprint* Blueprint, const UClass* ComponentClass)
	{
		if (!Blueprint || !ComponentClass)
		{
			return false;
		}
		if (SCSHasComponent(Blueprint->SimpleConstructionScript, ComponentClass))
		{
			return true;
		}

		UClass* Parent = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetSuperClass() : Blueprint->ParentClass.Get();
		while (Parent)
		{
			const UBlueprintGeneratedClass* ParentBPGC = Cast<UBlueprintGeneratedClass>(Parent);
			if (!ParentBPGC)
			{
				if (const AActor* CDO = Cast<AActor>(Parent->GetDefaultObject()))
				{
					for (const UActorComponent* Component : CDO->GetComponents())
					{
						if (Component && Component->IsA(ComponentClass))
						{
							return true;
						}
					}
				}
				break;
			}
			if (SCSHasComponent(ParentBPGC->SimpleConstructionScript, ComponentClass))
			{
				return true;
			}
			Parent = Parent->GetSuperClass();
		}
		return false;
	}

	USCS_Node* AddComponentNode(UBlueprint* Blueprint, UClass* ComponentClass, const TCHAR* NodeName)
	{
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		SCS->Modify();

		USCS_Node* Node = SCS->CreateNode(ComponentClass, FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, NodeName));
		if (!Node)
		{
			return nullptr;
		}

		// Both Convai components are scene components (the chatbot derives from UAudioComponent), so
		// they have to go under the existing scene root. Added as a second root node instead,
		// AddNode's ValidateSceneRootNodes would promote ours to actor root, reparent the creator's
		// components under it and delete DefaultSceneRoot.
		//
		// GetSceneRootComponentTemplate never reports DefaultSceneRoot (SimpleConstructionScript.cpp,
		// the `RootNode != DefaultSceneRootNode` guard), so that case is picked up by hand.
		USCS_Node* RootNode = nullptr;
		SCS->GetSceneRootComponentTemplate(false, &RootNode);
		if (!RootNode && SCS->GetRootNodes().Contains(SCS->GetDefaultSceneRootNode()))
		{
			RootNode = SCS->GetDefaultSceneRootNode();
		}

		// The root node can belong to a parent blueprint's SCS - GetSceneRootComponentTemplate scans
		// the SCS stack parents-first and only excludes *this* SCS's DefaultSceneRoot. AddChildNode
		// files the node in whichever SCS owns the parent, so on an inherited root that would write
		// into a blueprint the creator never picked. Same guard the SCS editor uses
		// (SubobjectDataSubsystem.cpp, AttachSubobject); a root node attaches to the actor's root at
		// construction anyway, and SetParent just records the intent.
		if (RootNode && RootNode->GetSCS() == SCS)
		{
			RootNode->AddChildNode(Node);
		}
		else
		{
			SCS->AddNode(Node);
			if (RootNode)
			{
				Node->SetParent(RootNode);
			}
		}
		return Node;
	}

	/** Loaded on first need: a MetaHuman with only a body mesh must not fail over the face asset. */
	struct FLazyAnimClass
	{
		FString AssetPath;
		UClass* Class = nullptr;
		bool bTried = false;

		UClass* Get()
		{
			if (!bTried)
			{
				bTried = true;
				Class = LoadGeneratedClass<UAnimBlueprint>(AssetPath);
			}
			return Class;
		}
	};

	bool IsImporterFaceAnimation(const UClass* Class)
	{
		if (!Class) return false;
		const FString Path = Class->GetPathName();
		// UE5.8 BuildPipeline/BP_MetaHuman uses ABP_Face; legacy exports use Face_AnimBP.
		// Prepared copies retain the original /Game subtree under their permanent plugin mount.
		// Match the verified importer location as well as the class name so a custom same-named
		// animation somewhere else is preserved. Post-process and LiveLink classes are unrelated.
		return Path.Equals(TEXT("/MetaHumanCharacter/Face/ABP_Face.ABP_Face_C"), ESearchCase::CaseSensitive)
			|| Path.EndsWith(TEXT("/Game/MetaHumans/Common/Face/ABP_Face.ABP_Face_C"), ESearchCase::CaseSensitive)
			|| Path.EndsWith(TEXT("/Game/MetaHumans/Common/Face/Face_AnimBP.Face_AnimBP_C"), ESearchCase::CaseSensitive);
	}

	/**
	 * The Convai anim blueprint each MetaHuman mesh node wants, all resolved before any is assigned.
	 *
	 * Planning is separate from applying so a missing Convai animation asset refuses with the
	 * creator's blueprint untouched, like every other refusal here.
	 */
	FString MappedSetupPath(const FString& Path, const TMap<FName, FName>* CopiedPackages)
	{
		if (!CopiedPackages) return Path;
		const FString Package = FPackageName::ObjectPathToPackageName(Path);
		const FName* Mapped = CopiedPackages->Find(FName(*Package));
		if (!Mapped) return Path;
		const FString Candidate = Mapped->ToString() + Path.Mid(Package.Len());
		// A local pending journal records all planned destinations before copying.
		// Until a copy exists, setup must still be able to use its original SDK class.
		return FSoftObjectPath(Candidate).ResolveObject() || FPackageName::DoesPackageExist(Mapped->ToString()) ? Candidate : Path;
	}

	bool PlanMetaHumanAnimBlueprints(UBlueprint* Blueprint, const FString& ContentRoot,
		TArray<TPair<USCS_Node*, UClass*>>& OutWanted, FString& OutError, const TMap<FName, FName>* CopiedPackages = nullptr)
	{
		FLazyAnimClass BodyAnim{ MappedSetupPath(ContentRoot + TEXT("MetaHumans/Animations/Convai_MetaHuman_BodyAnim.Convai_MetaHuman_BodyAnim"), CopiedPackages) };
		FLazyAnimClass FaceAnim{ MappedSetupPath(ContentRoot + TEXT("MetaHumans/Animations/Convai_MetaHuman_FaceAnim.Convai_MetaHuman_FaceAnim"), CopiedPackages) };

		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			USkeletalMeshComponent* Mesh = Node ? Cast<USkeletalMeshComponent>(Node->ComponentTemplate) : nullptr;
			if (!Mesh)
			{
				continue;
			}

			const FString NodeName = Node->GetVariableName().ToString();
			UClass* const Existing = Mesh->AnimClass.Get();

			FLazyAnimClass* Wanted = nullptr;
			if (NodeName.Contains(TEXT("body")) && !Existing)
			{
				Wanted = &BodyAnim;
			}
			else if (NodeName.Contains(TEXT("face")) && (!Existing || IsImporterFaceAnimation(Existing)))
			{
				Wanted = &FaceAnim;
			}
			if (!Wanted)
			{
				continue;
			}

			UClass* AnimClass = Wanted->Get();
			if (!AnimClass)
			{
				OutError = FString::Printf(
					TEXT("'%s' is a MetaHuman, but the Convai animation blueprint '%s' could not be loaded. ")
					TEXT("Reinstall the Convai plugin content and pick the asset again."),
					*Blueprint->GetName(), *Wanted->AssetPath);
				return false;
			}

			OutWanted.Emplace(Node, AnimClass);
		}
		return true;
	}
}

namespace ConvaiAvatarStudio::BlueprintSetup
{
bool IsMetaHuman(const UBlueprint* Blueprint)
{
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return false;
	}

	for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
	{
		if (!Node || !Node->GetVariableName().ToString().Contains(TEXT("body")))
		{
			continue;
		}
		const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Node->ComponentTemplate);
		const USkeletalMesh* MeshAsset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
		const USkeleton* Skeleton = MeshAsset ? MeshAsset->GetSkeleton() : nullptr;
		if (Skeleton && Skeleton->GetPathName().Contains(TEXT("metahuman")))
		{
			return true;
		}
	}
	return false;
}

bool GetSetupDependencies(UBlueprint* Blueprint, bool bIsMetaHuman, TArray<FName>& OutPackages, FString& OutError)
{
	OutPackages.Reset(); OutError.Reset();
	if (!Blueprint || !Blueprint->SimpleConstructionScript) { OutError = TEXT("Choose an Actor Blueprint before checking its Convai setup."); return false; }
	const FString ContentRoot = ConvaiContentRoot();
	UClass* ChatbotClass = LoadGeneratedClass<UBlueprint>(ContentRoot + TEXT("ConvaiConveniencePack/ConvaiBPComponent/BP_ConvaiChatbotComponent.BP_ConvaiChatbotComponent"));
	if (!ChatbotClass) { OutError = TEXT("The Convai Chatbot Blueprint component could not be loaded. Reinstall the Convai plugin content before uploading."); return false; }
	if (!HasComponent(Blueprint, ChatbotClass)) OutPackages.AddUnique(ChatbotClass->GetOutermost()->GetFName());
	TArray<TPair<USCS_Node*, UClass*>> AnimAssignments;
	if (bIsMetaHuman && !PlanMetaHumanAnimBlueprints(Blueprint, ContentRoot, AnimAssignments, OutError)) return false;
	for (const auto& Assignment : AnimAssignments) OutPackages.AddUnique(Assignment.Value->GetOutermost()->GetFName());
	return true;
}

bool PrepareAvatarBlueprint(UBlueprint* Blueprint, bool bIsMetaHuman, FString& OutError, TArray<FString>& OutChanges,
	const TMap<FName, FName>* CopiedPackages)
{
	OutError.Reset();
	OutChanges.Reset();
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		OutError = TEXT("The selected asset is not an Actor blueprint, so Convai's components cannot be added to it.");
		return false;
	}

	const FString ContentRoot = ConvaiContentRoot();
	const FString OriginalChatbotPath = ContentRoot + TEXT("ConvaiConveniencePack/ConvaiBPComponent/BP_ConvaiChatbotComponent.BP_ConvaiChatbotComponent");
	const FString ChatbotPath = MappedSetupPath(OriginalChatbotPath, CopiedPackages);
	UClass* ChatbotClass = LoadGeneratedClass<UBlueprint>(ChatbotPath);
	if (!ChatbotClass)
	{
		OutError = FString::Printf(
			TEXT("BP_ConvaiChatbotComponent could not be loaded from '%s'. The Convai plugin content is missing or not mounted."),
			*ContentRoot);
		return false;
	}
	// During an opt-in update the existing component may still reference the original;
	// after remapping it is a separate Blueprint class with the same native parent.
	const UClass* OriginalChatbotClass = ChatbotPath == OriginalChatbotPath ? ChatbotClass : LoadGeneratedClass<UBlueprint>(OriginalChatbotPath);
	const bool bNeedsChatbot = !HasComponent(Blueprint, ChatbotClass) && !HasComponent(Blueprint, OriginalChatbotClass);
	if (bNeedsChatbot && HasComponent(Blueprint, UConvaiChatbotComponent::StaticClass()))
	{
		OutError = FString::Printf(
			TEXT("%s uses the C++ Convai Chatbot Component. Open this Blueprint (or the parent that supplies the component), ")
			TEXT("transfer its settings and event connections to BP_ConvaiChatbotComponent, then replace the C++ component and retry. ")
			TEXT("The Blueprint component supplies the action and movement behavior uploaded avatars need. No components were changed."),
			*Blueprint->GetName());
		return false;
	}

	TArray<TPair<USCS_Node*, UClass*>> AnimAssignments;
	if (bIsMetaHuman && !PlanMetaHumanAnimBlueprints(Blueprint, ContentRoot, AnimAssignments, OutError, CopiedPackages))
	{
		return false;
	}

	if (bNeedsChatbot)
	{
		if (!AddComponentNode(Blueprint, ChatbotClass, TEXT("BP_ConvaiChatbotComponent")))
		{
			OutError = FString::Printf(TEXT("Could not add BP_ConvaiChatbotComponent to %s."), *Blueprint->GetName());
			return false;
		}
		OutChanges.Add(TEXT("added BP_ConvaiChatbotComponent"));
	}

	if (!HasComponent(Blueprint, UConvaiFaceSyncComponent::StaticClass()))
	{
		if (!AddComponentNode(Blueprint, UConvaiFaceSyncComponent::StaticClass(), TEXT("ConvaiFaceSync")))
		{
			OutError = FString::Printf(TEXT("Could not add ConvaiFaceSyncComponent to %s."), *Blueprint->GetName());
			return false;
		}
		OutChanges.Add(TEXT("added ConvaiFaceSyncComponent"));
	}

	for (const TPair<USCS_Node*, UClass*>& Assignment : AnimAssignments)
	{
		USkeletalMeshComponent* Mesh = CastChecked<USkeletalMeshComponent>(Assignment.Key->ComponentTemplate);
		Mesh->Modify();
		Mesh->SetAnimInstanceClass(Assignment.Value);
		OutChanges.Add(FString::Printf(TEXT("assigned %s to '%s'"),
			*Assignment.Value->GetName(), *Assignment.Key->GetVariableName().ToString()));
	}

	if (OutChanges.Num() > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		// The caller saves only after the complete prepared dependency closure succeeds.
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
		Blueprint->MarkPackageDirty();

		UE_LOG(LogTemp, Log, TEXT("Prepared Avatar blueprint '%s': %s."),
			*Blueprint->GetName(), *FString::Join(OutChanges, TEXT(", ")));
	}
	return true;
}
}
