// Copyright 2026 Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarDownloadService.h"
#include "Services/ConvaiAvatarNativeRuntimeContent.h"
#include "Services/ConvaiAvatarDownloadServicePrivate.h"
#include "Services/ConvaiAvatarSupportFiles.h"
#include "ConvaiAvatarDurableFile.h"
#include "Services/ConvaiAvatarMissingPackages.h"
#include "Services/ConvaiAvatarSourceMap.h"
#include "Services/ConvaiAvatarPluginDependencies.h"
#include "Misc/App.h"

#include "Archive/ConvaiAvatarSourceArchive.h"
#include "Archive/ConvaiAvatarSourceArchivePrivate.h"
#include "Config/ConvaiAvatarProjectConfiguration.h"
#include "Packaging/ConvaiAvatarPackagingPrivate.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
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
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
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
	/** In a job folder once the job is being discarded: its restore then retires it instead of staging it for another attempt. */
	constexpr const TCHAR* DiscardName = TEXT("discard.json");
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
	/** Reads only schema_version and phase: finished history is never acted on, so it needs no full validation. */
	bool IsFinishedDownloadJob(const FString& JobFile)
	{
		TSharedPtr<FJsonObject> Json;
		int32 Schema = 0;
		FString Phase;
		return ReadDownloadJson(JobFile, Json) && Json->TryGetNumberField(TEXT("schema_version"), Schema) && Schema == 1
			&& Json->TryGetStringField(TEXT("phase"), Phase) && (Phase == TEXT("complete") || Phase == TEXT("abandoned"));
	}
	bool WriteDownloadJson(const FString& Path, const TSharedRef<FJsonObject>& Json, FString& Error)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		FString Text;
		FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
		if (!ConvaiAvatarDurableFile::Replace(Path, Text, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Error = TEXT("The pending avatar download could not be saved. Check available disk space and folder access.");
			return false;
		}
		return true;
	}
	bool NoLinks(const FString& Path, FString& Error, const FString& StopAt = FString())
	{
#if PLATFORM_WINDOWS
		FString Current = Normalize(Path);
		while (!Current.IsEmpty() && (StopAt.IsEmpty() || !FPaths::IsSamePath(Current, StopAt)))
		{
			if (ConvaiAvatarPackagingPrivate::IsReparsePoint(Current))
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
		// A staged job has moved nothing, so the live record and cook rule are the state to restore: an earlier attempt's snapshot
		// would revert records saved since. Once the job is moving, the old avatar may already be in its backup.
		if (Job.bHasPreviousWorkspaceState && Job.Phase != TEXT("staged")) return true;
		FConvaiAvatarPreparedAsset Previous;
		if (!FConvaiAvatarWorkspace::FindPreparedAsset(Job.AssetId, Previous, Error) || Previous.PluginName != Job.PluginName) return false;
		TSharedPtr<FJsonObject> Index;
		if (!ReadDownloadJson(WorkspaceIndex(Job.AssetId), Index)) { Error = TEXT("The previous avatar workspace record could not be backed up."); return false; }
		if (!WriteDownloadJson(Job.JobDirectory / TEXT("previous-index.json"), Index.ToSharedRef(), Error)) return false;
		if (!FConvaiAvatarProjectConfiguration::HasStagingCookExclusion(FPaths::ProjectDir(), Job.PluginName, Job.bPreviousCookExcluded, Error)) return false;
		Job.bHasPreviousWorkspaceState = true;
		return true;
	}
	bool RestorePreviousWorkspaceState(const FConvaiAvatarPendingDownload& Job, FString& Error, bool* bOutCannotFinish = nullptr)
	{
		if (!Job.bHasPreviousWorkspaceState) return true;
		TSharedPtr<FJsonObject> Index;
		FString Id, Plugin;
		if (!NoLinks(Job.JobDirectory / TEXT("previous-index.json"), Error)) return false;
		if (!ReadDownloadJson(Job.JobDirectory / TEXT("previous-index.json"), Index)
			|| !Index->TryGetStringField(TEXT("asset_id"), Id) || Id != Job.AssetId
			|| !Index->TryGetStringField(TEXT("plugin_name"), Plugin) || Plugin != Job.PluginName)
		{
			if (bOutCannotFinish) *bOutCannotFinish = true;
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
	/** Ends the notice of every restore that waits for its avatar to unload. */
	constexpr const TCHAR* RestoreLater = TEXT(" Its previous state is restored at the next editor start, before a level loads it; to restore it sooner, release the avatar and choose Get latest.");
	/** Why the avatar unloads, which words its unsaved-changes prompt and the notice of an unload that has to wait. */
	enum class EUnloadFor : uint8
	{
		/** The downloaded source replaces it. */
		Apply,
		/** Its previous state comes back after a registration this machine stopped. Nothing is saved to finish that later, so an unload
		 * that cannot run leaves the download installed for another try, and the caller words that notice. */
		Retry,
		/** Its previous state comes back, saved as rolling_back first: a restore that waits finishes at the next editor start. */
		Restore,
	};
	bool ConfirmDirtyChanges(const TArray<UPackage*>& Packages, TSet<FName>& ConfirmedDirtyPackages, EUnloadFor For, FString& Notice)
	{
		TArray<FString> NewlyDirtyNames;
		for (const UPackage* Package : Packages)
			if (Package->IsDirty() && !ConfirmedDirtyPackages.Contains(Package->GetFName())) NewlyDirtyNames.Add(FPackageName::GetShortName(Package));
		if (NewlyDirtyNames.IsEmpty()) return true;
		const FString Message = TEXT("This avatar has unsaved changes in:\n\n") + FString::Join(NewlyDirtyNames, TEXT("\n")) + (For == EUnloadFor::Restore
			? TEXT("\n\nDiscard these changes and restore this avatar's previous state? Choosing No keeps your changes until the next editor start, which restores it.")
			: For == EUnloadFor::Retry ? TEXT("\n\nDiscard these changes and restore this avatar's previous state? Choosing No keeps your changes and the downloaded version; refresh Cloud Avatars to retry.")
			: TEXT("\n\nDiscard these changes and apply the downloaded source? Choosing No keeps your changes and leaves the download ready to apply."));
		if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Message), NSLOCTEXT("ConvaiAvatarStudio", "DiscardFreshAvatarChanges", "Replace unsaved avatar changes")) != EAppReturnType::Yes)
		{
			Notice = For == EUnloadFor::Restore ? FString(TEXT("Your unsaved changes were kept.")) + RestoreLater
				: TEXT("Your unsaved changes were kept. Save or discard them, then click Get latest to apply the downloaded source.");
			return false;
		}
		for (const UPackage* Package : Packages) if (Package->IsDirty()) ConfirmedDirtyPackages.Add(Package->GetFName());
		return true;
	}
	bool UnloadAvatar(const FString& PluginName, bool bUserInitiated, EUnloadFor For, FString& Notice)
	{
		const auto Deferred = [For, &Notice](const TCHAR* Restoring, const TCHAR* Applying)
		{
			Notice = For == EUnloadFor::Restore ? FString(Restoring) + RestoreLater : FString(Applying);
			return false;
		};
		TArray<UPackage*> Packages = LoadedPackages(PluginName);
		if (Packages.IsEmpty()) return true;
		if (!bUserInitiated) return Deferred(TEXT("This avatar is loaded."),
			TEXT("The downloaded source is ready. Click Get latest to apply it, or restart Unreal before this avatar is loaded. Its open editors and local changes were kept."));
		if (!GEditor || GEditor->PlayWorld) return Deferred(TEXT("Play mode is running."), TEXT("Stop Play mode or restart Unreal before applying the downloaded avatar update."));
		// This consent exists only for this synchronous invocation. Persisted replacement consent
		// never authorizes discarding edits made while a download was pending or in another session.
		TSet<FName> ConfirmedDirtyPackages;
		if (!ConfirmDirtyChanges(Packages, ConfirmedDirtyPackages, For, Notice)) return false;
		UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		// Respect a cancelled editor-close/save prompt.
		if (Editors && !ConvaiAvatarDownloadServicePrivate::CloseAvatarEditors(Packages, *Editors)) return Deferred(TEXT("An asset editor for this avatar stayed open."),
			TEXT("The update is staged. Close this avatar's asset editors and try again, or restart Unreal to apply it."));
		Packages = LoadedPackages(PluginName);
		if (!ConfirmDirtyChanges(Packages, ConfirmedDirtyPackages, For, Notice)) return false;
		UPackageTools::FUnloadPackageParams Params(Packages);
		Params.bUnloadDirtyPackages = !ConfirmedDirtyPackages.IsEmpty();
		Params.bResetTransBuffer = false; // Preserve unrelated editor undo history; restart if it holds references.
		UPackageTools::UnloadPackages(Params);
		// UnloadPackages returns whether *any* package changed, not whether every package unloaded.
		if (!LoadedPackages(PluginName).IsEmpty()) return Deferred(TEXT("This avatar is still in use, for example placed in the open level."),
			TEXT("The updated source is ready. Restart Unreal to apply it before this avatar is loaded; its existing files are unchanged."));
		return true;
	}
	/** Validation records a package count only once it accepts the avatar, so a failure with a count came from a later write on this
	 * machine: a read-only configuration, a locked index or a proxy link. Without one, the downloaded files were refused, and no
	 * retry of the same files can change that, unless the check that refused them was about this editor's state. Unsaved edits are
	 * that state wherever they fail validation, so TryApply asks only when the avatar has none. */
	bool RejectedSource(const FConvaiAvatarPreparedAsset& Attempt, const FString& Error)
	{
		if (Attempt.PackageCount > 0) return false;
		// ponytail: matched by the wording ConvaiAvatarWorkspace.cpp gives those checks (a cancelled scan, unsaved edits, a missing,
		// disabled or unmountable plugin). A RegisterDownloadedAsset that reported which stage failed would replace this list.
		// Edits saved to the installed files still pass for the download's own, so one that fails validation discards the download;
		// hashing the files at install would tell the two apart. So does an unsaved breaking edit outside the avatar's mount, such
		// as to a parent Blueprint: it leaves the avatar Blueprint in error with its own packages clean.
		for (const TCHAR* EditorState : {TEXT("Avatar preparation was cancelled"), TEXT("Save your changes before "), TEXT("The plugin required by "),
			TEXT("Enable the "), TEXT("Install and enable "), TEXT("Unreal could not mount ")})
			if (Error.StartsWith(EditorState)) return false;
		return true;
	}
	/** Validation words its compile error for an upload; a fix made to this copy would leave with the discarded download. */
	FString RejectionReason(const FString& Error)
	{
		return Error == TEXT("The prepared avatar Blueprint cannot load or has compile errors. Open and compile it before uploading.")
			? FString(TEXT("Its Blueprint cannot load, or does not compile with this project's engine and plugins.")) : Error;
	}
	/** A first download, or a replacement whose avatar folder was already gone when it installed: no previous avatar was moved aside. */
	bool NothingToRestore(const FConvaiAvatarPendingDownload& Job)
	{
		return !Job.bHasPreviousWorkspaceState && (!Job.bReplaceExisting || !IFileManager::Get().DirectoryExists(*Job.BackupDirectory));
	}
	/** Unset unless the job is being discarded. The reason is why registration rejected its source, and empty for the user's discard. */
	TOptional<FString> DiscardReason(const FConvaiAvatarPendingDownload& Job)
	{
		const FString File = Job.JobDirectory / DiscardName;
		if (!IFileManager::Get().FileExists(*File)) return {};
		TSharedPtr<FJsonObject> Discard;
		FString Reason, LinkError;
		if (NoLinks(File, LinkError) && ReadDownloadJson(File, Discard)) Discard->TryGetStringField(TEXT("reason"), Reason);
		return Reason;
	}
	/** bUser marks the user's own discard; a rejected source's discard keeps only its reason. */
	bool SaveDiscard(const FConvaiAvatarPendingDownload& Job, const FString& Reason, FString& Error, bool bUser = false)
	{
		const TSharedRef<FJsonObject> Discard = MakeShared<FJsonObject>();
		Discard->SetStringField(TEXT("reason"), Reason);
		if (bUser) Discard->SetBoolField(TEXT("user"), true);
		return WriteDownloadJson(Job.JobDirectory / DiscardName, Discard, Error);
	}
	/** Whether the user already chose to discard the job once. */
	bool UserDiscarded(const FConvaiAvatarPendingDownload& Job)
	{
		const FString File = Job.JobDirectory / DiscardName;
		TSharedPtr<FJsonObject> Discard;
		FString LinkError;
		bool bUser = false;
		return IFileManager::Get().FileExists(*File) && NoLinks(File, LinkError) && ReadDownloadJson(File, Discard)
			&& Discard->TryGetBoolField(TEXT("user"), bUser) && bUser;
	}
	FString DiscardedNotice(const FString& Reason, bool bRestoredPrevious)
	{
		const FString Discarded = TEXT("The download was discarded");
		if (Reason.IsEmpty())
			return Discarded + (bRestoredPrevious ? TEXT(" and its previous version restored. Choose Get latest to download the newest source.") : TEXT(" and removed from this project."));
		return Discarded + TEXT(" because it cannot register in this project: ") + Reason + (bRestoredPrevious
			? TEXT(" Its previous version was restored. Fix the source where it was made and upload it again, then choose Get latest.")
			: TEXT(" It was removed from this project. Fix the source where it was made and upload it again, then download it."));
	}
	/** bOutCannotFinish is set when the restore can never finish: the previous avatar or its workspace record is gone from the
	 *  backup, or the folders no longer match the download. Every other failure, such as a file in use or a failed write, can finish
	 *  on a later try. */
	bool FinishReplacementRollback(FConvaiAvatarPendingDownload& Job, bool bUserInitiated, FString& Notice, FString& Error, bool* bOutCannotFinish = nullptr)
	{
		const FString Target = TargetDirectory(Job);
		if (HasReceipt(Target, Job))
		{
			if (!UnloadAvatar(Job.PluginName, bUserInitiated, EUnloadFor::Restore, Notice)) return false;
			ConvaiAvatarDownloadServicePrivate::RefreshMount(Job.PluginName, Target, true);
			if (!MoveDirectory(Target, Job.StagedDirectory, Error)) { ConvaiAvatarDownloadServicePrivate::RefreshMount(Job.PluginName, Target, false); return false; }
		}
		const TOptional<FString> Discarding = DiscardReason(Job);
		if (!IFileManager::Get().DirectoryExists(*Target))
		{
			if (NothingToRestore(Job) && HasReceipt(Job.StagedDirectory, Job))
			{
				if (!ConvaiAvatarSupportFiles::Rollback(Job, Error)) return false;
				Job.Phase = Discarding.IsSet() ? TEXT("abandoned") : TEXT("staged");
				if (!FConvaiAvatarDownloadService::SaveJob(Job, Error)) return false;
				Notice = Discarding.IsSet() ? DiscardedNotice(*Discarding, false)
					: TEXT("The unsuccessful avatar installation was returned to staging. Newly added unchanged support files were removed; existing and edited files were kept.");
				return true;
			}
			if (!OwnedBy(Job.BackupDirectory, Job))
			{
				if (bOutCannotFinish) *bOutCannotFinish = true;
				Error = TEXT("The previous avatar is missing from its retained backup."); return false;
			}
			// The avatar folder can be gone with its mount point left registered, as a delete outside Unreal leaves it.
			ConvaiAvatarDownloadServicePrivate::RefreshMount(Job.PluginName, Target, true);
			if (!MoveDirectory(Job.BackupDirectory, Target, Error)) return false;
		}
		if (!OwnedBy(Target, Job) || HasReceipt(Target, Job) || !HasReceipt(Job.StagedDirectory, Job))
		{
			if (bOutCannotFinish) *bOutCannotFinish = true;
			Error = TEXT("The pending rollback folders no longer match the original and downloaded avatar."); return false;
		}
		FPluginDescriptor PreviousDescriptor;
		FText PreviousError;
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Job.PluginName))
			if (PreviousDescriptor.Load(Target / (Job.PluginName + TEXT(".uplugin")), PreviousError)) Plugin->UpdateDescriptor(PreviousDescriptor, PreviousError);
		ConvaiAvatarDownloadServicePrivate::RefreshMount(Job.PluginName, Target, false);
		if (!RestorePreviousWorkspaceState(Job, Error, bOutCannotFinish)) return false;
		if (!ConvaiAvatarSupportFiles::Rollback(Job, Error)) return false;
		Job.Phase = Discarding.IsSet() ? TEXT("abandoned") : TEXT("staged");
		if (!FConvaiAvatarDownloadService::SaveJob(Job, Error)) return false;
		Notice = Discarding.IsSet() ? DiscardedNotice(*Discarding, true)
			: TEXT("The previous local avatar and its workspace/cook state were restored. The downloaded source remains staged for another attempt.");
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
				if (ConvaiAvatarPluginDependencies::IsRequired(Reference, FPlatformMisc::GetUBTPlatform(),
					FApp::GetBuildConfiguration(), FApp::GetBuildTargetType())) Queue.AddUnique(Reference.Name);
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

