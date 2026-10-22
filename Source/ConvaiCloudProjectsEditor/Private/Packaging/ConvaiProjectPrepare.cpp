// Copyright Convai Inc. All Rights Reserved.
#include "Packaging/ConvaiProjectPrepare.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "ConvaiPlayerComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ProjectDescriptor.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "ConvaiCloudProjects"

namespace
{
const TCHAR* PixelStreamingPlugin = TEXT("PixelStreaming");
/**
 * The SDK's own bridge: a UPixelStreamingAudioComponent that also implements
 * IConvaiAudioCaptureInterface, so UConvaiPlayerComponent adopts it as the microphone and
 * routes it to /ConvAI/Submixes/AudioInput. The plain engine component is not enough --
 * the player component never discovers it and falls back to the host's (absent) mic.
 * Resolved by path so this module links neither ConvaiPSAudioCapture nor PixelStreaming.
 */
const TCHAR* StreamingAudioComponentClass = TEXT("/Script/ConvaiPSAudioCapture.ConvaiPSAudioCaptureComponent");
/** The convenience Blueprint component; projects add this far more often than the native class. */
const TCHAR* ConvaiPlayerComponentAsset = TEXT("/ConvAI/ConvaiConveniencePack/ConvaiBPComponent/BP_ConvaiPlayerComponent");

UClass* LoadClassByPath(const TCHAR* Path)
{
	return LoadObject<UClass>(nullptr, Path);
}

/** True for a class from one of the C++ modules the .uproject lists, as opposed to the engine or a plugin. */
bool IsProjectModuleClass(const UClass* Class)
{
	const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
	const FName Module = FPackageName::GetShortFName(Class->GetOutermost()->GetFName());
	return Project && Project->Modules.ContainsByPredicate([Module](const FModuleDescriptor& Each) { return Each.Name == Module; });
}

bool SaveBlueprint(UBlueprint* Blueprint, FString& OutError)
{
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	Blueprint->MarkPackageDirty();

	UPackage* Package = Blueprint->GetOutermost();
	if (!Package) return true;

	const FString FileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Blueprint, *FileName, SaveArgs))
	{
		OutError = FString::Printf(TEXT("'%s' was changed but its package could not be saved."), *Blueprint->GetName());
		return false;
	}
	return true;
}
}

FString FConvaiProjectPrepare::GetProjectName()
{
	return FApp::GetProjectName();
}

bool FConvaiProjectPrepare::IsPixelStreamingEnabled()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PixelStreamingPlugin);
	return Plugin.IsValid() && Plugin->IsEnabled();
}

FConvaiProjectPrepareReport FConvaiProjectPrepare::Run()
{
	check(IsInGameThread());
	FConvaiProjectPrepareReport Report;
	if (!EnablePixelStreaming(Report)) return Report;
	AddStreamingAudioComponent(Report);
	return Report;
}

bool FConvaiProjectPrepare::EnablePixelStreaming(FConvaiProjectPrepareReport& Report)
{
	if (IsPixelStreamingEnabled()) return true;

	if (!IPluginManager::Get().FindPlugin(PixelStreamingPlugin).IsValid())
	{
		Report.Error = TEXT("The Pixel Streaming plugin is not installed with this engine. Install it, then upload again.");
		return false;
	}

	FText FailReason;
	if (!IProjectManager::Get().SetPluginEnabled(PixelStreamingPlugin, true, FailReason)
		|| !IProjectManager::Get().SaveCurrentProjectToDisk(FailReason))
	{
		Report.Error = FString::Printf(TEXT("Pixel Streaming could not be enabled: %s"), *FailReason.ToString());
		return false;
	}

	// Newly enabled modules are compiled into the packaged build by UAT, but this editor session
	// is still running without them. Say so plainly rather than pretending the change is live.
	Report.Changes.Add(TEXT("Enabled the Pixel Streaming plugin for this project."));
	Report.Warnings.Add(TEXT("Pixel Streaming was just enabled. The packaged build includes it; restart the editor before testing it in the editor."));
	return true;
}

