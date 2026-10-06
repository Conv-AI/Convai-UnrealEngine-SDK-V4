// Copyright 2026 Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarDownloadService.h"
#include "Services/ConvaiAvatarSupportFiles.h"
#include "Services/ConvaiAvatarMissingPackages.h"
#include "Services/ConvaiAvatarSourceMap.h"

#include "Archive/ConvaiAvatarSourceArchive.h"
#include "Config/ConvaiAvatarProjectConfiguration.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PackageTools.h"
#include "PluginDescriptor.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/ProjectPackagingSettings.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "Workspace/ConvaiAvatarWorkspace.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	constexpr const TCHAR* ReceiptName = TEXT("ConvaiAvatarPendingInstall.json");
	FString Normalize(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}
	FString AssetKey(const FString& Id) { return FMD5::HashAnsiString(*Id); }
	FString TargetDirectory(const FConvaiAvatarPendingDownload& Job) { return Normalize(FConvaiAvatarWorkspace::GetAvatarsDirectory() / Job.PluginName); }
	bool ValidBlueprintPath(const FString& Path, const FString& PluginName)
	{
		// IsValidObjectPath also requires a mounted root. Downloads are unmounted before
		// installation and while replacing files, so validate syntax and the selected root here.
		FString Package, Object;
		return Path.Split(TEXT("."), &Package, &Object)
			&& Package.StartsWith(TEXT("/") + PluginName + TEXT("/"))
			&& FPackageName::IsValidTextForLongPackageName(Package)
			&& !Object.IsEmpty() && FName::IsValidXName(Object, INVALID_OBJECTNAME_CHARACTERS);
	}
	bool ReadDownloadJson(const FString& Path, TSharedPtr<FJsonObject>& Out)
	{
		FString Text;
		return FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out.IsValid();
	}
	bool WriteDownloadJson(const FString& Path, const TSharedRef<FJsonObject>& Json, FString& Error)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		FString Text;
		FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
		const FString Temporary = Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			|| !IFileManager::Get().Move(*Path, *Temporary, true, false))
		{
			Error = TEXT("The pending avatar download could not be saved. Check available disk space and folder access.");
			IFileManager::Get().Delete(*Temporary);
			return false;
		}
		return true;
	}
	bool NoLinks(const FString& Path, FString& Error)
	{
#if PLATFORM_WINDOWS
		FString Current = Normalize(Path);
		while (!Current.IsEmpty())
		{
			const DWORD Attributes = GetFileAttributesW(*Current);
			if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			{
				Error = TEXT("The avatar's canonical or staging folder contains a symbolic link or junction. The local files were not replaced.");
				return false;
			}
			const FString Parent = FPaths::GetPath(Current);
			if (Parent == Current) break;
			Current = Parent;
		}
		return true;
#else
		Error = TEXT("Installing Cloud Avatars source currently requires Windows."); return false;
#endif
	}
	bool ValidateJobPaths(const FConvaiAvatarPendingDownload& Job, FString& Error)
	{
		FGuid Guid;
		if (Job.AssetId.IsEmpty() || Job.AssetId.Len() > 256 || !FGuid::ParseExact(Job.JobId, EGuidFormats::Digits, Guid)
			|| !FConvaiAvatarWorkspace::IsSafePluginName(Job.PluginName))
		{
			Error = TEXT("The pending download has invalid avatar identity information."); return false;
		}
		const FString ExpectedJob = Normalize(FConvaiAvatarDownloadService::PendingRoot() / AssetKey(Job.AssetId) / Job.JobId);
		const FString ExpectedStage = Normalize(ExpectedJob / TEXT("Source") / Job.PluginName);
		const FString ExpectedBackup = Normalize(FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/Backups") / AssetKey(Job.AssetId) / Job.JobId / Job.PluginName);
		if (!FPaths::IsSamePath(Job.JobDirectory, ExpectedJob) || !FPaths::IsSamePath(Job.StagedDirectory, ExpectedStage)
			|| !FPaths::IsSamePath(Job.BackupDirectory, ExpectedBackup)
			|| !ValidBlueprintPath(Job.EntryPoint, Job.PluginName))
		{
			Error = TEXT("The pending avatar download points outside its managed folders."); return false;
		}
		for (const FString& Name : Job.RequiredPlugins)
			if (!FConvaiAvatarWorkspace::IsSafePluginName(Name)) { Error = TEXT("The pending download contains an invalid dependency name."); return false; }
		if (Job.AcknowledgedMissingPackages.IsSet()
			&& !ConvaiAvatarMissingPackages::Validate(Job.AcknowledgedMissingPackages.GetValue(), Job.PluginName, Error)) return false;
		if (Job.SourceToDestinationPackages.IsSet()
			&& !ConvaiAvatarSourceMap::Validate(Job.SourceToDestinationPackages.GetValue(), Job.PluginName, Error)) return false;
		return ConvaiAvatarSupportFiles::Validate(Job, Error) && NoLinks(Job.JobDirectory, Error) && NoLinks(Job.StagedDirectory, Error)
			&& NoLinks(Job.BackupDirectory, Error) && NoLinks(TargetDirectory(Job), Error);
	}
	bool OwnedBy(const FString& Directory, const FConvaiAvatarPendingDownload& Job)
	{
		TSharedPtr<FJsonObject> Owner;
		FString Id, Plugin;
		return ReadDownloadJson(Directory / TEXT("ConvaiAvatarStudio.json"), Owner)
			&& Owner->TryGetStringField(TEXT("asset_id"), Id) && Id == Job.AssetId
			&& Owner->TryGetStringField(TEXT("plugin_name"), Plugin) && Plugin == Job.PluginName;
	}
	bool HasReceipt(const FString& Directory, const FConvaiAvatarPendingDownload& Job)
	{
		TSharedPtr<FJsonObject> Receipt;
		FString Id, JobId;
		return ReadDownloadJson(Directory / ReceiptName, Receipt) && Receipt->TryGetStringField(TEXT("asset_id"), Id)
			&& Id == Job.AssetId && Receipt->TryGetStringField(TEXT("job_id"), JobId) && JobId == Job.JobId;
	}
	bool RequiredSourceMissing(const FConvaiAvatarPendingDownload& Job)
	{
		const FString Package = FPackageName::ObjectPathToPackageName(Job.EntryPoint);
		const FString RelativePackage = Package.Mid(Job.PluginName.Len() + 2);
		if (!IFileManager::Get().DirectoryExists(*Job.StagedDirectory)
			|| !IFileManager::Get().FileExists(*(Job.StagedDirectory / (Job.PluginName + TEXT(".uplugin"))))
			|| !IFileManager::Get().FileExists(*(Job.StagedDirectory / TEXT("Content") / (RelativePackage + TEXT(".uasset"))))) return true;
		for (const auto& Pair : Job.SupportFiles)
		{
			// Matching existing support can be reused without the staging cache. Preflight
			// still verifies its bytes and loaded/dirty state before installation.
			if (!IFileManager::Get().FileExists(*(FPaths::ProjectContentDir() / Pair.Key))
				&& !IFileManager::Get().FileExists(*(Job.JobDirectory / TEXT("Source/BaseContent") / Pair.Key))) return true;
		}
		return false;
	}
	bool CanRetireMissingSource(const FConvaiAvatarPendingDownload& Job)
	{
		const auto Exists = [](const FString& Path)
		{
			return IFileManager::Get().FileExists(*Path) || IFileManager::Get().DirectoryExists(*Path);
		};
		// Only an untouched pre-install job can be abandoned. Keep all files; a journal
		// that may own installed support or a previous avatar must remain recoverable.
		if (Job.Phase != TEXT("staged") || Job.SupportPhase != TEXT("pending")
			|| Job.bHasPreviousWorkspaceState || !Job.SupportOwnedIdentities.IsEmpty()
			|| Exists(Job.BackupDirectory) || Exists(Job.JobDirectory / TEXT("previous-index.json"))
			|| Exists(Job.JobDirectory / TEXT("SupportInstall"))
			|| Exists(TargetDirectory(Job) / ReceiptName)
			|| IFileManager::Get().FileExists(*Job.StagedDirectory)) return false;
		if (IFileManager::Get().DirectoryExists(*Job.StagedDirectory)
			&& (!HasReceipt(Job.StagedDirectory, Job) || !OwnedBy(Job.StagedDirectory, Job))) return false;
		return !IFileManager::Get().DirectoryExists(*TargetDirectory(Job)) || OwnedBy(TargetDirectory(Job), Job);
	}
	FString MissingSourceRecovery(const FConvaiAvatarPendingDownload& Job)
	{
		return TEXT("This pending download's validated source is missing or changed, and it may contain installation or rollback state. Existing files and backups were kept. Restore its original validated Source folder under ")
			+ Job.JobDirectory + TEXT(" and retry. If that cache is unavailable, keep this recovery folder and download the avatar into a separate project; do not delete its journal or retained backup.");
	}
	bool EnsureInstallOwnership(const FString& Directory, const FConvaiAvatarPendingDownload& Job, FString& Error)
	{
		// Portable archives omit local ownership metadata. Keep a trusted local marker during
		// registration so Workspace can validate the previous index after a replacement.
		const FString File = Directory / TEXT("ConvaiAvatarStudio.json");
		if (IFileManager::Get().FileExists(*File))
		{
			if (OwnedBy(Directory, Job)) return true;
			Error = TEXT("The pending avatar's local ownership record belongs to a different asset."); return false;
		}
		if (!HasReceipt(Directory, Job)) { Error = TEXT("The avatar has no validated installation receipt."); return false; }
		const TSharedRef<FJsonObject> Owner = MakeShared<FJsonObject>();
		Owner->SetStringField(TEXT("asset_id"), Job.AssetId);
		Owner->SetStringField(TEXT("plugin_name"), Job.PluginName);
		return WriteDownloadJson(File, Owner, Error);
	}
	FString WorkspaceIndex(const FString& AssetId)
	{
		return Normalize(FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/Assets"))
			/ (FConvaiAvatarWorkspace::MakePluginName(AssetId) + TEXT(".json"));
	}
	bool SavePreviousWorkspaceState(FConvaiAvatarPendingDownload& Job, FString& Error)
	{
		if (Job.bHasPreviousWorkspaceState) return true;
		FConvaiAvatarPreparedAsset Previous;
		if (!FConvaiAvatarWorkspace::FindPreparedAsset(Job.AssetId, Previous, Error) || Previous.PluginName != Job.PluginName) return false;
		TSharedPtr<FJsonObject> Index;
		if (!ReadDownloadJson(WorkspaceIndex(Job.AssetId), Index)) { Error = TEXT("The previous avatar workspace record could not be backed up."); return false; }
		if (!WriteDownloadJson(Job.JobDirectory / TEXT("previous-index.json"), Index.ToSharedRef(), Error)) return false;
		if (!FConvaiAvatarProjectConfiguration::HasStagingCookExclusion(FPaths::ProjectDir(), Job.PluginName, Job.bPreviousCookExcluded, Error)) return false;
		Job.bHasPreviousWorkspaceState = true;
		return true;
	}
	bool RestorePreviousWorkspaceState(const FConvaiAvatarPendingDownload& Job, FString& Error)
	{
		if (!Job.bHasPreviousWorkspaceState) return true;
		TSharedPtr<FJsonObject> Index;
		FString Id, Plugin;
		if (!NoLinks(Job.JobDirectory / TEXT("previous-index.json"), Error)) return false;
		if (!ReadDownloadJson(Job.JobDirectory / TEXT("previous-index.json"), Index)
			|| !Index->TryGetStringField(TEXT("asset_id"), Id) || Id != Job.AssetId
			|| !Index->TryGetStringField(TEXT("plugin_name"), Plugin) || Plugin != Job.PluginName)
		{
			Error = TEXT("The retained workspace backup is missing or belongs to a different avatar."); return false;
		}
		if (!NoLinks(WorkspaceIndex(Job.AssetId), Error) || !WriteDownloadJson(WorkspaceIndex(Job.AssetId), Index.ToSharedRef(), Error)) return false;
		FConvaiAvatarPreparedAsset Previous;
		if (!FConvaiAvatarWorkspace::FindPreparedAsset(Job.AssetId, Previous, Error)) return false;
		return FConvaiAvatarWorkspace::SetHostCookExclusion(Previous, Job.bPreviousCookExcluded, Error);
	}
	bool MoveDirectory(const FString& From, const FString& To, FString& Error)
	{
		if (!NoLinks(From, Error) || !NoLinks(To, Error)) return false;
		if (!IFileManager::Get().DirectoryExists(*From) || IFileManager::Get().DirectoryExists(*To) || IFileManager::Get().FileExists(*To))
		{
			Error = TEXT("The avatar folders changed during installation. No existing destination will be overwritten."); return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(To), true);
#if PLATFORM_WINDOWS
		if (!MoveFileExW(*From, *To, MOVEFILE_WRITE_THROUGH))
		{
			Error = TEXT("The avatar folder could not be moved. Close programs using it and restart Unreal to finish the pending download."); return false;
		}
		return true;
#else
		return false;
#endif
	}
	TArray<UPackage*> LoadedPackages(const FString& PluginName)
	{
		TArray<UPackage*> Packages;
		const FString Mount = TEXT("/") + PluginName + TEXT("/");
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (!It->GetName().StartsWith(Mount)) continue;
			bool bHasObjects = false;
			ForEachObjectWithPackage(*It, [&bHasObjects](UObject*) { bHasObjects = true; return false; });
			if (bHasObjects || It->GetLinker()) Packages.Add(*It);
		}
		return Packages;
	}
	bool ConfirmDirtyChanges(const TArray<UPackage*>& Packages, TSet<FName>& ConfirmedDirtyPackages, FString& Notice)
	{
		TArray<FString> NewlyDirtyNames;
		for (const UPackage* Package : Packages)
			if (Package->IsDirty() && !ConfirmedDirtyPackages.Contains(Package->GetFName())) NewlyDirtyNames.Add(FPackageName::GetShortName(Package));
		if (NewlyDirtyNames.IsEmpty()) return true;
		const FString Message = TEXT("This avatar has unsaved changes in:\n\n") + FString::Join(NewlyDirtyNames, TEXT("\n"))
			+ TEXT("\n\nDiscard these changes and apply the downloaded source? Choosing No keeps your changes and leaves the download ready to apply.");
		if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Message), NSLOCTEXT("ConvaiAvatarStudio", "DiscardFreshAvatarChanges", "Replace unsaved avatar changes")) != EAppReturnType::Yes)
		{
			Notice = TEXT("Your unsaved changes were kept. Save or discard them, then click Get latest to apply the downloaded source.");
			return false;
		}
		for (const UPackage* Package : Packages) if (Package->IsDirty()) ConfirmedDirtyPackages.Add(Package->GetFName());
		return true;
	}
	bool UnloadAvatar(const FString& PluginName, bool bUserInitiated, FString& Notice)
	{
		TArray<UPackage*> Packages = LoadedPackages(PluginName);
		if (Packages.IsEmpty()) return true;
		if (!bUserInitiated)
		{
			Notice = TEXT("The downloaded source is ready. Click Get latest to apply it, or restart Unreal before this avatar is loaded. Its open editors and local changes were kept.");
			return false;
		}
		if (!GEditor || GEditor->PlayWorld) { Notice = TEXT("Stop Play mode or restart Unreal before applying the downloaded avatar update."); return false; }
		// This consent exists only for this synchronous invocation. Persisted replacement consent
		// never authorizes discarding edits made while a download was pending or in another session.
		TSet<FName> ConfirmedDirtyPackages;
		if (!ConfirmDirtyChanges(Packages, ConfirmedDirtyPackages, Notice)) return false;
		if (UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			for (UPackage* Package : Packages)
				ForEachObjectWithPackage(Package, [Editors](UObject* Object) { if (Object->IsAsset()) Editors->CloseAllEditorsForAsset(Object); return true; });
			bool bStillOpen = false;
			for (UPackage* Package : Packages)
				ForEachObjectWithPackage(Package, [Editors, &bStillOpen](UObject* Object)
				{
					if (Object->IsAsset() && !Editors->FindEditorsForAssetAndSubObjects(Object).IsEmpty()) bStillOpen = true;
					return !bStillOpen;
				});
			if (bStillOpen)
			{
				Notice = TEXT("The update is staged. Close this avatar's asset editors and try again, or restart Unreal to apply it.");
				return false; // Respect a cancelled editor-close/save prompt.
			}
		}
		Packages = LoadedPackages(PluginName);
		if (!ConfirmDirtyChanges(Packages, ConfirmedDirtyPackages, Notice)) return false;
		UPackageTools::FUnloadPackageParams Params(Packages);
		Params.bUnloadDirtyPackages = !ConfirmedDirtyPackages.IsEmpty();
		Params.bResetTransBuffer = false; // Preserve unrelated editor undo history; restart if it holds references.
		UPackageTools::UnloadPackages(Params);
		// UnloadPackages returns whether *any* package changed, not whether every package unloaded.
		if (!LoadedPackages(PluginName).IsEmpty())
		{
			Notice = TEXT("The updated source is ready. Restart Unreal to apply it before this avatar is loaded; its existing files are unchanged.");
			return false;
		}
		return true;
	}
	void RefreshMount(const FString& PluginName, const FString& Directory, bool bRemove)
	{
		const FString Root = TEXT("/") + PluginName + TEXT("/");
		const FString Content = Directory / TEXT("Content/");
		if (bRemove && FPackageName::MountPointExists(Root)) FPackageName::UnRegisterMountPoint(Root, Content);
		if (!bRemove)
		{
			if (!FPackageName::MountPointExists(Root)) FPackageName::RegisterMountPoint(Root, Content);
			IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			Registry.SetTemporaryCachingModeInvalidated();
			Registry.ScanPathsSynchronous({Root}, true);
		}
	}
	bool FinishReplacementRollback(FConvaiAvatarPendingDownload& Job, bool bUserInitiated, FString& Notice, FString& Error)
	{
		const FString Target = TargetDirectory(Job);
		if (HasReceipt(Target, Job))
		{
			if (!UnloadAvatar(Job.PluginName, bUserInitiated, Notice)) return false;
			RefreshMount(Job.PluginName, Target, true);
			if (!MoveDirectory(Target, Job.StagedDirectory, Error)) { RefreshMount(Job.PluginName, Target, false); return false; }
		}
		if (!IFileManager::Get().DirectoryExists(*Target))
		{
			if (!Job.bReplaceExisting && !Job.bHasPreviousWorkspaceState && HasReceipt(Job.StagedDirectory, Job))
			{
				if (!ConvaiAvatarSupportFiles::Rollback(Job, Error)) return false;
				Job.Phase = TEXT("staged");
				if (!FConvaiAvatarDownloadService::SaveJob(Job, Error)) return false;
				Notice = TEXT("The unsuccessful avatar installation was returned to staging. Newly added unchanged support files were removed; existing and edited files were kept.");
				return true;
			}
			if (!OwnedBy(Job.BackupDirectory, Job)) { Error = TEXT("The previous avatar is missing from its retained backup."); return false; }
			if (!MoveDirectory(Job.BackupDirectory, Target, Error)) return false;
		}
		if (!OwnedBy(Target, Job) || HasReceipt(Target, Job) || !HasReceipt(Job.StagedDirectory, Job))
		{
			Error = TEXT("The pending rollback folders no longer match the original and downloaded avatar."); return false;
		}
		FPluginDescriptor PreviousDescriptor;
		FText PreviousError;
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Job.PluginName))
			if (PreviousDescriptor.Load(Target / (Job.PluginName + TEXT(".uplugin")), PreviousError)) Plugin->UpdateDescriptor(PreviousDescriptor, PreviousError);
		RefreshMount(Job.PluginName, Target, false);
		if (!RestorePreviousWorkspaceState(Job, Error)) return false;
		if (!ConvaiAvatarSupportFiles::Rollback(Job, Error)) return false;
		Job.Phase = TEXT("staged");
		if (!FConvaiAvatarDownloadService::SaveJob(Job, Error)) return false;
		Notice = TEXT("The previous local avatar and its workspace/cook state were restored. The downloaded source remains staged for another attempt.");
		return true;
	}
	bool DependenciesReady(FConvaiAvatarPendingDownload& Job, bool bPrompt, FString& Notice, FString& Error)
	{
		TArray<FString> Queue = Job.RequiredPlugins, Missing, Disabled;
		TSet<FString> Visited;
		for (int32 Index = 0; Index < Queue.Num(); ++Index)
		{
			const FString Name = Queue[Index];
			if (Visited.Contains(Name) || Name == Job.PluginName) continue;
			Visited.Add(Name);
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
			if (!Plugin) { Missing.Add(Name); continue; }
			if (!Plugin->IsEnabled()) Disabled.Add(Name);
			for (const FPluginReferenceDescriptor& Reference : Plugin->GetDescriptor().Plugins)
				if (Reference.bEnabled && !Reference.bOptional) Queue.AddUnique(Reference.Name);
		}
		if (Missing.IsEmpty() && Disabled.IsEmpty()) return true;
		if (!Missing.IsEmpty()) Notice = TEXT("Source is ready. Install these required plugins, restart Unreal, and open Cloud Avatars to finish: ") + FString::Join(Missing, TEXT(", ")) + TEXT(".");
		if (!Disabled.IsEmpty())
		{
			bool bEnable=false;
			if(bPrompt)
			{
#if WITH_DEV_AUTOMATION_TESTS
				// The opt-in fresh-project fixture exercises this exact QA avatar's real dialog.
				// It still has to inspect the full dependency message and click the visible Yes button.
				const bool bFreshQa=Job.AssetId==TEXT("21856cca-f860-4b9f-9366-f9665a4f4f20") &&
					Job.AvatarName==TEXT("Cloud Avatars QA 69EFF3F64FE5845C6EC416B50D01802B") &&
					FParse::Param(FCommandLine::Get(),TEXT("AvatarStudioTestCloudFreshDownload")) &&
					FParse::Param(FCommandLine::Get(),TEXT("RenderOffscreen"));
				const TGuardValue<bool> DialogAutomationGuard(GIsAutomationTesting,bFreshQa?false:GIsAutomationTesting);
#endif
				bEnable=FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(TEXT("Enable the plugins required by ") + Job.AvatarName + TEXT("?\n\n") + FString::Join(Disabled, TEXT(", ")) + TEXT("\n\nUnreal must restart before this avatar can be installed. Your downloaded source will be kept."))) == EAppReturnType::Yes;
			}
			if(bEnable)
			{
				TArray<FString> Enabled;
				FText Reason;
				for (const FString& Name : Disabled)
				{
					if (!IProjectManager::Get().SetPluginEnabled(Name, true, Reason))
					{
						for (const FString& Revert : Enabled) { FText Ignored; IProjectManager::Get().SetPluginEnabled(Revert, false, Ignored); }
						Error = TEXT("The required plugins could not be enabled: ") + Reason.ToString(); return false;
					}
					Enabled.Add(Name);
				}
				if (!IProjectManager::Get().SaveCurrentProjectToDisk(Reason))
				{
					for (const FString& Revert : Enabled) { FText Ignored; IProjectManager::Get().SetPluginEnabled(Revert, false, Ignored); }
					Error = TEXT("The project could not save its enabled plugins: ") + Reason.ToString(); return false;
				}
				if (!Notice.IsEmpty()) Notice += TEXT(" ");
				Notice += TEXT("Required installed plugins are now enabled. Restart Unreal to finish adding the downloaded avatar.");
			}
			else
			{
				if (!Notice.IsEmpty()) Notice += TEXT(" ");
				Notice += TEXT("Enable these installed plugins and restart, or click Download / Get latest to enable them here: ") + FString::Join(Disabled, TEXT(", ")) + TEXT(".");
			}
		}
		return false;
	}
}