bool ConvaiAvatarDownloadServicePrivate::IsFinishedJob(const FConvaiAvatarPendingDownload& Job)
{
	return IsFinishedDownloadJob(Job.JobDirectory / TEXT("job.json"));
}

TOptional<FString> ConvaiAvatarDownloadServicePrivate::DiscardingReason(const FConvaiAvatarPendingDownload& Job)
{
	return DiscardReason(Job);
}

bool ConvaiAvatarDownloadServicePrivate::CloseAvatarEditors(const TArray<UPackage*>& Packages, UAssetEditorSubsystem& Editors)
{
	// Collect first, as PackageTools' unloader does: a closing editor can create or rename objects
	// (a Material editor applying its changes), which is fatal inside ForEachObjectWithPackage.
	const auto Collect = [&Packages]
	{
		TArray<UObject*> Objects;
		for (UPackage* Package : Packages) GetObjectsWithPackage(Package, Objects);
		return Objects;
	};
	for (UObject* Object : Collect()) if (Object->IsAsset()) Editors.CloseAllEditorsForAsset(Object);
	// Collect again: a close may have added or renamed objects in these packages.
	return !Collect().ContainsByPredicate([&Editors](UObject* Object)
		{ return Object->IsAsset() && !Editors.FindEditorsForAssetAndSubObjects(Object).IsEmpty(); });
}

