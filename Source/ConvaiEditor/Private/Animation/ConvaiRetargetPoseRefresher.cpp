/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiRetargetPoseRefresher.cpp
 */

#include "ConvaiRetargetPoseRefresher.h"
#include "ConvaiEditor.h"

// The MetaHuman content that depends on the project skeleton ships only in UE 5.6+
// packages (E:/Dependencies_MH_fix), and the APIs used here are exported from 5.6.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "ConvaiRetargetPoseRefresher"

namespace
{
	const TCHAR* ConfigSection = TEXT("ConvaiMetaHumanAnimations");
	const TCHAR* ConfigKey = TEXT("UpToDateFor");

	FDelegateHandle PreSaveHandle;
	FDelegateHandle EditorInitializedHandle;
	FDelegateHandle FilesLoadedHandle;

	bool UsesStoredRetargetPose(const UAnimSequence* Sequence)
	{
		// The stored pose is only used when no named retarget source is selected.
		return Sequence && Sequence->GetSkeleton() && Sequence->RetargetSource.IsNone()
			&& !Sequence->GetRetargetSourceAsset().IsNull()
			&& Sequence->GetPackage()->GetName().StartsWith(TEXT("/ConvAI/"));
	}

	/** True when the pose stored in the sequence differs from the one its retarget mesh gives on the current skeleton. */
	bool IsStoredPoseOutOfDate(const UAnimSequence* Sequence, const USkeletalMesh* RetargetMesh)
	{
		TArray<FTransform> Expected;
		FAnimationRuntime::MakeSkeletonRefPoseFromMesh(RetargetMesh, Sequence->GetSkeleton(), Expected);
		const TArray<FTransform>& Stored = Sequence->RetargetSourceAssetReferencePose;
		if (Stored.Num() != Expected.Num())
		{
			return true;
		}
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			if (!Stored[Index].Equals(Expected[Index], KINDA_SMALL_NUMBER))
			{
				return true;
			}
		}
		return false;
	}

	/** Reads an object path tag, which may be in export-text form (Class'/Path.Name') or None. */
	FSoftObjectPath GetPathTag(const FAssetData& Asset, const TCHAR* Tag)
	{
		const FString Value = FPackageName::ExportTextPathToObjectPath(Asset.GetTagValueRef<FString>(FName(Tag)));
		return (Value.IsEmpty() || Value == TEXT("None")) ? FSoftObjectPath() : FSoftObjectPath(Value);
	}

	FString PackageFilename(const UPackage* Package)
	{
		return FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	}

	/** Identifies the skeletons and plugin version a clean check was made against. */
	FString MakeUpToDateKey(const TSet<FSoftObjectPath>& Skeletons)
	{
		TArray<FString> Parts;
		for (const FSoftObjectPath& Skeleton : Skeletons)
		{
			FString Filename;
			FPackageName::DoesPackageExist(Skeleton.GetLongPackageName(), &Filename);
			Parts.Add(FString::Printf(TEXT("%s@%s"), *Skeleton.ToString(), *IFileManager::Get().GetTimeStamp(*Filename).ToString()));
		}
		Parts.Sort();
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ConvAI"));
		Parts.Add(Plugin ? Plugin->GetDescriptor().VersionName : FString());
		return FString::Join(Parts, TEXT(";"));
	}

	void Notify(const FText& Message, SNotificationItem::ECompletionState State)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 8.0f;
		Info.bUseSuccessFailIcons = true;
		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(State);
		}
	}

	/** Re-evaluates placed characters on the fixed skeletons so the editor viewport shows the new pose. */
	void RefreshPlacedCharacters(const TSet<const USkeleton*>& Skeletons)
	{
		for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
		{
			USkeletalMeshComponent* Component = *It;
			const USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset();
			if (Mesh && Skeletons.Contains(Mesh->GetSkeleton()) && Component->IsRegistered() && !Component->IsTemplate())
			{
				Component->InitAnim(true);
			}
		}
	}

	/** Rebuilds, recompresses and saves the given sequences. Returns how many were saved. */
	int32 FixAndSave(const TArray<UAnimSequence*>& Sequences)
	{
		FScopedSlowTask Progress(Sequences.Num(), LOCTEXT("Fixing", "Retargeting Convai MetaHuman animations..."));
		Progress.MakeDialog();

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TSet<const USkeleton*> FixedSkeletons;
		int32 Saved = 0;
		for (UAnimSequence* Sequence : Sequences)
		{
			Progress.EnterProgressFrame(1.0f, FText::FromString(Sequence->GetName()));

			Sequence->UpdateRetargetSourceAssetData();
			Sequence->BeginCacheDerivedDataForCurrentPlatform();
			FixedSkeletons.Add(Sequence->GetSkeleton());

			UPackage* Package = Sequence->GetPackage();
			Package->MarkPackageDirty();
			const FString Filename = PackageFilename(Package);
			// Marketplace and Fab installs ship the plugin's content read-only.
			if (PlatformFile.IsReadOnly(*Filename))
			{
				PlatformFile.SetReadOnly(*Filename, false);
			}

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.Error = GWarn;
			if (UPackage::SavePackage(Package, Sequence, *Filename, SaveArgs))
			{
				++Saved;
			}
			else
			{
				UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiEditor: could not save %s to %s"), *Sequence->GetPathName(), *Filename);
			}
		}
		RefreshPlacedCharacters(FixedSkeletons);
		return Saved;
	}

	void CheckOnStartup()
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		FARFilter Filter;
		Filter.PackagePaths.Add(TEXT("/ConvAI"));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);

		// Only sequences whose skeleton and retarget mesh both exist can be checked and fixed.
		// Without the project's MetaHuman Common assets, resaving would break them for every project.
		TArray<FAssetData> Candidates;
		TSet<FSoftObjectPath> Skeletons;
		for (const FAssetData& Asset : Assets)
		{
			const FSoftObjectPath Skeleton = GetPathTag(Asset, TEXT("Skeleton"));
			if (Skeleton.IsNull() || !AssetRegistry.GetAssetByObjectPath(Skeleton).IsValid())
			{
				continue;
			}
			// A missing tag is resolved after loading; a retarget mesh that doesn't exist rules the sequence out.
			const FSoftObjectPath RetargetMesh = GetPathTag(Asset, TEXT("RetargetSourceAsset"));
			if (!RetargetMesh.IsNull() && !AssetRegistry.GetAssetByObjectPath(RetargetMesh).IsValid())
			{
				continue;
			}
			Candidates.Add(Asset);
			Skeletons.Add(Skeleton);
		}
		if (Candidates.Num() == 0)
		{
			UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiEditor: MetaHuman animation check skipped; the project has no MetaHuman skeleton for them"));
			return;
		}

		const FString UpToDateKey = MakeUpToDateKey(Skeletons);
		FString LastUpToDate;
		GConfig->GetString(ConfigSection, ConfigKey, LastUpToDate, GEditorPerProjectIni);
		if (LastUpToDate == UpToDateKey)
		{
			UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiEditor: MetaHuman animations already verified for this project's skeleton"));
			return;
		}

		TArray<UAnimSequence*> OutOfDate;
		{
			FScopedSlowTask Progress(Candidates.Num(), LOCTEXT("Checking", "Checking Convai MetaHuman animations..."));
			Progress.MakeDialog();
			for (const FAssetData& Asset : Candidates)
			{
				Progress.EnterProgressFrame();
				UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset());
				if (!UsesStoredRetargetPose(Sequence))
				{
					continue;
				}
				const USkeletalMesh* RetargetMesh = Sequence->GetRetargetSourceAsset().LoadSynchronous();
				if (RetargetMesh && IsStoredPoseOutOfDate(Sequence, RetargetMesh))
				{
					OutOfDate.Add(Sequence);
				}
			}
		}
		UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiEditor: %d of %d Convai MetaHuman animations need retargeting for this project's skeleton"), OutOfDate.Num(), Candidates.Num());

		if (OutOfDate.Num() == 0)
		{
			GConfig->SetString(ConfigSection, ConfigKey, *UpToDateKey, GEditorPerProjectIni);
			GConfig->Flush(false, GEditorPerProjectIni);
			return;
		}
		if (FApp::IsUnattended())
		{
			return;
		}

		const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo,
			FText::Format(LOCTEXT("Prompt", "The current Convai animations are not retargeted for your MetaHumans ({0} animations), so your characters may look distorted.\n\nWould you like us to fix them? This updates and saves these animations in the Convai plugin."), OutOfDate.Num()),
			LOCTEXT("PromptTitle", "Convai MetaHuman Animations"));
		if (Answer != EAppReturnType::Yes)
		{
			UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiEditor: MetaHuman animation fix declined; asking again on the next startup"));
			return;
		}

		const int32 Saved = FixAndSave(OutOfDate);
		UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiEditor: retargeted and saved %d of %d Convai MetaHuman animations"), Saved, OutOfDate.Num());
		if (Saved == OutOfDate.Num())
		{
			GConfig->SetString(ConfigSection, ConfigKey, *UpToDateKey, GEditorPerProjectIni);
			GConfig->Flush(false, GEditorPerProjectIni);
			Notify(FText::Format(LOCTEXT("Fixed", "Fixed {0} Convai MetaHuman animations."), Saved), SNotificationItem::CS_Success);
		}
		else
		{
			Notify(FText::Format(LOCTEXT("PartlyFixed", "Fixed {0} of {1} Convai MetaHuman animations. See the Output Log for the ones that could not be saved."), Saved, OutOfDate.Num()), SNotificationItem::CS_Fail);
		}
	}

	void CheckWhenAssetsAreKnown()
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		if (AssetRegistry.IsLoadingAssets())
		{
			FilesLoadedHandle = AssetRegistry.OnFilesLoaded().AddLambda([]()
			{
				FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().OnFilesLoaded().Remove(FilesLoadedHandle);
				FilesLoadedHandle.Reset();
				CheckOnStartup();
			});
			return;
		}
		CheckOnStartup();
	}

	void OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext)
	{
		if (!SaveContext.IsCooking())
		{
			return;
		}
		UAnimSequence* Sequence = Cast<UAnimSequence>(Object);
		if (!UsesStoredRetargetPose(Sequence))
		{
			return;
		}
		const USkeletalMesh* RetargetMesh = Sequence->GetRetargetSourceAsset().LoadSynchronous();
		if (RetargetMesh && IsStoredPoseOutOfDate(Sequence, RetargetMesh))
		{
			// The platform data was compressed with the stale pose before this point;
			// rebuild it before the package is serialized. The plugin file is untouched.
			Sequence->UpdateRetargetSourceAssetData();
			Sequence->CacheDerivedDataForPlatform(SaveContext.GetTargetPlatform());
			UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiEditor: retargeted %s for cooking"), *Sequence->GetPathName());
		}
	}
}