FString FConvaiAvatarDownloadService::PendingRoot() { return Normalize(FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/PendingDownloads")); }

bool FConvaiAvatarDownloadService::SaveJob(const FConvaiAvatarPendingDownload& Job, FString& OutError)
{
	if (!ValidateJobPaths(Job, OutError)) return false;
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("schema_version"), 1);
	Json->SetStringField(TEXT("job_id"), Job.JobId); Json->SetStringField(TEXT("asset_id"), Job.AssetId);
	Json->SetStringField(TEXT("avatar_name"), Job.AvatarName); Json->SetStringField(TEXT("plugin_name"), Job.PluginName);
	Json->SetStringField(TEXT("entry_point"), Job.EntryPoint); Json->SetStringField(TEXT("source_version"), Job.SourceVersion);
	Json->SetStringField(TEXT("phase"), Job.Phase);
	if (Job.MetaHumanChoice.IsSet()) Json->SetBoolField(TEXT("is_metahuman"), Job.MetaHumanChoice.GetValue());
	if (Job.IncludeConvaiContent.IsSet()) Json->SetBoolField(TEXT("include_convai_content"), Job.IncludeConvaiContent.GetValue());
	ConvaiAvatarMissingPackages::Write(*Json, Job.AcknowledgedMissingPackages);
	ConvaiAvatarSourceMap::Write(*Json, Job.SourceToDestinationPackages);
	Json->SetBoolField(TEXT("replace_existing"), Job.bReplaceExisting); Json->SetBoolField(TEXT("replacement_confirmed"), Job.bReplacementConfirmed);
	Json->SetBoolField(TEXT("has_previous_workspace_state"), Job.bHasPreviousWorkspaceState);
	Json->SetBoolField(TEXT("previous_cook_excluded"), Job.bPreviousCookExcluded);
	Json->SetStringField(TEXT("support_phase"), Job.SupportPhase);
	const TSharedRef<FJsonObject> SupportFiles = MakeShared<FJsonObject>(), SupportOwners = MakeShared<FJsonObject>();
	for (const auto& Pair : Job.SupportFiles) SupportFiles->SetStringField(Pair.Key, Pair.Value);
	for (const auto& Pair : Job.SupportOwnedIdentities) SupportOwners->SetStringField(Pair.Key, Pair.Value);
	Json->SetObjectField(TEXT("support_files"), SupportFiles);
	Json->SetObjectField(TEXT("support_owned_identities"), SupportOwners);
	TArray<TSharedPtr<FJsonValue>> Dependencies;
	for (const FString& Name : Job.RequiredPlugins) Dependencies.Add(MakeShared<FJsonValueString>(Name));
	Json->SetArrayField(TEXT("required_plugins"), Dependencies);
	return WriteDownloadJson(Job.JobDirectory / TEXT("job.json"), Json, OutError);
}

bool FConvaiAvatarDownloadService::ReadJob(const FString& JobFile, FConvaiAvatarPendingDownload& OutJob, FString& OutError)
{
	OutError.Reset();
	if (!NoLinks(JobFile, OutError)) return false;
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *JobFile))
	{
		OutError = TEXT("A pending avatar download has an unreadable manifest."); return false;
	}
	if (!ConvaiAvatarSourceMap::ValidateJsonText(Text, OutError)) return false;
	TSharedPtr<FJsonObject> Json;
	int32 Schema = 0;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid()
		|| !Json->TryGetNumberField(TEXT("schema_version"), Schema) || Schema != 1)
	{
		OutError = TEXT("A pending avatar download has an unreadable manifest."); return false;
	}
	FConvaiAvatarPendingDownload Job;
	Json->TryGetStringField(TEXT("job_id"), Job.JobId); Json->TryGetStringField(TEXT("asset_id"), Job.AssetId);
	Json->TryGetStringField(TEXT("avatar_name"), Job.AvatarName); Json->TryGetStringField(TEXT("plugin_name"), Job.PluginName);
	Json->TryGetStringField(TEXT("entry_point"), Job.EntryPoint); Json->TryGetStringField(TEXT("source_version"), Job.SourceVersion);
	Json->TryGetStringField(TEXT("phase"), Job.Phase);
	if (!ConvaiAvatarMissingPackages::Read(*Json, Job.PluginName, Job.AcknowledgedMissingPackages, OutError)) return false;
	if (!ConvaiAvatarSourceMap::Read(*Json, Job.PluginName, Job.SourceToDestinationPackages, OutError)) return false;
	if (const TSharedPtr<FJsonValue> Choice = Json->TryGetField(TEXT("is_metahuman")))
	{
		if (Choice->Type != EJson::Boolean) { OutError = TEXT("The pending avatar's MetaHuman choice is unreadable. Preserve its recovery files and retry the download after restoring its record."); return false; }
		Job.MetaHumanChoice = Choice->AsBool();
	}
	if (const TSharedPtr<FJsonValue> Choice = Json->TryGetField(TEXT("include_convai_content")))
	{
		if (Choice->Type != EJson::Boolean) { OutError = TEXT("The pending avatar's Convai content choice is unreadable. Preserve its recovery files and retry the download after restoring its record."); return false; }
		Job.IncludeConvaiContent = Choice->AsBool();
	}
	Json->TryGetBoolField(TEXT("replace_existing"), Job.bReplaceExisting); Json->TryGetBoolField(TEXT("replacement_confirmed"), Job.bReplacementConfirmed);
	Json->TryGetBoolField(TEXT("has_previous_workspace_state"), Job.bHasPreviousWorkspaceState);
	Json->TryGetBoolField(TEXT("previous_cook_excluded"), Job.bPreviousCookExcluded);
	Json->TryGetStringField(TEXT("support_phase"), Job.SupportPhase);
	for (const auto& Field : {TEXT("support_files"), TEXT("support_owned_identities")})
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Json->TryGetObjectField(Field, Object)) for (const auto& Pair : (*Object)->Values)
		{
			FString Value;
			if (!Pair.Value->TryGetString(Value)) { OutError = TEXT("The pending shared support record is unreadable."); return false; }
			(FCString::Strcmp(Field, TEXT("support_files")) == 0 ? Job.SupportFiles : Job.SupportOwnedIdentities).Add(FString(*Pair.Key), MoveTemp(Value));
		}
	}
	Json->TryGetStringArrayField(TEXT("required_plugins"), Job.RequiredPlugins);
	Job.JobDirectory = Normalize(FPaths::GetPath(JobFile));
	Job.StagedDirectory = Normalize(Job.JobDirectory / TEXT("Source") / Job.PluginName);
	Job.BackupDirectory = Normalize(FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/Backups") / AssetKey(Job.AssetId) / Job.JobId / Job.PluginName);
	if (!ValidateJobPaths(Job, OutError)) return false;
	OutJob = MoveTemp(Job); return true;
}