bool ConvaiAvatarDownloadServicePrivate::NoLinksBelow(const FString& Path, const FString& Anchor, FString& Error)
{
	return NoLinks(Path, Error, FPaths::IsUnderDirectory(Path, Anchor) ? Normalize(Anchor) : FString());
}

bool ConvaiAvatarDownloadServicePrivate::IsDeadInstalledJob(const FConvaiAvatarPendingDownload& Job)
{
	const FString Target = TargetDirectory(Job);
	return Job.Phase == TEXT("installed") && Job.SupportPhase != TEXT("installing") && Job.SupportPhase != TEXT("rolling_back")
		&& !IFileManager::Get().DirectoryExists(*Job.StagedDirectory) && !HasReceipt(Target, Job) && !OwnedBy(Target, Job);
}

TArray<FString> ConvaiAvatarDownloadServicePrivate::DropStagedFiles(const FString& JobDirectory)
{
	TArray<FString> Left = ConvaiAvatarPackagingPrivate::DeleteOwnedTree(JobDirectory / TEXT("Source"), JobDirectory);
	Left.Append(ConvaiAvatarPackagingPrivate::DeleteOwnedTree(JobDirectory / TEXT("SupportInstall"), JobDirectory));
	return Left;
}

void ConvaiAvatarDownloadServicePrivate::DropJobFolder(const FString& JobDirectory)
{
	// GetPending's sweep can find a folder again while its delete still runs; two deletes of one tree would each report the other's.
	static FCriticalSection DroppingLock;
	static TSet<FString> Dropping;
	{
		FScopeLock Scope(&DroppingLock);
		if (Dropping.Contains(JobDirectory)) return;
		Dropping.Add(JobDirectory);
	}
	Async(EAsyncExecution::ThreadPool, [JobDirectory]
	{
		ConvaiAvatarPackagingPrivate::WarnScratchKept(ConvaiAvatarPackagingPrivate::DeleteOwnedTree(JobDirectory, FConvaiAvatarDownloadService::PendingRoot()));
		FScopeLock Scope(&DroppingLock);
		Dropping.Remove(JobDirectory);
	});
}