TArray<UBlueprint*> FConvaiProjectPrepare::FindBlueprintsWithConvaiPlayer(const TArray<UClass*>& NativePlayerClasses)
{
	TArray<UBlueprint*> Found;
	UClass* PlayerClass = UConvaiPlayerComponent::StaticClass();
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Without it, every GetDerivedClassNames below rebuilds the registry's whole class tree.
	const bool bWasCaching = AssetRegistry.GetTemporaryCachingMode();
	AssetRegistry.SetTemporaryCachingMode(true);
	ON_SCOPE_EXIT { AssetRegistry.SetTemporaryCachingMode(bWasCaching); };

	auto Consider = [&Found, PlayerClass](const FAssetData& Asset)
	{
		UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
		if (FindComponentSource(Blueprint, PlayerClass) != EComponentSource::None) Found.AddUnique(Blueprint);
	};
	// Read from the registry's class data, so finding a class's project descendants loads none of them.
	auto ProjectDescendants = [&AssetRegistry](const FTopLevelAssetPath& ClassPath)
	{
		TSet<FTopLevelAssetPath> Derived;
		AssetRegistry.GetDerivedClassNames({ ClassPath }, {}, Derived);
		TArray<FAssetData> Descendants;
		for (const FTopLevelAssetPath& Each : Derived)
		{
			TArray<FAssetData> Assets;
			AssetRegistry.GetAssetsByPackageName(Each.GetPackageName(), Assets);
			Descendants.Append(Assets.FilterByPredicate(&FConvaiProjectPrepare::IsProjectBlueprint));
		}
		return Descendants;
	};

	// Scanning every Blueprint in a project means loading every Blueprint in a project. The
	// asset registry already knows which packages reference the Convai module and the convenience
	// component, so only the Blueprints among those load, plus the project's children of a player.
	TSet<FName> Candidates;
	for (const TCHAR* Root : { TEXT("/Script/Convai"), ConvaiPlayerComponentAsset })
	{
		TArray<FName> Referencers;
		AssetRegistry.GetReferencers(FName(Root), Referencers);
		Candidates.Append(Referencers);
	}

	for (const FName& PackageName : Candidates)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(PackageName, Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (IsProjectBlueprint(Asset))
			{
				Consider(Asset);
				continue;
			}

			// A project child of an SDK player Blueprint inherits the player without referencing Convai itself.
			// The SDK Blueprint is only read, and it loads only when the project has such a child.
			FString GeneratedClass;
			if (Asset.AssetClassPath != UBlueprint::StaticClass()->GetClassPathName()
				|| !Asset.GetTagValue(FBlueprintTags::GeneratedClassPath, GeneratedClass)) continue;
			const TArray<FAssetData> Children =
				ProjectDescendants(FTopLevelAssetPath(FPackageName::ExportTextPathToObjectPath(GeneratedClass)));
			if (Children.IsEmpty()
				|| FindComponentSource(Cast<UBlueprint>(Asset.GetAsset()), PlayerClass) == EComponentSource::None) continue;
			for (const FAssetData& Child : Children) Consider(Child);
		}
	}

	// The same holds for a project child of a C++ class whose constructor creates the player.
	for (const UClass* Native : NativePlayerClasses)
	{
		for (const FAssetData& Child : ProjectDescendants(Native->GetClassPathName())) Consider(Child);
	}
	return Found;
}

FConvaiProjectPrepare::EComponentSource FConvaiProjectPrepare::FindComponentSource(const UBlueprint* Blueprint, UClass* ComponentClass)
{
	if (!Blueprint || !ComponentClass) return EComponentSource::None;

	auto ScriptHas = [ComponentClass](const UBlueprint* Owner)
	{
		if (!Owner->SimpleConstructionScript) return false;
		for (const USCS_Node* Node : Owner->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf(ComponentClass)) return true;
		}
		return false;
	};
	if (ScriptHas(Blueprint)) return EComponentSource::OwnScript;

	// An inherited node stays in the construction script of the ancestor that added it.
	for (const UBlueprint* Ancestor = UBlueprint::GetBlueprintFromClass(Blueprint->ParentClass); Ancestor;
		Ancestor = UBlueprint::GetBlueprintFromClass(Ancestor->ParentClass))
	{
		if (ScriptHas(Ancestor)) return EComponentSource::ParentScript;
	}

	// Native default subobjects register with their owner, so the class defaults list them.
	const AActor* Defaults = Blueprint->GeneratedClass ? Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
	return Defaults && Defaults->FindComponentByClass(ComponentClass) ? EComponentSource::Native : EComponentSource::None;
}

bool FConvaiProjectPrepare::IsNativeHolder(const UClass* Class, UClass* ComponentClass)
{
	// A class Live Coding replaced stays loaded, flagged, beside its successor, which would find and warn about it twice.
	if (!Class->HasAnyClassFlags(CLASS_Native) || Class->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated)
		|| !Class->IsChildOf<AActor>()) return false;
	const AActor* Defaults = Cast<AActor>(Class->GetDefaultObject(false));
	return Defaults && Defaults->FindComponentByClass(ComponentClass);
}

UBlueprint* FConvaiProjectPrepare::FindCaptureOwner(UBlueprint* Blueprint, const TArray<UBlueprint*>& Targets,
	UClass* PlayerClass, UClass* CaptureClass)
{
	// One more capture above a target that has its own would give that target's actors two.
	auto IsAboveOwnCapture = [&Targets, CaptureClass](const UBlueprint* Candidate)
	{
		return Targets.ContainsByPredicate([Candidate, CaptureClass](const UBlueprint* Target)
		{
			return Target != Candidate && Target->GeneratedClass && Target->GeneratedClass->IsChildOf(Candidate->GeneratedClass)
				&& FindComponentSource(Target, CaptureClass) == EComponentSource::OwnScript;
		});
	};
	if (IsAboveOwnCapture(Blueprint)) return nullptr;

	// The highest project Blueprint in the chain that sees the player takes it; its children inherit it.
	UBlueprint* Owner = Blueprint;
	for (UBlueprint* Ancestor = UBlueprint::GetBlueprintFromClass(Blueprint->ParentClass); Ancestor;
		Ancestor = UBlueprint::GetBlueprintFromClass(Ancestor->ParentClass))
	{
		if (!IsProjectBlueprint(FAssetData(Ancestor, FAssetData::ECreationFlags::SkipAssetRegistryTagsGathering))
			|| FindComponentSource(Ancestor, PlayerClass) == EComponentSource::None) continue;
		// Every Blueprint higher up is above that target too.
		if (IsAboveOwnCapture(Ancestor)) break;
		Owner = Ancestor;
	}
	return Owner;
}