void FConvaiAvatarDownloadService::GetPending(TArray<FConvaiAvatarPendingDownload>& OutJobs, FString& OutError)
{
	OutJobs.Reset(); OutError.Reset();
	if (!NoLinks(PendingRoot(), OutError)) return;
	TArray<FString> Owners;
	IFileManager::Get().FindFiles(Owners, *(PendingRoot() / TEXT("*")), false, true);
	for (const FString& Owner : Owners)
	{
		if (!NoLinks(PendingRoot() / Owner, OutError)) continue;
		TArray<FString> Jobs;
		IFileManager::Get().FindFiles(Jobs, *(PendingRoot() / Owner / TEXT("*")), false, true);
		for (const FString& Id : Jobs)
		{
			const FString File = PendingRoot() / Owner / Id / TEXT("job.json");
			if (!IFileManager::Get().FileExists(*File)) continue;
			FConvaiAvatarPendingDownload Job;
			if (ReadJob(File, Job, OutError) && Job.Phase != TEXT("complete") && Job.Phase != TEXT("abandoned")) OutJobs.Add(MoveTemp(Job));
		}
	}
}

bool FConvaiAvatarDownloadService::FindPending(const FString& AssetId, FConvaiAvatarPendingDownload& OutJob, FString& OutError)
{
	TArray<FConvaiAvatarPendingDownload> Jobs;
	GetPending(Jobs, OutError);
	for (auto& Job : Jobs) if (Job.AssetId == AssetId)
	{
		if (RequiredSourceMissing(Job) && CanRetireMissingSource(Job))
		{
			// FindPending is called by the explicit Download/Get latest action. Persist
			// retirement before allowing that action to request a fresh source archive.
			Job.Phase = TEXT("abandoned");
			if (!SaveJob(Job, OutError)) return false;
			continue;
		}
		OutJob = MoveTemp(Job); return true;
	}
	return false;
}