#if WITH_DEV_AUTOMATION_TESTS
int32 ConvaiAvatarDownloadServicePrivate::RefreshMountScans = 0;
#endif

void ConvaiAvatarDownloadServicePrivate::RefreshMount(const FString& PluginName, const FString& Directory, bool bRemove)
{
	const FString Root = TEXT("/") + PluginName + TEXT("/");
	const FString Content = Directory / TEXT("Content/");
	if (bRemove && FPackageName::MountPointExists(Root)) FPackageName::UnRegisterMountPoint(Root, Content);
	if (!bRemove && !FPackageName::MountPointExists(Root))
	{
		FPackageName::RegisterMountPoint(Root, Content);
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.SetTemporaryCachingModeInvalidated();
		Registry.ScanPathsSynchronous({Root}, true);
#if WITH_DEV_AUTOMATION_TESTS
		++RefreshMountScans;
#endif
	}
}

FString FConvaiAvatarDownloadService::PendingRoot() { return Normalize(FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/PendingDownloads")); }

bool FConvaiAvatarDownloadService::SaveJob(const FConvaiAvatarPendingDownload& Job, FString& OutError)
{
	return ValidateJobPaths(Job, OutError) && ConvaiAvatarDownloadServicePrivate::PersistJob(Job, OutError);
}

bool ConvaiAvatarDownloadServicePrivate::PersistJob(const FConvaiAvatarPendingDownload& Job, FString& OutError)
{
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("schema_version"), 1);
	Json->SetStringField(TEXT("job_id"), Job.JobId); Json->SetStringField(TEXT("asset_id"), Job.AssetId);
	Json->SetStringField(TEXT("avatar_name"), Job.AvatarName); Json->SetStringField(TEXT("plugin_name"), Job.PluginName);
	Json->SetStringField(TEXT("entry_point"), Job.EntryPoint); Json->SetStringField(TEXT("source_version"), Job.SourceVersion);
	Json->SetStringField(TEXT("phase"), Job.Phase);
	if (Job.MetaHumanChoice.IsSet()) Json->SetBoolField(TEXT("is_metahuman"), Job.MetaHumanChoice.GetValue());
	if (Job.IncludeConvaiContent.IsSet()) Json->SetBoolField(TEXT("include_convai_content"), Job.IncludeConvaiContent.GetValue());
	if (!Job.NativeRuntimeContract.IsEmpty()) Json->SetStringField(TEXT("native_runtime_contract"),Job.NativeRuntimeContract);
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
    if (const auto Field = Json->TryGetField(TEXT("native_runtime_contract")))
        if (Field->Type != EJson::String || !Field->TryGetString(Job.NativeRuntimeContract) || !ConvaiAvatarNativeRuntimeContent::IsValidRequirement(Job.NativeRuntimeContract))
        { OutError=TEXT("The source requires unsupported groom runtime content."); return false; }
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
	const FString Root = PendingRoot();
	FString Error;
	// Links count only below the project, which may itself be reached through one. Each job's ReadJob still walks every ancestor.
	if (!ConvaiAvatarDownloadServicePrivate::NoLinksBelow(Root, FPaths::ProjectDir(), Error)) { OutError = Root + TEXT(": ") + Error; return; }
	// Every failure keeps its own error, prefixed with its folder: a later job must not reset an earlier one's.
	TArray<FString> Failures;
	TArray<FString> Owners;
	IFileManager::Get().FindFiles(Owners, *(Root / TEXT("*")), false, true);
	for (const FString& Owner : Owners)
	{
		if (!ConvaiAvatarDownloadServicePrivate::NoLinksBelow(Root / Owner, FPaths::ProjectDir(), Error)) { Failures.Add(Root / Owner + TEXT(": ") + Error); continue; }
		TArray<FString> Jobs;
		IFileManager::Get().FindFiles(Jobs, *(Root / Owner / TEXT("*")), false, true);
		for (const FString& Id : Jobs)
		{
			const FString File = Root / Owner / Id / TEXT("job.json");
			if (!IFileManager::Get().FileExists(*File))
			{
				// Staging that never got its job.json: a crash, a kill or an exit cut it off, and nothing reads the folder. No staging
				// outlives its 6 h 2 min deadline, so a folder a day old is no other editor's either.
				const FDateTime Written = IFileManager::Get().GetTimeStamp(*(Root / Owner / Id));
				if (Written != FDateTime::MinValue() && Written < FDateTime::UtcNow() - FTimespan::FromDays(1))
					ConvaiAvatarDownloadServicePrivate::DropJobFolder(Root / Owner / Id);
				continue;
			}
			if (IsFinishedDownloadJob(File)) continue;
			FConvaiAvatarPendingDownload Job;
			if (ReadJob(File, Job, Error)) OutJobs.Add(MoveTemp(Job));
			else Failures.Add(Root / Owner / Id + TEXT(": ") + Error);
		}
	}
	OutError = FString::Join(Failures, TEXT("\n"));
}

bool FConvaiAvatarDownloadService::FindPending(const FString& AssetId, FConvaiAvatarPendingDownload& OutJob, FString& OutError)
{
	TArray<FConvaiAvatarPendingDownload> Jobs;
	GetPending(Jobs, OutError);
	for (auto& Job : Jobs) if (Job.AssetId == AssetId)
	{
		if ((RequiredSourceMissing(Job) && CanRetireMissingSource(Job)) || ConvaiAvatarDownloadServicePrivate::IsDeadInstalledJob(Job))
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

void ConvaiAvatarDownloadServicePrivate::Discard(FConvaiAvatarPendingDownload& Job, FString& OutNotice, FString& OutError)
{
	check(IsInGameThread());
	OutNotice.Reset(); OutError.Reset();
	if (!ValidateJobPaths(Job, OutError)) return;
	FString RestoreError;
	// A staged avatar is not installed yet; only support files the job added can be in the project.
	if (Job.Phase == TEXT("staged")) ConvaiAvatarSupportFiles::Rollback(Job, RestoreError);
	else
	{
		// Discarding again after the user's own earlier discard retires the job below whatever stops its restore, except an
		// avatar that still cannot unload, so a download does not stay stuck. A rejected source's discard keeps its reason.
		const bool bAgain = UserDiscarded(Job);
		if (!bAgain && !SaveDiscard(Job, DiscardReason(Job).Get(FString()), OutError, true)) return;
		Job.Phase = TEXT("rolling_back");
		if (!FConvaiAvatarDownloadService::SaveJob(Job, OutError)) return;
		// Finished, the restore retired the job. Waiting on an unload, the next editor start finishes it.
		bool bCannotFinish = false;
		if (FinishReplacementRollback(Job, true, OutNotice, RestoreError, &bCannotFinish) || RestoreError.IsEmpty()) return;
		// Stopped by a file in use or a failed write: the job stays discarding, and the next editor start finishes the restore
		// before anything loads the avatar or its support files.
		if (!bAgain && !bCannotFinish)
		{
			OutNotice = TEXT("The download will be discarded the next time Unreal starts. ") + RestoreError;
			return;
		}
	}
	// What cannot be restored is reported, and the job is retired anyway, so that a new download can start. Every file is kept.
	Job.Phase = TEXT("abandoned");
	if (!FConvaiAvatarDownloadService::SaveJob(Job, OutError)) return;
	if (RestoreError.IsEmpty()) { OutNotice = TEXT("The download was discarded before it was installed."); return; }
	// Nothing finishes a retired job, so the reason is given without the advice to restart that came with it.
	FString Reason = RestoreError;
	RestoreError.Split(TEXT(". "), &Reason, nullptr);
	Reason.RemoveFromEnd(TEXT("."));
	OutError = TEXT("The download was discarded, but not everything it changed could be undone: ") + Reason
		+ TEXT(". Its files were kept in ") + Job.JobDirectory
		+ (IFileManager::Get().DirectoryExists(*Job.BackupDirectory) ? TEXT(" and ") + Job.BackupDirectory : FString()) + TEXT(".");
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
	static const std::atomic<bool> NeverCancelled{false};
	if (!ConvaiAvatarDownloadServicePrivate::Stage(Zip, Source, bReplaceConfirmed, OutJob, OutError, NeverCancelled)) return false;
	if (SaveJob(OutJob, OutError)) return true;
	ConvaiAvatarDownloadServicePrivate::DropJobFolder(OutJob.JobDirectory);
	return false;
}

bool ConvaiAvatarDownloadServicePrivate::Stage(const FString& Zip, const FConvaiAvatarDownloadSource& Source, bool bReplaceConfirmed,
	FConvaiAvatarPendingDownload& OutJob, FString& OutError, const std::atomic<bool>& Cancelled)
{
	OutError.Reset();
	FConvaiAvatarPendingDownload Job;
	Job.JobId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Job.AssetId = Source.AssetId; Job.AvatarName = Source.AvatarName; Job.SourceVersion = Source.SourceVersion;
	Job.PluginName = Source.PluginName; Job.EntryPoint = Source.EntryPoint;
	Job.MetaHumanChoice = Source.MetaHumanChoice;
	Job.JobDirectory = Normalize(FConvaiAvatarDownloadService::PendingRoot() / AssetKey(Job.AssetId) / Job.JobId);
	Job.StagedDirectory = Normalize(Job.JobDirectory / TEXT("Source") / Job.PluginName);
	Job.BackupDirectory = Normalize(FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/Backups") / AssetKey(Job.AssetId) / Job.JobId / Job.PluginName);
	Job.bReplaceExisting = bReplaceConfirmed; Job.bReplacementConfirmed = bReplaceConfirmed;
	if (!ValidateJobPaths(Job, OutError)) return false;
	// The folder is this call's own, its GUID new. Whatever stops it, a cancel, the helper's deadline or a failed write, leaves what
	// the helper extracted there: it goes off this worker, which the game thread may be waiting for.
	bool bStaged = false;
	ON_SCOPE_EXIT { if (!bStaged) ConvaiAvatarDownloadServicePrivate::DropJobFolder(Job.JobDirectory); };
	FString StageDirectory;
	TOptional<bool> ArchiveMetaHumanChoice;
	// The validated portable archive owns the requirement, including its absence in legacy archives.
	if (!ConvaiAvatarSourceArchivePrivate::Stage(Zip, Job.AssetId, Job.PluginName, FSoftObjectPath(Job.EntryPoint), Job.JobDirectory / TEXT("Source"), StageDirectory, Job.RequiredPlugins, OutError,
		&Job.SupportFiles, &ArchiveMetaHumanChoice, &Job.AcknowledgedMissingPackages, &Job.IncludeConvaiContent, &Job.SourceToDestinationPackages, Cancelled, &Job.NativeRuntimeContract)) return false;
	if (!Job.MetaHumanChoice.IsSet()) Job.MetaHumanChoice = ArchiveMetaHumanChoice;
	if (!Job.IncludeConvaiContent.IsSet()) Job.IncludeConvaiContent = false;
	if (!FPaths::IsSamePath(StageDirectory, Job.StagedDirectory)) { OutError = TEXT("The validated source returned an unexpected staging directory."); return false; }
	const TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(TEXT("asset_id"), Job.AssetId); Receipt->SetStringField(TEXT("job_id"), Job.JobId);
	if (!WriteDownloadJson(Job.StagedDirectory / ReceiptName, Receipt, OutError) || !EnsureInstallOwnership(Job.StagedDirectory, Job, OutError)) return false;
	bStaged = true;
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
		if (bTargetExists && !UnloadAvatar(Job.PluginName, bUserInitiated, EUnloadFor::Apply, OutNotice)) return EConvaiAvatarDownloadApplyResult::Pending;
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
		// Also without the avatar folder: a delete outside Unreal leaves its mount point registered.
		ConvaiAvatarDownloadServicePrivate::RefreshMount(Job.PluginName, Target, true);
		if (bTargetExists && !MoveDirectory(Target, Job.BackupDirectory, OutError))
		{
			ConvaiAvatarDownloadServicePrivate::RefreshMount(Job.PluginName, Target, false);
			FString RollbackError; if (!ConvaiAvatarSupportFiles::Rollback(Job, RollbackError)) OutError += TEXT(" ") + RollbackError;
			return EConvaiAvatarDownloadApplyResult::Pending;
		}
		if (!MoveDirectory(Job.StagedDirectory, Target, OutError))
		{
			if (IFileManager::Get().DirectoryExists(*Job.BackupDirectory))
			{
				FString RollbackError;
				if (!MoveDirectory(Job.BackupDirectory, Target, RollbackError)) OutError += TEXT(" The previous avatar remains in its retained backup: ") + Job.BackupDirectory;
				else ConvaiAvatarDownloadServicePrivate::RefreshMount(Job.PluginName, Target, false);
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
		ConvaiAvatarDownloadServicePrivate::RefreshMount(Job.PluginName, Target, false);
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
	ConvaiAvatarDownloadServicePrivate::RefreshMount(Job.PluginName, Target, false);
	FConvaiAvatarPreparedAsset Prepared;
	// Validation reads the avatar as this editor holds it, so an unsaved edit fails it whatever the files hold, compile errors included.
	const bool bUnsavedEdits = LoadedPackages(Job.PluginName).ContainsByPredicate([](const UPackage* Package) { return Package->IsDirty(); });
	if (!FConvaiAvatarWorkspace::RegisterDownloadedAsset(Job.AssetId, Target, FSoftObjectPath(Job.EntryPoint), Prepared, OutError, Job.MetaHumanChoice, Job.AcknowledgedMissingPackages, TOptional<bool>(Job.IncludeConvaiContent.Get(false)), Job.SourceToDestinationPackages, Job.NativeRuntimeContract))
	{
		FString RollbackError;
		if (!bUnsavedEdits && RejectedSource(Prepared, OutError))
		{
			// These files can never register here, so this one failure discards them. The restore is saved before any unload: an
			// avatar that cannot unload now is restored at the next editor start, before a level can load it.
			const FString Rejection = RejectionReason(OutError);
			OutError = TEXT("The download cannot register in this project and will be discarded: ") + Rejection;
			FString RollbackNotice;
			Job.Phase = TEXT("rolling_back");
			if (SaveDiscard(Job, Rejection, RollbackError) && SaveJob(Job, RollbackError) && FinishReplacementRollback(Job, bUserInitiated, RollbackNotice, RollbackError))
				OutError = RollbackNotice;
			else OutError += TEXT(" ") + (RollbackError.IsEmpty() ? RollbackNotice : TEXT("Rollback is pending: ") + RollbackError);
			return EConvaiAvatarDownloadApplyResult::Pending;
		}
		// This machine stopped registration, so the same files may register later. Restore the old version if the downloaded
		// content can unload, staging it for another attempt; never replace files beneath loaded packages.
		const bool bHasBackup = IFileManager::Get().DirectoryExists(*Job.BackupDirectory) && OwnedBy(Job.BackupDirectory, Job);
		FString UnloadNotice;
		if ((bHasBackup || NothingToRestore(Job)) && UnloadAvatar(Job.PluginName, bUserInitiated, EUnloadFor::Retry, UnloadNotice))
		{
			Job.Phase = TEXT("rolling_back");
			if (SaveJob(Job, RollbackError) && FinishReplacementRollback(Job, bUserInitiated, OutNotice, RollbackError)) OutError += TEXT(" ") + OutNotice;
			else OutError += TEXT(" Rollback is pending: ") + RollbackError;
			return EConvaiAvatarDownloadApplyResult::Pending;
		}
		OutNotice = TEXT("Downloaded source is installed but not registered yet. Fix the reported problem, then refresh Cloud Avatars to retry.")
			+ (bHasBackup ? TEXT(" Its previous version is retained in ") + Job.BackupDirectory + TEXT(".") : FString());
		return EConvaiAvatarDownloadApplyResult::Pending;
	}
	// The receipt goes before the job is saved complete: nothing applies a complete job again, and no listing reads a folder that
	// holds its receipt. A crash in between leaves the job installed in a folder it owns, which the next refresh registers again.
	if (!IFileManager::Get().Delete(*(Target / ReceiptName), false, false, true))
	{
		OutError = TEXT("The avatar is installed, but its pending-install record could not be removed: ") + Target / ReceiptName + TEXT(". Close any program using it, then refresh Cloud Avatars.");
		return EConvaiAvatarDownloadApplyResult::Pending;
	}
	Job.Phase = TEXT("complete");
	if (!SaveJob(Job, OutError)) return EConvaiAvatarDownloadApplyResult::Failed;
	// The staged support copies can run to thousands of files, so they go off the game thread.
	Async(EAsyncExecution::ThreadPool, [JobDirectory = Job.JobDirectory]
		{ ConvaiAvatarPackagingPrivate::WarnScratchKept(ConvaiAvatarDownloadServicePrivate::DropStagedFiles(JobDirectory)); });
	OutNotice = Job.bReplaceExisting ? TEXT("Latest avatar source installed. The previous local files are backed up in ") + Job.BackupDirectory
		: TEXT("Avatar added to this project. Its Blueprint is in Plugins / ConvaiAvatars / ") + Prepared.PluginName;
	return EConvaiAvatarDownloadApplyResult::Applied;
}