void FConvaiRetargetPoseRefresher::Register()
{
	PreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddStatic(&OnObjectPreSave);

	if (GIsEditor && !IsRunningCommandlet())
	{
		EditorInitializedHandle = FEditorDelegates::OnEditorInitialized.AddLambda([](double)
		{
			FEditorDelegates::OnEditorInitialized.Remove(EditorInitializedHandle);
			EditorInitializedHandle.Reset();
			CheckWhenAssetsAreKnown();
		});
	}
}

void FConvaiRetargetPoseRefresher::Unregister()
{
	FCoreUObjectDelegates::OnObjectPreSave.Remove(PreSaveHandle);
	PreSaveHandle.Reset();
	if (EditorInitializedHandle.IsValid())
	{
		FEditorDelegates::OnEditorInitialized.Remove(EditorInitializedHandle);
		EditorInitializedHandle.Reset();
	}
	if (FilesLoadedHandle.IsValid())
	{
		if (FAssetRegistryModule* AssetRegistry = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
		{
			AssetRegistry->Get().OnFilesLoaded().Remove(FilesLoadedHandle);
		}
		FilesLoadedHandle.Reset();
	}
}

#undef LOCTEXT_NAMESPACE

#else

void FConvaiRetargetPoseRefresher::Register() {}
void FConvaiRetargetPoseRefresher::Unregister() {}

#endif