bool FConvaiAvatarDownloadService::SnapshotSource(const FConvaiAvatarAsset& Asset, FConvaiAvatarDownloadSource& OutSource, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	FConvaiAvatarDownloadSource Source;
	Source.AssetId = Asset.AssetId; Source.AvatarName = Asset.Name; Source.SourceVersion = Asset.FindSourceVersion();
	if (Asset.Metadata.IsValid())
	{
		if (const TSharedPtr<FJsonValue> Choice = Asset.Metadata->TryGetField(TEXT("is_metahuman")))
		{
			if (Choice->Type != EJson::Boolean) { OutError = TEXT("This avatar has an invalid MetaHuman choice in its source details. Correct that record before downloading."); return false; }
			Source.MetaHumanChoice = Choice->AsBool();
		}
	}
	if (!Asset.Metadata || !Asset.Metadata->TryGetStringField(TEXT("plugin_name"), Source.PluginName)
		|| !Asset.Metadata->TryGetStringField(TEXT("blueprint_class_path"), Source.EntryPoint))
	{
		OutError = TEXT("This source record does not identify its avatar plugin and Blueprint."); return false;
	}
	FString EntryPackage, EntryObject;
	if (!Source.EntryPoint.Split(TEXT("."), &EntryPackage, &EntryObject)) Source.EntryPoint += TEXT(".") + FPackageName::GetShortName(Source.EntryPoint);
	else if (EntryObject == FPackageName::GetShortName(EntryPackage) + TEXT("_C")) Source.EntryPoint.RemoveFromEnd(TEXT("_C"));
	if (Source.AssetId.IsEmpty() || Source.AssetId.Len() > 256 || !FConvaiAvatarWorkspace::IsSafePluginName(Source.PluginName)
		|| !ValidBlueprintPath(Source.EntryPoint, Source.PluginName))
	{
		OutError = TEXT("The source record has an invalid plugin name or Blueprint path."); return false;
	}
	OutSource = MoveTemp(Source); return true;
}