bool FConvaiProjectPrepare::IsProjectBlueprint(const FAssetData& Asset)
{
	// Only the project's own content. Referencers include the SDK's sample Blueprints
	// (/ConvAI/Core/ConvaiBasePlayer and friends), and editing those would modify the
	// plugin in place -- changes that are untracked, and lost on the next SDK update.
	// Maps, animation and widget Blueprints and external actors reference the Convai module too;
	// their registry class rules them out, so none of them is loaded only to be discarded.
	return Asset.PackageName.ToString().StartsWith(TEXT("/Game/"))
		&& Asset.AssetClassPath == UBlueprint::StaticClass()->GetClassPathName();
}

void FConvaiProjectPrepare::AddStreamingAudioComponent(FConvaiProjectPrepareReport& Report)
{
	UClass* ComponentClass = LoadClassByPath(StreamingAudioComponentClass);
	if (!ComponentClass)
	{
		// Expected on the pass that first enables the plugin: its module is not loaded yet.
		Report.Warnings.Add(TEXT("The Pixel Streaming audio component is not available in this editor session. Restart the editor and upload again to add it to the player pawn."));
		return;
	}

	UClass* PlayerClass = UConvaiPlayerComponent::StaticClass();
	TArray<UClass*> NativePlayers;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (IsNativeHolder(*It, PlayerClass)) NativePlayers.Add(*It);
	}
	const TArray<UBlueprint*> Targets = FindBlueprintsWithConvaiPlayer(NativePlayers);

	// C++ cannot be edited from here. A project class that creates the player, with no project
	// Blueprint child to carry the capture, is named instead of silently streaming without a mic.
	bool bProjectNativePlayer = false;
	for (UClass* Native : NativePlayers)
	{
		// An abstract base is searched above for its Blueprint children, but only its concrete subclasses are warned about.
		if (Native->HasAnyClassFlags(CLASS_Abstract) || !IsProjectModuleClass(Native)) continue;
		bProjectNativePlayer = true;

		const bool bHasProjectChild = Targets.ContainsByPredicate([Native](const UBlueprint* Blueprint)
		{
			return Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(Native);
		});
		const AActor* Defaults = Cast<AActor>(Native->GetDefaultObject());
		if (bHasProjectChild || (Defaults && Defaults->FindComponentByClass(ComponentClass))) continue;
		Report.Warnings.Add(FString::Printf(
			TEXT("'%s%s' creates its Convai player component in C++, which cannot be edited automatically, so it has no streaming microphone. Add a UConvaiPSAudioCaptureComponent to it in C++."),
			Native->GetPrefixCPP(), *Native->GetName()));
	}

	if (Targets.IsEmpty() && !bProjectNativePlayer)
	{
		// A project that never talks to a Convai character still packages and streams fine.
		// There is simply no microphone to wire up, so this is a note and not a warning.
		Report.Changes.Add(TEXT("No Convai player component found, so no streaming microphone was added."));
		return;
	}

	for (UBlueprint* Blueprint : Targets)
	{
		// A capture anywhere up the chain is inherited, so a second one is never added beside it.
		if (FindComponentSource(Blueprint, ComponentClass) != EComponentSource::None) continue;

		UBlueprint* Owner = FindCaptureOwner(Blueprint, Targets, PlayerClass, ComponentClass);
		if (!Owner)
		{
			Report.Warnings.Add(FString::Printf(
				TEXT("No streaming microphone was added to '%s': a Blueprint below it has its own, and one here would give that Blueprint two."),
				*Blueprint->GetName()));
			continue;
		}

		USimpleConstructionScript* SCS = Owner->SimpleConstructionScript;
		if (!SCS) continue;

		SCS->Modify();
		USCS_Node* NewNode = SCS->CreateNode(ComponentClass, TEXT("ConvaiPSAudioCapture"));
		if (!NewNode)
		{
			Report.Warnings.Add(FString::Printf(
				TEXT("The streaming microphone could not be added to '%s'."), *Owner->GetName()));
			continue;
		}
		SCS->AddNode(NewNode);

		FString SaveError;
		if (!SaveBlueprint(Owner, SaveError))
		{
			Report.Warnings.Add(SaveError);
			continue;
		}
		Report.Changes.Add(FString::Printf(
			TEXT("Added the Convai Pixel Streaming audio capture to '%s'."), *Owner->GetName()));
	}
}

#undef LOCTEXT_NAMESPACE
