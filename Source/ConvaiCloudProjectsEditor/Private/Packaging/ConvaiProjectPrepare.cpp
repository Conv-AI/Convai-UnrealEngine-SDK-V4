// Copyright Convai Inc. All Rights Reserved.
#include "Packaging/ConvaiProjectPrepare.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ConvaiPlayerComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

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

TArray<UBlueprint*> FConvaiProjectPrepare::FindBlueprintsWithConvaiPlayer()
{
	TArray<UBlueprint*> Found;

	// Scanning every Blueprint in a project means loading every Blueprint in a project. The
	// asset registry already knows which packages reference the Convai module and the convenience
	// component, and nothing else can be holding a Convai player component, so only those load.
	const IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

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
			UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
			if (!Blueprint || !Blueprint->SimpleConstructionScript) continue;

			for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf(UConvaiPlayerComponent::StaticClass()))
				{
					Found.AddUnique(Blueprint);
					break;
				}
			}
		}
	}
	return Found;
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

	const TArray<UBlueprint*> Targets = FindBlueprintsWithConvaiPlayer();
	if (Targets.IsEmpty())
	{
		// A project that never talks to a Convai character still packages and streams fine.
		// There is simply no microphone to wire up, so this is a note and not a warning.
		Report.Changes.Add(TEXT("No Convai player component found, so no streaming microphone was added."));
		return;
	}

	for (UBlueprint* Blueprint : Targets)
	{
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS) continue;

		bool bAlreadyPresent = false;
		for (const USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf(ComponentClass))
			{
				bAlreadyPresent = true;
				break;
			}
		}
		if (bAlreadyPresent) continue;

		SCS->Modify();
		USCS_Node* NewNode = SCS->CreateNode(ComponentClass, TEXT("ConvaiPSAudioCapture"));
		if (!NewNode)
		{
			Report.Warnings.Add(FString::Printf(
				TEXT("The streaming microphone could not be added to '%s'."), *Blueprint->GetName()));
			continue;
		}
		SCS->AddNode(NewNode);

		FString SaveError;
		if (!SaveBlueprint(Blueprint, SaveError))
		{
			Report.Warnings.Add(SaveError);
			continue;
		}
		Report.Changes.Add(FString::Printf(
			TEXT("Added the Convai Pixel Streaming audio capture to '%s'."), *Blueprint->GetName()));
	}
}

#undef LOCTEXT_NAMESPACE