bool FConvaiAvatarDownloadService::Stage(const FString& Zip, const FConvaiAvatarDownloadSource& Source, bool bReplaceConfirmed,
	FConvaiAvatarPendingDownload& OutJob, FString& OutError)
{
	OutError.Reset();
	FConvaiAvatarPendingDownload Job;
	Job.JobId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Job.AssetId = Source.AssetId; Job.AvatarName = Source.AvatarName; Job.SourceVersion = Source.SourceVersion;
	Job.PluginName = Source.PluginName; Job.EntryPoint = Source.EntryPoint;
	Job.MetaHumanChoice = Source.MetaHumanChoice;
	Job.JobDirectory = Normalize(PendingRoot() / AssetKey(Job.AssetId) / Job.JobId);
	Job.StagedDirectory = Normalize(Job.JobDirectory / TEXT("Source") / Job.PluginName);
	Job.BackupDirectory = Normalize(FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/Backups") / AssetKey(Job.AssetId) / Job.JobId / Job.PluginName);
	Job.bReplaceExisting = bReplaceConfirmed; Job.bReplacementConfirmed = bReplaceConfirmed;
	if (!ValidateJobPaths(Job, OutError)) return false;
	FString StageDirectory;
	TOptional<bool> ArchiveMetaHumanChoice;
	if (!FConvaiAvatarSourceArchive::Stage(Zip, Job.AssetId, Job.PluginName, FSoftObjectPath(Job.EntryPoint), Job.JobDirectory / TEXT("Source"), StageDirectory, Job.RequiredPlugins, OutError, &Job.SupportFiles, &ArchiveMetaHumanChoice, &Job.AcknowledgedMissingPackages, &Job.IncludeConvaiContent, &Job.SourceToDestinationPackages)) return false;
	if (!Job.MetaHumanChoice.IsSet()) Job.MetaHumanChoice = ArchiveMetaHumanChoice;
	if (!Job.IncludeConvaiContent.IsSet()) Job.IncludeConvaiContent = false;
	if (!FPaths::IsSamePath(StageDirectory, Job.StagedDirectory)) { OutError = TEXT("The validated source returned an unexpected staging directory."); return false; }
	const TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(TEXT("asset_id"), Job.AssetId); Receipt->SetStringField(TEXT("job_id"), Job.JobId);
	if (!WriteDownloadJson(Job.StagedDirectory / ReceiptName, Receipt, OutError)
		|| !EnsureInstallOwnership(Job.StagedDirectory, Job, OutError) || !SaveJob(Job, OutError)) return false;
	OutJob = MoveTemp(Job); return true;
}

EConvaiAvatarDownloadApplyResult FConvaiAvatarDownloadService::TryApply(FConvaiAvatarPendingDownload& Job, bool bPromptDependencies,
	FString& OutNotice, FString& OutError, bool bRegister, bool bUserInitiated)
{
	check(IsInGameThread());
	OutNotice.Reset(); OutError.Reset();
	if (!ValidateJobPaths(Job, OutError)) return EConvaiAvatarDownloadApplyResult::Failed;
	if (Job.Phase == TEXT("rolling_back"))
	{
		FinishReplacementRollback(Job, bUserInitiated, OutNotice, OutError);
		return EConvaiAvatarDownloadApplyResult::Pending;
	}
	if (Job.SupportPhase == TEXT("rolling_back") && !ConvaiAvatarSupportFiles::Rollback(Job, OutError)) return EConvaiAvatarDownloadApplyResult::Pending;
	if (!DependenciesReady(Job, bPromptDependencies, OutNotice, OutError))
		return OutError.IsEmpty() ? EConvaiAvatarDownloadApplyResult::Pending : EConvaiAvatarDownloadApplyResult::Failed;
	const FString Target = TargetDirectory(Job);
	bool bStageExists = IFileManager::Get().DirectoryExists(*Job.StagedDirectory);
	bool bTargetExists = IFileManager::Get().DirectoryExists(*Target);
	const bool bBackupExists = IFileManager::Get().DirectoryExists(*Job.BackupDirectory);
	if (!bStageExists && bTargetExists && HasReceipt(Target, Job) && Job.Phase != TEXT("installed"))
	{
		Job.Phase = TEXT("installed"); // Recover after the final folder move, including before editor startup.
		if (!SaveJob(Job, OutError)) return EConvaiAvatarDownloadApplyResult::Failed;
	}
	if (Job.Phase == TEXT("installed") && !bTargetExists && bStageExists && HasReceipt(Job.StagedDirectory, Job)) Job.Phase = TEXT("moving"); // Recover an interrupted registration rollback.
	// Once both the avatar and its support are installed, support is ordinary project content:
	// deferred registration must preserve edits made since that commit. A staged avatar with
	// installed support still needs the original preflight before its avatar-folder commit.
	if (!(Job.Phase == TEXT("installed") && Job.SupportPhase == TEXT("installed"))
		&& !ConvaiAvatarSupportFiles::Preflight(Job, OutError))
	{
		if (OutError == TEXT("The staged shared support is missing or changed. Download this avatar again.")) OutError = MissingSourceRecovery(Job);
		return EConvaiAvatarDownloadApplyResult::Pending;
	}
	if (Job.Phase != TEXT("installed") && RequiredSourceMissing(Job))
	{
		OutError = MissingSourceRecovery(Job);
		return EConvaiAvatarDownloadApplyResult::Failed;
	}
	if (Job.Phase != TEXT("installed"))
	{
		if (!bStageExists || !HasReceipt(Job.StagedDirectory, Job)) { OutError = MissingSourceRecovery(Job); return EConvaiAvatarDownloadApplyResult::Failed; }
		if (bTargetExists && !OwnedBy(Target, Job))
		{
			OutError = TEXT("A different or unmanaged plugin already uses this avatar's name. Its files will not be replaced."); return EConvaiAvatarDownloadApplyResult::Failed;
		}
		if (bTargetExists && (!Job.bReplaceExisting || !Job.bReplacementConfirmed))
		{
			if (!bUserInitiated)
			{
				OutNotice = TEXT("This avatar is already in the project. Click Get latest to confirm replacing it with the downloaded source.");
				return EConvaiAvatarDownloadApplyResult::Pending;
			}
			if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(TEXT("Replace the local source for ") + Job.AvatarName
				+ TEXT(" with the downloaded source? The existing files will be kept in a backup."))) != EAppReturnType::Yes)
			{
				OutNotice = TEXT("The local avatar was kept. Its downloaded source remains ready to apply."); return EConvaiAvatarDownloadApplyResult::Pending;
			}
			Job.bReplaceExisting = true; Job.bReplacementConfirmed = true;
			if (!SaveJob(Job, OutError)) return EConvaiAvatarDownloadApplyResult::Failed;
		}
		if (Job.bReplaceExisting && !Job.bReplacementConfirmed) { OutError = TEXT("Replacing the local avatar requires confirmation."); return EConvaiAvatarDownloadApplyResult::Failed; }
		if (bBackupExists && !OwnedBy(Job.BackupDirectory, Job)) { OutError = TEXT("The pending update's backup belongs to a different avatar."); return EConvaiAvatarDownloadApplyResult::Failed; }
		if (bTargetExists && !SavePreviousWorkspaceState(Job, OutError)) return EConvaiAvatarDownloadApplyResult::Failed;
		if (bTargetExists && !UnloadAvatar(Job.PluginName, bUserInitiated, OutNotice)) return EConvaiAvatarDownloadApplyResult::Pending;
		if (!ConvaiAvatarSupportFiles::Install(Job, OutError))
		{
			FString RollbackError;
			if (!ConvaiAvatarSupportFiles::Rollback(Job, RollbackError)) OutError += TEXT(" Rollback is pending: ") + RollbackError;
			return EConvaiAvatarDownloadApplyResult::Pending;
		}
		Job.Phase = TEXT("moving");
		if (!SaveJob(Job, OutError))
		{
			FString RollbackError; if (!ConvaiAvatarSupportFiles::Rollback(Job, RollbackError)) OutError += TEXT(" ") + RollbackError;
			return EConvaiAvatarDownloadApplyResult::Pending;
		}
		if (bTargetExists)
		{
			RefreshMount(Job.PluginName, Target, true);
			if (!MoveDirectory(Target, Job.BackupDirectory, OutError))
			{
				RefreshMount(Job.PluginName, Target, false);
				FString RollbackError; if (!ConvaiAvatarSupportFiles::Rollback(Job, RollbackError)) OutError += TEXT(" ") + RollbackError;
				return EConvaiAvatarDownloadApplyResult::Pending;
			}
		}
		if (!MoveDirectory(Job.StagedDirectory, Target, OutError))
		{
			if (IFileManager::Get().DirectoryExists(*Job.BackupDirectory))
			{
				FString RollbackError;
				if (!MoveDirectory(Job.BackupDirectory, Target, RollbackError)) OutError += TEXT(" The previous avatar remains in its retained backup: ") + Job.BackupDirectory;
				else RefreshMount(Job.PluginName, Target, false);
			}
			FString RollbackError; if (!ConvaiAvatarSupportFiles::Rollback(Job, RollbackError)) OutError += TEXT(" ") + RollbackError;
			return EConvaiAvatarDownloadApplyResult::Pending;
		}
		Job.Phase = TEXT("installed");
		if (!SaveJob(Job, OutError)) return EConvaiAvatarDownloadApplyResult::Failed;
	}
	if (!HasReceipt(Target, Job) && !OwnedBy(Target, Job)) { OutError = TEXT("The installed pending avatar no longer matches this download."); return EConvaiAvatarDownloadApplyResult::Failed; }
	if (!EnsureInstallOwnership(Target, Job, OutError)) return EConvaiAvatarDownloadApplyResult::Failed;
	// Support must be present before registration or any mount-triggered Blueprint load.
	if (!(Job.Phase == TEXT("installed") && Job.SupportPhase == TEXT("installed"))
		&& !ConvaiAvatarSupportFiles::Install(Job, OutError)) return EConvaiAvatarDownloadApplyResult::Pending;
	ConvaiAvatarSupportFiles::RefreshRegistry(Job);
	if (!bRegister || !GEditor)
	{
		RefreshMount(Job.PluginName, Target, false);
		OutNotice = TEXT("Downloaded avatar source applied. Open Cloud Avatars after the editor starts to finish registering it.");
		return EConvaiAvatarDownloadApplyResult::Pending;
	}
	FPluginDescriptor Descriptor;
	FText Reason;
	if (!Descriptor.Load(Target / (Job.PluginName + TEXT(".uplugin")), Reason)) { OutError = Reason.ToString(); return EConvaiAvatarDownloadApplyResult::Failed; }
	if (const TSharedPtr<IPlugin> ExistingPlugin = IPluginManager::Get().FindPlugin(Job.PluginName))
	{
		if (!FPaths::IsSamePath(ExistingPlugin->GetBaseDir(), Target) || !ExistingPlugin->UpdateDescriptor(Descriptor, Reason)) { OutError = TEXT("The installed avatar plugin could not be refreshed: ") + Reason.ToString(); return EConvaiAvatarDownloadApplyResult::Failed; }
	}
	RefreshMount(Job.PluginName, Target, false);
	FConvaiAvatarPreparedAsset Prepared;
	if (!FConvaiAvatarWorkspace::RegisterDownloadedAsset(Job.AssetId, Target, FSoftObjectPath(Job.EntryPoint), Prepared, OutError, Job.MetaHumanChoice, Job.AcknowledgedMissingPackages, TOptional<bool>(Job.IncludeConvaiContent.Get(false)), Job.SourceToDestinationPackages))
	{
		// Registration can discover an invalid Blueprint after disk validation. Restore the old
		// version if the rejected content can unload; never replace files beneath loaded packages.
		FString UnloadNotice;
		if (((IFileManager::Get().DirectoryExists(*Job.BackupDirectory) && OwnedBy(Job.BackupDirectory, Job)) || (!Job.bReplaceExisting && !Job.bHasPreviousWorkspaceState))
			&& UnloadAvatar(Job.PluginName, bUserInitiated, UnloadNotice))
		{
			FString RollbackError;
			Job.Phase = TEXT("rolling_back");
			if (SaveJob(Job, RollbackError) && FinishReplacementRollback(Job, bUserInitiated, OutNotice, RollbackError)) OutError += TEXT(" ") + OutNotice;
			else OutError += TEXT(" Rollback is pending: ") + RollbackError;
			return EConvaiAvatarDownloadApplyResult::Pending;
		}
		OutNotice = TEXT("Downloaded source is installed but registration is pending. Its previous version is retained in ") + Job.BackupDirectory + TEXT(". Restart Unreal and refresh Cloud Avatars.");
		return EConvaiAvatarDownloadApplyResult::Pending;
	}
	Job.Phase = TEXT("complete");
	if (!SaveJob(Job, OutError)) return EConvaiAvatarDownloadApplyResult::Failed;
	IFileManager::Get().Delete(*(Target / ReceiptName));
	OutNotice = Job.bReplaceExisting ? TEXT("Latest avatar source installed. The previous local files are backed up in ") + Job.BackupDirectory
		: TEXT("Avatar added to this project. Its Blueprint is in Plugins / ConvaiAvatars / ") + Prepared.PluginName;
	return EConvaiAvatarDownloadApplyResult::Applied;
}
