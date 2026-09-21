// Copyright Convai Inc. All Rights Reserved.
#include "Workspace/ConvaiAvatarWorkspace.h"
#include "Misc/EngineVersionComparison.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"
#include "Workspace/ConvaiAvatarBlueprintSetup.h"
#include "ConvaiAvatarReferenceRemap.h"
#include "ConvaiAvatarSourceRevision.h"
#include "ConvaiAvatarReusableEnums.h"
#include "ConvaiAvatarWorkspaceLinks.h"
#include "Config/ConvaiAvatarProjectConfiguration.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintNodeTemplateCache.h"
#include "Components/ChildActorComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Brush.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "ExternalPackageHelper.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/KismetReinstanceUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/EngineVersionComparison.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
#include "Misc/ScopedCVar.h"
#endif
#include "Misc/ScopedSlowTask.h"
#if UE_VERSION_OLDER_THAN(5, 7, 0)
#include "Containers/UnrealString.h"
#else
#include "Misc/StringOutputDevice.h"
#endif
#include "Misc/ScopeExit.h"
#include "Misc/RedirectCollector.h"
#include "Misc/SecureHash.h"
#include "PluginDescriptor.h"
#include "PackageTools.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <winioctl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace ConvaiAvatarWorkspacePrivate
{
static constexpr int32 ManifestVersion = 1;
static constexpr int32 MaximumPackages = 15000;
static const TCHAR* ManifestName = TEXT("ConvaiAvatarStudio.json");
static const TCHAR* BindingReceiptName = TEXT("ConvaiAvatarCloudBinding.json");

void GetPreparationObjectsWithOuter(const UObjectBase* Outer, TArray<UObject*>& Objects, bool bIncludeNestedObjects)
{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	GetObjectsWithOuter(Outer, Objects, bIncludeNestedObjects);
#else
	GetObjectsWithOuter(Outer, Objects, bIncludeNestedObjects ? EGetObjectsFlags::IncludeNestedObjects : EGetObjectsFlags::None);
#endif
}

FString Normalized(const FString& Path)
{
	FString Result = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeDirectoryName(Result);
	FPaths::CollapseRelativeDirectories(Result);
	return Result;
}

bool IsInside(const FString& Path, const FString& Root)
{
	return Normalized(Path).StartsWith(Normalized(Root) + TEXT("/"), ESearchCase::IgnoreCase);
}

bool IsReparsePoint(const FString& Path)
{
#if PLATFORM_WINDOWS
	const DWORD Attributes = GetFileAttributesW(*Path);
	return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
	return false;
#endif
}

bool ResolveDirectory(const FString& Directory, FString& OutResolved)
{
#if PLATFORM_WINDOWS
	HANDLE Handle = CreateFileW(*Directory, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (Handle == INVALID_HANDLE_VALUE) return false;
	WCHAR Buffer[32768];
	const DWORD Size = GetFinalPathNameByHandleW(Handle, Buffer, UE_ARRAY_COUNT(Buffer), FILE_NAME_NORMALIZED);
	CloseHandle(Handle);
	if (!Size || Size >= UE_ARRAY_COUNT(Buffer)) return false;
	OutResolved = Buffer;
	if (OutResolved.StartsWith(TEXT("\\\\?\\UNC\\"))) OutResolved = TEXT("\\\\") + OutResolved.Mid(8);
	else if (OutResolved.StartsWith(TEXT("\\\\?\\"))) OutResolved.RightChopInline(4);
	OutResolved = Normalized(OutResolved);
	return true;
#else
	OutResolved = Normalized(Directory);
	return IFileManager::Get().DirectoryExists(*Directory);
#endif
}

/** Creates one directory at a time and rejects redirected ancestors outside the expected project tree. */
bool ManagedDirectory(const FString& Directory, FString& Error, TArray<FString>* CreatedDirectories = nullptr)
{
	const FString ProjectRoot = Normalized(FPaths::ProjectDir());
	const FString Absolute = Normalized(Directory);
	if (!IsInside(Absolute, ProjectRoot))
	{
		Error = TEXT("The avatar workspace must stay inside this project.");
		return false;
	}
	FString ResolvedProject;
	if (!ResolveDirectory(ProjectRoot, ResolvedProject))
	{
		Error = TEXT("Could not resolve the project directory.");
		return false;
	}
	TArray<FString> Parts;
	Absolute.Mid(ProjectRoot.Len() + 1).ParseIntoArray(Parts, TEXT("/"), true);
	FString Current = ProjectRoot;
	FString Expected = ResolvedProject;
	for (const FString& Part : Parts)
	{
		Current /= Part;
		Expected /= Part;
		if (!IFileManager::Get().DirectoryExists(*Current))
		{
			bool bCreated = false;
#if PLATFORM_WINDOWS
			// Capture actual creations, not MakeDirectory's successful "already exists" result.
			bCreated = CreateDirectoryW(*Current, nullptr) != 0;
			const bool bExists = bCreated || (GetLastError() == ERROR_ALREADY_EXISTS && IFileManager::Get().DirectoryExists(*Current));
#else
			bCreated = IFileManager::Get().MakeDirectory(*Current);
			const bool bExists = bCreated;
#endif
			if (!bExists)
			{
				Error = FString::Printf(TEXT("Could not create %s. Check folder permissions."), *Current);
				return false;
			}
			if (bCreated && CreatedDirectories) CreatedDirectories->Add(Current);
		}
		FString Resolved;
		if (!ResolveDirectory(Current, Resolved) || !Resolved.Equals(Expected, ESearchCase::IgnoreCase))
		{
			Error = FString::Printf(TEXT("The workspace path is redirected: %s. Choose a project with local workspace folders."), *Current);
			return false;
		}
	}
	return true;
}

void RemoveNewEmptyDirectories(const TArray<FString>& Directories)
{
	const FString ProjectRoot = Normalized(FPaths::ProjectDir());
	FString ResolvedProject;
	if (!ResolveDirectory(ProjectRoot, ResolvedProject)) return;
	for (int32 Index = Directories.Num() - 1; Index >= 0; --Index)
	{
		const FString& Directory = Directories[Index];
		FString Resolved;
		if (IsInside(Directory, ProjectRoot) && !IsReparsePoint(Directory) && ResolveDirectory(Directory, Resolved) &&
			Resolved.Equals(ResolvedProject / Directory.Mid(ProjectRoot.Len() + 1), ESearchCase::IgnoreCase))
		{
			// Only directories created by this operation, and only while empty. Never recurse.
			IFileManager::Get().DeleteDirectory(*Directory, false, false);
		}
	}
}

FString IndexFile(const FString& AssetId)
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ConvaiAvatarStudio/Assets"), FConvaiAvatarWorkspace::MakePluginName(AssetId) + TEXT(".json"));
}

void FillDirectories(FConvaiAvatarPreparedAsset& Asset)
{
	Asset.PluginDirectory = Normalized(FPaths::Combine(FConvaiAvatarWorkspace::GetAvatarsDirectory(), Asset.PluginName));
	Asset.ProxyDirectory = FConvaiAvatarWorkspace::GetProxyDirectory();
	Asset.ProxyPluginDirectory = Normalized(FPaths::Combine(Asset.ProxyDirectory, TEXT("Plugins/ConvaiAvatars"), Asset.PluginName));
}

bool ReadWorkspaceJson(const FString& Path, TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	return FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
}

bool WriteWorkspaceJson(const FString& Path, const TSharedRef<FJsonObject>& Object, FString& Error)
{
	if (IsReparsePoint(Path)) { Error = TEXT("The avatar metadata file is redirected and will not be overwritten."); return false; }
	if (!ManagedDirectory(FPaths::GetPath(Path), Error)) return false;
	FString Text;
	if (!FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text)) ||
		!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Error = FString::Printf(TEXT("Could not save avatar workspace metadata: %s"), *Path);
		return false;
	}
	return true;
}

bool SaveManifest(const FConvaiAvatarPreparedAsset& Asset, bool bReady, FString& Error)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("version"), ManifestVersion);
	Object->SetStringField(TEXT("asset_id"), Asset.AssetId);
	Object->SetStringField(TEXT("display_name"), Asset.DisplayName);
	Object->SetStringField(TEXT("plugin_name"), Asset.PluginName);
	Object->SetStringField(TEXT("entry_point"), Asset.EntryPoint.ToString());
	Object->SetStringField(TEXT("original_entry_point"), Asset.OriginalEntryPoint.ToString());
	Object->SetBoolField(TEXT("ready"), bReady);
	Object->SetBoolField(TEXT("staging"), Asset.bIsStaging);
	if (Asset.bHasMetaHumanChoice) Object->SetBoolField(TEXT("is_metahuman"), Asset.bIsMetaHuman);
	if (Asset.Diorama.IsSet()) Object->SetObjectField(TEXT("stage"), ConvaiAvatarDiorama::MakeRecord(Asset.Diorama.GetValue()));
	Object->SetBoolField(TEXT("include_convai_content"), Asset.bIncludeConvaiContent);
	Object->SetBoolField(TEXT("convai_content_pending"), Asset.bConvaiContentPending);
	Object->SetBoolField(TEXT("draft"), Asset.bIsDraft);
	Object->SetNumberField(TEXT("package_count"), Asset.PackageCount);
	TSharedRef<FJsonObject> Mappings = MakeShared<FJsonObject>();
	for (const auto& Pair : Asset.SourceToDestinationPackages) Mappings->SetStringField(Pair.Key.ToString(), Pair.Value.ToString());
	Object->SetObjectField(TEXT("packages"), Mappings);
	TSharedRef<FJsonObject> Retained = MakeShared<FJsonObject>();
	for (const auto& Pair : Asset.RetainedSourceToDestinationPackages) Retained->SetStringField(Pair.Key.ToString(), Pair.Value.ToString());
	Object->SetObjectField(TEXT("retained_package_mappings"), Retained);
	TSharedRef<FJsonObject> Hashes = MakeShared<FJsonObject>();
	for (const auto& Pair : Asset.SourcePackageHashes) Hashes->SetStringField(Pair.Key.ToString(), Pair.Value);
	Object->SetObjectField(TEXT("source_fingerprints"), Hashes);
	TSharedRef<FJsonObject> SourceFiles = MakeShared<FJsonObject>();
	for (const auto& Pair : Asset.SourceFileHashes) SourceFiles->SetStringField(Pair.Key, Pair.Value);
	Object->SetObjectField(TEXT("source_file_fingerprints"), SourceFiles);
	TSharedRef<FJsonObject> PreparedFiles = MakeShared<FJsonObject>();
	for (const auto& Pair : Asset.PreparedFileHashes) PreparedFiles->SetStringField(Pair.Key, Pair.Value);
	Object->SetObjectField(TEXT("prepared_file_fingerprints"), PreparedFiles);
	// Provenance is independent of cooking exclusions. Legacy records infer it from the original path.
	Object->SetStringField(TEXT("source_provenance"), Asset.OriginalEntryPoint.IsNull() ? TEXT("downloaded") : TEXT("project"));
	TArray<TSharedPtr<FJsonValue>> Plugins;
	for (const FString& Name : Asset.RequiredPlugins) Plugins.Add(MakeShared<FJsonValueString>(Name));
	Object->SetArrayField(TEXT("required_plugins"), Plugins);
	TArray<TSharedPtr<FJsonValue>> Missing;
	for (FName Name : Asset.AcknowledgedMissingPackages) Missing.Add(MakeShared<FJsonValueString>(Name.ToString()));
	Object->SetArrayField(TEXT("acknowledged_missing_packages"), Missing);
	return WriteWorkspaceJson(FPaths::Combine(Asset.PluginDirectory, ManifestName), Object, Error) && WriteWorkspaceJson(IndexFile(Asset.AssetId), Object, Error);
}

/** A cloud creation receipt survives a partial sidecar/index write; it must never become another Create. */
bool ReconcileBindingReceipt(const FString& Directory, TSharedPtr<FJsonObject>& Record, FString& Error, bool bAllowWrites = true)
{
	const FString ReceiptFile = Directory / BindingReceiptName;
	if (!IFileManager::Get().FileExists(*ReceiptFile)) return true;
	TSharedPtr<FJsonObject> Receipt, Canonical;
	FString OldId, NewId, PluginName, CurrentId, RecordedPlugin, CanonicalId, CanonicalPlugin;
	int32 CanonicalVersion = 0;
	if (IsReparsePoint(ReceiptFile) || !ReadWorkspaceJson(ReceiptFile, Receipt) || !ReadWorkspaceJson(Directory / ManifestName, Canonical) ||
		!Receipt->TryGetStringField(TEXT("previous_asset_id"), OldId) || !Receipt->TryGetStringField(TEXT("asset_id"), NewId) ||
		NewId.TrimStartAndEnd().IsEmpty() || NewId.Len() > 256 || OldId.IsEmpty() || OldId == NewId ||
		!Receipt->TryGetStringField(TEXT("plugin_name"), PluginName) || PluginName != FPaths::GetCleanFilename(Directory) ||
		!Record->TryGetStringField(TEXT("plugin_name"), RecordedPlugin) || RecordedPlugin != PluginName ||
		!Record->TryGetStringField(TEXT("asset_id"), CurrentId) || (CurrentId != OldId && CurrentId != NewId) ||
		!Canonical->TryGetStringField(TEXT("asset_id"), CanonicalId) || (CanonicalId != OldId && CanonicalId != NewId) ||
		!Canonical->TryGetNumberField(TEXT("version"), CanonicalVersion) || CanonicalVersion != ManifestVersion ||
		!Canonical->TryGetStringField(TEXT("plugin_name"), CanonicalPlugin) || CanonicalPlugin != PluginName)
	{
		Error = TEXT("This avatar has an invalid cloud binding receipt. Resolve its local record before attempting another upload.");
		return false;
	}
	TSharedPtr<FJsonObject> ExistingNewIndex;
	FString ExistingPlugin;
	if (ReadWorkspaceJson(IndexFile(NewId), ExistingNewIndex) && (!ExistingNewIndex->TryGetStringField(TEXT("plugin_name"), ExistingPlugin) || ExistingPlugin != PluginName))
	{
		Error = FString::Printf(TEXT("Cloud avatar %s already has a different local plugin. Its draft will not be uploaded as a new avatar."), *NewId);
		return false;
	}
	// The portable sidecar is authoritative if it was saved just before the index write failed.
	Record = Canonical;
	Record->SetStringField(TEXT("asset_id"), NewId);
	Record->SetBoolField(TEXT("draft"), false);
	if (!bAllowWrites) return true;
	if (!WriteWorkspaceJson(Directory / ManifestName, Record.ToSharedRef(), Error) || !WriteWorkspaceJson(IndexFile(NewId), Record.ToSharedRef(), Error))
	{
		Error += FString::Printf(TEXT(" The avatar already exists in the cloud as %s. Retry that avatar; do not create another one."), *NewId);
		return false;
	}
	IFileManager::Get().Delete(*IndexFile(OldId), false, true, true);
	IFileManager::Get().Delete(*ReceiptFile, false, true, true);
	return true;
}

/** Rebuild only from a complete-format canonical sidecar, never a pending download's minimal receipt. */
bool FindManifestDocument(const FString& AssetId, TSharedPtr<FJsonObject>& Object, bool& bFound, FString& Error, bool bAllowWrites = true)
{
	bFound = false;
	if (ReadWorkspaceJson(IndexFile(AssetId), Object))
	{
		FString PluginName;
		if (!Object->TryGetStringField(TEXT("plugin_name"), PluginName) || !FConvaiAvatarWorkspace::IsSafePluginName(PluginName))
		{
			Error = TEXT("The local avatar index has an invalid plugin name.");
			return false;
		}
		if (!ReconcileBindingReceipt(FConvaiAvatarWorkspace::GetAvatarsDirectory() / PluginName, Object, Error, bAllowWrites)) return false;
		FString EffectiveId;
		Object->TryGetStringField(TEXT("asset_id"), EffectiveId);
		if (EffectiveId != AssetId)
		{
			Error = FString::Printf(TEXT("This draft is already bound to cloud avatar %s. Select that avatar to retry the upload."), *EffectiveId);
			return false;
		}
		bFound = true;
		return true;
	}
	const FString Root = FConvaiAvatarWorkspace::GetAvatarsDirectory();
	if (!IFileManager::Get().DirectoryExists(*Root)) return true;
	if (IsReparsePoint(Root)) { Error = TEXT("The canonical avatar folder is redirected; local records cannot be recovered safely."); return false; }
	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *(Root / TEXT("*")), false, true);
	TSharedPtr<FJsonObject> Match;
	for (const FString& Folder : Folders)
	{
		if (!FConvaiAvatarWorkspace::IsSafePluginName(Folder)) continue;
		const FString Directory = Root / Folder;
		const FString Sidecar = Directory / ManifestName;
		if (IsReparsePoint(Directory) || IsReparsePoint(Sidecar) || IFileManager::Get().FileExists(*(Directory / TEXT("ConvaiAvatarPendingInstall.json")))) continue;
		TSharedPtr<FJsonObject> Candidate;
		FString Id, PluginName, Entry;
		int32 Version = 0;
		bool bReady = false;
		if (!ReadWorkspaceJson(Sidecar, Candidate) || !Candidate->TryGetStringField(TEXT("asset_id"), Id)) continue;
		if (!Candidate->TryGetNumberField(TEXT("version"), Version) || Version != ManifestVersion ||
			!Candidate->TryGetStringField(TEXT("plugin_name"), PluginName) || PluginName != Folder ||
			!Candidate->TryGetBoolField(TEXT("ready"), bReady) || !Candidate->TryGetStringField(TEXT("entry_point"), Entry) ||
			!FSoftObjectPath(Entry).IsValid() || !FSoftObjectPath(Entry).GetLongPackageName().StartsWith(TEXT("/") + Folder + TEXT("/"))) continue;
		TSharedPtr<FJsonObject> Receipt;
		FString BoundId;
		if (ReadWorkspaceJson(Directory / BindingReceiptName, Receipt)) Receipt->TryGetStringField(TEXT("asset_id"), BoundId);
		if (Id != AssetId && BoundId != AssetId) continue;
		if (!ReconcileBindingReceipt(Directory, Candidate, Error, bAllowWrites)) return false;
		Candidate->TryGetStringField(TEXT("asset_id"), Id);
		if (Id != AssetId)
		{
			Error = FString::Printf(TEXT("This draft is already bound to cloud avatar %s. Select that avatar to retry the upload."), *Id);
			return false;
		}
		if (Match.IsValid()) { Error = TEXT("More than one canonical plugin claims this asset ID. Resolve the duplicate local records before continuing."); return false; }
		Match = Candidate;
	}
	if (!Match.IsValid()) return true;
	if (bAllowWrites && !WriteWorkspaceJson(IndexFile(AssetId), Match.ToSharedRef(), Error)) return false;
	Object = Match;
	bFound = true;
	return true;
}

bool ReadManifest(const FString& AssetId, FConvaiAvatarPreparedAsset& Asset, bool& bReady, FString& Error, bool bAllowWrites = true)
{
	TSharedPtr<FJsonObject> Object;
	bool bFound = false;
	if (!FindManifestDocument(AssetId, Object, bFound, Error, bAllowWrites)) return false;
	if (!bFound)
	{
		Error = TEXT("This avatar has no local prepared source. Download its source or prepare a Blueprint first.");
		return false;
	}
	int32 Version = 0;
	if (!Object->TryGetNumberField(TEXT("version"), Version) || Version != ManifestVersion ||
		!Object->TryGetStringField(TEXT("asset_id"), Asset.AssetId) || Asset.AssetId != AssetId ||
		!Object->TryGetStringField(TEXT("plugin_name"), Asset.PluginName) || !FConvaiAvatarWorkspace::IsSafePluginName(Asset.PluginName))
	{
		Error = TEXT("The local avatar manifest is invalid or belongs to another asset.");
		return false;
	}
	FillDirectories(Asset);
	if (IsReparsePoint(Asset.PluginDirectory) || IsReparsePoint(Asset.PluginDirectory / ManifestName) ||
		IsReparsePoint(Asset.PluginDirectory / (Asset.PluginName + TEXT(".uplugin"))))
	{
		Error = TEXT("The local avatar source is redirected and cannot be managed safely.");
		return false;
	}
	Object->TryGetStringField(TEXT("display_name"), Asset.DisplayName);
	FString EntryPoint, Original;
	Object->TryGetStringField(TEXT("entry_point"), EntryPoint);
	Object->TryGetStringField(TEXT("original_entry_point"), Original);
	Asset.EntryPoint = FSoftObjectPath(EntryPoint);
	Asset.OriginalEntryPoint = FSoftObjectPath(Original);
	FString Provenance;
	if (Object->TryGetStringField(TEXT("source_provenance"), Provenance) &&
		((Provenance != TEXT("project") && Provenance != TEXT("downloaded")) ||
		(Provenance == TEXT("project") && !Asset.OriginalEntryPoint.IsValid()) ||
		(Provenance == TEXT("downloaded") && !Asset.OriginalEntryPoint.IsNull())))
	{ Error = TEXT("The saved avatar source location is inconsistent. Restore its workspace record before uploading."); return false; }
	if (!Asset.EntryPoint.IsValid() || !Asset.EntryPoint.GetLongPackageName().StartsWith(TEXT("/") + Asset.PluginName + TEXT("/")))
	{
		Error = TEXT("The local avatar entry point is outside its managed plugin.");
		return false;
	}
	Object->TryGetBoolField(TEXT("ready"), bReady);
	Object->TryGetBoolField(TEXT("staging"), Asset.bIsStaging);
	Asset.bHasMetaHumanChoice = false;
	Asset.bIsMetaHuman = false;
	if (const TSharedPtr<FJsonValue> Choice = Object->TryGetField(TEXT("is_metahuman")))
	{
		if (Choice->Type != EJson::Boolean) { Error = TEXT("The saved avatar's MetaHuman choice is invalid. Restore its local record before uploading."); return false; }
		Asset.bIsMetaHuman = Choice->AsBool();
		Asset.bHasMetaHumanChoice = true;
	}
	Asset.Diorama.Reset();
	if (const TSharedPtr<FJsonValue> Diorama = Object->TryGetField(TEXT("stage")))
	{
		FConvaiAvatarDioramaRecord Record;
		if (Diorama->Type != EJson::Object || !ConvaiAvatarDiorama::ReadRecord(Diorama->AsObject(), Record))
		{ Error = TEXT("The saved avatar's diorama record is invalid. Restore its local record before uploading."); return false; }
		Asset.Diorama = MoveTemp(Record);
	}
	Object->TryGetBoolField(TEXT("draft"), Asset.bIsDraft);
	Asset.bIncludeConvaiContent = false;
	if (const TSharedPtr<FJsonValue> Include = Object->TryGetField(TEXT("include_convai_content")))
	{
		if (Include->Type != EJson::Boolean) { Error = TEXT("The saved avatar's Convai content choice is invalid. Restore its local record before uploading."); return false; }
		Asset.bIncludeConvaiContent = Include->AsBool();
	}
	Asset.bConvaiContentPending = false;
	if (const TSharedPtr<FJsonValue> Pending = Object->TryGetField(TEXT("convai_content_pending")))
	{
		if (Pending->Type != EJson::Boolean) { Error = TEXT("The saved avatar's content update state is invalid."); return false; }
		Asset.bConvaiContentPending = Pending->AsBool();
		if (Asset.bConvaiContentPending && (!Asset.bIncludeConvaiContent || bReady)) { Error = TEXT("The saved avatar's content update state is inconsistent."); return false; }
	}
	Asset.AcknowledgedMissingPackages.Reset();
	if (const TSharedPtr<FJsonValue> MissingField = Object->TryGetField(TEXT("acknowledged_missing_packages")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Missing;
		if (!MissingField->TryGetArray(Missing) || Missing->Num() > MaximumPackages)
		{ Error = TEXT("The saved avatar's missing-reference review is invalid."); return false; }
		for (const TSharedPtr<FJsonValue>& Value : *Missing)
		{
			FString Name;
			if (!Value.IsValid() || !Value->TryGetString(Name) || Name.Len() > 1023 || !FPackageName::IsValidTextForLongPackageName(Name) ||
				Name.StartsWith(TEXT("/Script/")) || FName(*Name) == FName(*Asset.EntryPoint.GetLongPackageName()))
			{ Error = TEXT("The saved avatar's missing-reference review contains an invalid package path."); return false; }
			Asset.AcknowledgedMissingPackages.AddUnique(FName(*Name));
		}
		Asset.AcknowledgedMissingPackages.Sort(FNameLexicalLess());
	}
	Object->TryGetNumberField(TEXT("package_count"), Asset.PackageCount);
	const TSharedPtr<FJsonObject>* Map;
	if (Object->TryGetObjectField(TEXT("packages"), Map))
	{
		for (const auto& Pair : (*Map)->Values)
		{
			FString Destination;
			if (!Pair.Value->TryGetString(Destination) || !Destination.StartsWith(TEXT("/") + Asset.PluginName + TEXT("/")))
			{
				Error = TEXT("The avatar manifest contains a package outside its plugin.");
				return false;
			}
			Asset.SourceToDestinationPackages.Add(FName(*Pair.Key), FName(*Destination));
		}
	}
	if (Object->HasField(TEXT("retained_package_mappings")))
	{
		if (!Object->TryGetObjectField(TEXT("retained_package_mappings"), Map) || (*Map)->Values.Num() > MaximumPackages) { Error = TEXT("The retained avatar copy records are invalid."); return false; }
		TSet<FName> Targets;
		for (const auto& Pair : (*Map)->Values)
		{
			FString Destination;
			const FString Source(*Pair.Key);
			if (!FPackageName::IsValidTextForLongPackageName(Source) || Source.StartsWith(TEXT("/Script/")) || Source.StartsWith(TEXT("/") + Asset.PluginName + TEXT("/")) || !Pair.Value->TryGetString(Destination) ||
				Destination != FConvaiAvatarWorkspace::MakeDestinationPackage(FName(*Source), Asset.PluginName).ToString())
			{ Error = TEXT("A retained avatar copy is outside its recorded source-to-plugin mapping."); return false; }
			if (Targets.Contains(FName(*Destination)) || Asset.RetainedSourceToDestinationPackages.Contains(FName(*Source))) { Error = TEXT("The retained avatar copy records contain duplicate paths."); return false; }
			Targets.Add(FName(*Destination));
			Asset.RetainedSourceToDestinationPackages.Add(FName(*Source), FName(*Destination));
		}
	}
	if (Object->TryGetObjectField(TEXT("source_fingerprints"), Map))
	{
		for (const auto& Pair : (*Map)->Values)
		{
			FString Value;
			if (Pair.Value->TryGetString(Value)) Asset.SourcePackageHashes.Add(FName(*Pair.Key), Value);
		}
	}
	auto ReadFingerprints = [&](const TCHAR* Field, TMap<FString, FString>& Target)
	{
		const TSharedPtr<FJsonObject>* Values;
		if (!Object->TryGetObjectField(Field, Values)) return true; // A legacy record has no trustworthy copied-file baseline.
		for (const auto& Pair : (*Values)->Values)
		{
			FString Hash;
			if (!Pair.Value->TryGetString(Hash) || Hash.Len() != 32) return false;
			for (TCHAR Character : Hash) if (!FChar::IsHexDigit(Character)) return false;
			Target.Add(FString(*Pair.Key), Hash);
		}
		return true;
	};
	if (!ReadFingerprints(TEXT("source_file_fingerprints"), Asset.SourceFileHashes) || !ReadFingerprints(TEXT("prepared_file_fingerprints"), Asset.PreparedFileHashes))
	{ Error = TEXT("The saved avatar revision fingerprints are invalid. Restore its workspace record before refreshing."); return false; }
	const TArray<TSharedPtr<FJsonValue>>* Plugins;
	if (Object->TryGetArrayField(TEXT("required_plugins"), Plugins))
	{
		for (const auto& Value : *Plugins)
		{
			FString Name;
			if (Value->TryGetString(Name) && FConvaiAvatarWorkspace::IsSafePluginName(Name)) Asset.RequiredPlugins.AddUnique(Name);
		}
	}
	TSharedPtr<FJsonObject> Ownership;
	FString OwnerId;
	if (!ReadWorkspaceJson(FPaths::Combine(Asset.PluginDirectory, ManifestName), Ownership) ||
		!Ownership->TryGetStringField(TEXT("asset_id"), OwnerId) || OwnerId != AssetId)
	{
		Error = TEXT("The avatar plugin does not match its workspace record. It will not be overwritten.");
		return false;
	}
	return true;
}

TArray<UPackage*> LoadedAvatarPackages(const FString& PluginName)
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

void ReleaseNodeTemplatesForRefresh(const FString& PluginName, const TSet<FName>& PreservedPackages)
{
	const FString Mount = TEXT("/") + PluginName + TEXT("/");
	for (TObjectIterator<UBlueprintNodeSpawner> It; It; ++It)
	{
		if (It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) continue;
		UEdGraphNode* TemplateNode = It->GetCachedTemplateNode();
		if (!TemplateNode || !FBlueprintNodeTemplateCache::IsTemplateOuter(TemplateNode->GetGraph())) continue;

		// Blueprint action menus cache prototype nodes even after their editors close.
		// Inspect only that disposable node and its owned subobjects, never referenced
		// user graphs or assets. Include transient hard references because they retain GC objects too.
		TArray<UObject*> TemplateObjects {TemplateNode};
		GetPreparationObjectsWithOuter(TemplateNode, TemplateObjects, true);
		bool bReferencesAvatar = false;
		for (UObject* TemplateObject : TemplateObjects)
		{
			TArray<UObject*> References;
			FReferenceFinder Finder(References, nullptr, false, true, false, false);
			Finder.FindReferences(TemplateObject);
			for (UObject* Reference : References)
			{
				if (Reference && Reference->GetOutermost()->GetName().StartsWith(Mount) &&
					!PreservedPackages.Contains(Reference->GetOutermost()->GetFName()))
				{
					bReferencesAvatar = true;
					break;
				}
			}
			if (bReferencesAvatar) break;
		}
		if (bReferencesAvatar) It->ClearCachedTemplateNode();
	}
}

bool UnloadForRefreshPreserving(const FString& PluginName, const TSet<FName>& PreservedPackages, FString& Error)
{
	TArray<UPackage*> Loaded = LoadedAvatarPackages(PluginName);
	for (UPackage* Package : Loaded)
	{
		if (Package->IsDirty()) { Error = FString::Printf(TEXT("Save or discard the changes in %s before replacing its prepared copy."), *Package->GetName()); return false; }
	}
	Loaded.RemoveAll([&PreservedPackages](UPackage* Package) { return PreservedPackages.Contains(Package->GetFName()); });
	TArray<TWeakObjectPtr<UObject>> RemovedActionOwners;
	ON_SCOPE_EXIT
	{
		// A real editor/scene/undo reference may still prevent unloading. Restore the
		// surviving assets' actions in that case; never reload an old collected asset.
		// Successful replacement repopulates actions through AssetCreated/OnAssetLoaded.
		if (FBlueprintActionDatabase* Database = FBlueprintActionDatabase::TryGet())
		{
			for (const TWeakObjectPtr<UObject>& Owner : RemovedActionOwners)
			{
				if (UObject* Asset = Owner.Get()) Database->RefreshAssetActions(Asset);
			}
		}
	};
	if (FBlueprintActionDatabase* Database = FBlueprintActionDatabase::TryGet())
	{
		const FString Mount = TEXT("/") + PluginName + TEXT("/");
		TArray<TWeakObjectPtr<UObject>> ActionCandidates;
		for (UPackage* Package : Loaded)
		{
			TArray<UObject*> PackageObjects;
			GetObjectsWithPackage(Package, PackageObjects);
			for (UObject* Object : PackageObjects)
			{
				if (Object->GetOutermost()->GetName().StartsWith(Mount)) ActionCandidates.Add(Object);
			}
		}
		// Clearing entries can broadcast editor notifications, so do it outside the
		// UObject hash-table walk. Field spawners themselves can retain enums/structs
		// even when they have no cached template node.
		for (const TWeakObjectPtr<UObject>& Candidate : ActionCandidates)
		{
			if (UObject* Object = Candidate.Get())
			{
				if (Database->ClearAssetActions(Object)) RemovedActionOwners.Add(Candidate);
			}
		}
	}
	ReleaseNodeTemplatesForRefresh(PluginName, PreservedPackages);
	UPackageTools::FUnloadPackageParams Params(Loaded);
	Params.bResetTransBuffer = false;
	Params.bUnloadDirtyPackages = false;
	if (!Loaded.IsEmpty()) UPackageTools::UnloadPackages(Params);
	// UE returns whether any package changed, not whether all packages unloaded. Do not
	// replace a package still held by an editor, scene instance, or the user's undo history.
	TArray<UPackage*> Retained = LoadedAvatarPackages(PluginName);
	Retained.RemoveAll([&PreservedPackages](UPackage* Package) { return PreservedPackages.Contains(Package->GetFName()); });
	if (!Retained.IsEmpty())
	{
		TArray<FString> Names;
		for (UPackage* Package : Retained) if (Names.Num() < 8) Names.Add(Package->GetName());
		Error = TEXT("Unreal is using these prepared avatar files. Save your work, close their asset editors, and open an empty level before retrying Upload changes. If they remain in undo history, restart Unreal with the empty level open. Your saved scene keeps its actors, and the original project Blueprint remains editable.\n") + FString::Join(Names, TEXT("\n"));
		return false;
	}
	return true;
}

bool UnloadForRefresh(const FString& PluginName, FString& Error)
{
	return UnloadForRefreshPreserving(PluginName, {}, Error);
}

TSharedPtr<IPlugin> PluginForMount(const FString& Package)
{
	int32 Slash = Package.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
	return Slash == INDEX_NONE ? nullptr : IPluginManager::Get().FindPlugin(Package.Mid(1, Slash - 1));
}

bool AddScriptDependency(const FString& Package, TArray<FString>& RequiredPlugins, FString& Error)
{
	const FString ModuleName = Package.Mid(8);
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
	{
		for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
		{
			if (Module.Name.ToString() == ModuleName)
			{
				if (Module.Type != EHostType::Editor && Module.Type != EHostType::EditorNoCommandlet && Module.Type != EHostType::EditorAndProgram)
				{
					if (!Plugin->IsEnabled())
					{
						Error = FString::Printf(TEXT("Enable the %s plugin before preparing this avatar."), *Plugin->GetName());
						return false;
					}
					RequiredPlugins.AddUnique(Plugin->GetName());
				}
				return true;
			}
		}
	}
    FString ModuleFile;
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    FModuleStatus ModuleStatus;
    if (FModuleManager::Get().QueryModule(FName(*ModuleName), ModuleStatus)) ModuleFile = ModuleStatus.FilePath;
    const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
    const bool bProjectModule = Project && Project->Modules.ContainsByPredicate(
        [&ModuleName](const FModuleDescriptor& Module) { return Module.Name == FName(*ModuleName); });
    if (bProjectModule || (!ModuleFile.IsEmpty() && IsInside(ModuleFile, FPaths::ProjectDir())))
#else
    if (FModuleManager::Get().ModuleExists(*ModuleName, &ModuleFile) && IsInside(ModuleFile, FPaths::ProjectDir()))
#endif
	{
		Error = FString::Printf(TEXT("This avatar needs project C++ module %s. Move that runtime code into a reusable plugin before preparing it."), *ModuleName);
		return false;
	}
	return true; // Engine modules, including editor-only serialization metadata, remain external.
}

bool MountPlugin(const FConvaiAvatarPreparedAsset& Asset, FString& Error)
{
	if (!ManagedDirectory(Asset.PluginDirectory, Error)) return false;
	const TSharedPtr<IPlugin> Existing = IPluginManager::Get().FindPlugin(Asset.PluginName);
	if (Existing.IsValid() && !Normalized(Existing->GetBaseDir()).Equals(Asset.PluginDirectory, ESearchCase::IgnoreCase))
	{
		Error = FString::Printf(TEXT("A different plugin already uses the name %s. It will not be replaced."), *Asset.PluginName);
		return false;
	}
	// UE's reference-domain rebuild observes newly mounted plugins immediately.
	// Migrate owned legacy root exclusions before that event, including retries.
	if (Asset.bIsStaging && !FConvaiAvatarProjectConfiguration::SetStagingCookExclusion(FPaths::ProjectDir(), Asset.PluginName, true, Error)) return false;
	FText Reason;
	if (!IPluginManager::Get().AddToPluginsList(FPaths::Combine(Asset.PluginDirectory, Asset.PluginName + TEXT(".uplugin")), &Reason))
	{
		Error = Reason.ToString();
		return false;
	}
	if (!FPackageName::MountPointExists(TEXT("/") + Asset.PluginName + TEXT("/")))
		IPluginManager::Get().MountNewlyCreatedPlugin(Asset.PluginName);
	if (!FPackageName::MountPointExists(TEXT("/") + Asset.PluginName + TEXT("/")))
	{
		Error = TEXT("Unreal could not mount the avatar plugin. Restart the editor and try again.");
		return false;
	}
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanPathsSynchronous({TEXT("/") + Asset.PluginName}, true);
	return true;
}

bool SavePackage(UPackage* Package, FString& Error)
{
	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
	if (!ManagedDirectory(FPaths::GetPath(Filename), Error)) return false;
	if (Package->ContainsMap())
	{
		if (UEditorLoadingAndSavingUtils::SavePackages({Package}, false)) return true;
		Error = FString::Printf(TEXT("Could not save %s. See the Output Log."), *Package->GetName());
		return false;
	}
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	FStringOutputDevice SaveMessages;
	Args.SaveFlags = SAVE_None;
	Args.Error = &SaveMessages;
	if (!UPackage::SavePackage(Package, nullptr, *Filename, Args))
	{
		Error = FString::Printf(TEXT("Could not save %s. %s"), *Package->GetName(), *SaveMessages.TrimStartAndEnd());
		return false;
	}
	return true;
}

UWorld* LoadDioramaWorld(FName PackageName, FString& Error)
{
	UPackage* Package = LoadPackage(nullptr, *PackageName.ToString(), LOAD_None);
	UWorld* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
	if (!World || !World->PersistentLevel) Error = TEXT("Could not load the diorama level: ") + PackageName.ToString();
	return World;
}

void AddDioramaPackages(UWorld* World, TArray<UPackage*>& Packages)
{
	Packages.AddUnique(World->GetPackage());
	for (UPackage* External : World->GetPackage()->GetExternalPackages())
		if (!UPackage::IsEmptyPackage(External)) Packages.AddUnique(External);
	if (World->PersistentLevel->MapBuildData) Packages.AddUnique(World->PersistentLevel->MapBuildData->GetPackage());
}

void ScanDioramaFiles(UWorld* World)
{
	TArray<UPackage*> Packages;
	AddDioramaPackages(World, Packages);
	TArray<FString> Files;
	for (UPackage* Package : Packages)
		Files.Add(FPackageName::LongPackageNameToFilename(Package->GetName(), Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension()));
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanModifiedAssetFiles(Files);
}

bool DioramaCopyMatches(UWorld* World, const FConvaiAvatarDioramaFacts& Facts, FString& Error)
{
	int32 Actors = 0;
	bool bCameraFound = Facts.CameraActorName.IsNone();
	for (AActor* Actor : World->PersistentLevel->Actors)
	{
		if (!IsValid(Actor)) continue;
		if (Facts.AvatarActorNames.Contains(Actor->GetFName()))
		{
			Error = TEXT("The diorama copy does not match the scan; rescan.");
			return false;
		}
		if (Actor->GetFName() == Facts.CameraActorName && Actor->Tags.Contains(TEXT("Convai.Diorama.Camera"))) bCameraFound = true;
		if (!Actor->IsChildActor() && !Actor->IsEditorOnly() && !Actor->HasAnyFlags(RF_Transient) && Actor != World->PersistentLevel->GetDefaultBrush() &&
			!Actor->IsA<AWorldSettings>() && !Actor->IsA<ALevelScriptActor>()) ++Actors;
	}
	if (Actors != Facts.Actors || !bCameraFound)
	{
		Error = TEXT("The diorama copy does not match the scan; rescan.");
		return false;
	}
	return true;
}

bool DeleteDioramaLevel(FName DestinationName, FString& Error)
{
	UEditorAssetSubsystem* Assets = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (!Assets) return false;
	if (FPackageName::DoesPackageExist(DestinationName.ToString()))
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(DestinationName.ToString(), FPackageName::GetMapPackageExtension());
		bool bDeletedAsset;
		{
			// UE 5.8's new finder misidentifies external-package references and permanently switches algorithms.
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
			FScopedCVar<int32> ReferenceFinder(TEXT("Editor.UseLegacyGetReferencersForDeletion"), 1);
#endif
			bDeletedAsset = Assets->DeleteAsset(DestinationName.ToString());
		}
		// World deletion can unload the package while leaving its map file on disk.
		if (bDeletedAsset && IFileManager::Get().FileExists(*Filename) && !FindPackage(nullptr, *DestinationName.ToString()))
		{
			IFileManager::Get().Delete(*Filename, true);
		}
		if (!bDeletedAsset || IFileManager::Get().FileExists(*Filename))
		{
			Error = TEXT("Could not replace the previous diorama copy at ") + DestinationName.ToString();
			return false;
		}
		// The editor deletion event can leave the disk registry entry until its directory watcher ticks.
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanModifiedAssetFiles({Filename});
		// Deleting an old map reloads its prepared imports. Clear them before copying any replacement.
		if (!UnloadForRefresh(FPackageName::GetPackageMountPoint(DestinationName.ToString()).ToString(), Error)) return false;
	}
	return true;
}

bool DeletePreviousDiorama(const FConvaiAvatarPreparedAsset& Previous, FName Replacement, FString& Error)
{
	if (!Previous.Diorama.IsSet() || FName(*Previous.Diorama->Level) == Replacement) return true;
	const FString Mount = TEXT("/") + Previous.PluginName + TEXT("/");
	const FName OldLevel(*Previous.Diorama->Level);
	if (!Previous.Diorama->Level.StartsWith(Mount) || !Previous.SourceToDestinationPackages.FindKey(OldLevel))
	{
		Error = TEXT("The previous diorama level is not owned by this avatar. Restore its local record before refreshing.");
		return false;
	}
	return DeleteDioramaLevel(OldLevel, Error);
}

bool DuplicateDioramaLevel(FName SourceName, FName DestinationName, const FConvaiAvatarDioramaFacts& Facts, FString& Error)
{
	UWorld* Source = LoadDioramaWorld(SourceName, Error);
	UEditorAssetSubsystem* Assets = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (!Source || !Assets || !DeleteDioramaLevel(DestinationName, Error)) return false;
	UWorld* Copy = Cast<UWorld>(Assets->DuplicateLoadedAsset(Source, DestinationName.ToString()));
	if (!Copy || !Copy->PersistentLevel)
	{
		Error = TEXT("Could not copy the diorama level into the plugin: ") + SourceName.ToString();
		return false;
	}
	int32 Removed = 0;
	const TArray<TObjectPtr<AActor>> Actors = Copy->PersistentLevel->Actors;
	for (AActor* Actor : Actors)
	{
		if (!IsValid(Actor)) continue;
		if (Facts.AvatarActorNames.Contains(Actor->GetFName()))
		{
			if (Copy->EditorDestroyActor(Actor, false)) ++Removed;
		}
		else if (Actor->GetFName() == Facts.CameraActorName)
		{
			Actor->Modify();
			Actor->Tags.AddUnique(TEXT("Convai.Diorama.Camera"));
		}
	}
	if (Removed != Facts.AvatarActorNames.Num() || !DioramaCopyMatches(Copy, Facts, Error))
	{
		Error = TEXT("The diorama copy does not match the scan; rescan.");
		return false;
	}
	if (!SavePackage(Copy->GetPackage(), Error)) return false;
	ScanDioramaFiles(Copy);
	return true;
}

/** Duplicate every top-level asset, including packages whose assets are not named after the package.
 * The refresh caller has unloaded the entire owned destination mount. Never ask Advanced Copy to
 * delete/reload an old disk asset: that API can silently skip assets retained by another old import. */
bool DuplicatePreparedPackage(FName SourceName, FName DestinationName, FString& Error)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> SourceAssets;
	Registry.GetAssetsByPackageName(SourceName, SourceAssets);
	if (SourceAssets.IsEmpty()) { Error = TEXT("No asset was found in dependency package ") + SourceName.ToString(); return false; }
	TArray<UObject*> Originals;
	for (const FAssetData& SourceAsset : SourceAssets)
	{
		UObject* Original = SourceAsset.GetAsset();
		if (!Original) { Error = TEXT("Could not load dependency ") + SourceAsset.GetObjectPathString(); return false; }
		if (Original->GetOutermost()->HasAnyPackageFlags(PKG_FilterEditorOnly))
		{ Error = TEXT("This dependency is cooked or missing editable source data: ") + SourceAsset.GetObjectPathString(); return false; }
		Originals.Add(Original);
	}
	UPackage* Destination = CreatePackage(*DestinationName.ToString());
	TArray<UObject*> ExistingObjects;
	GetPreparationObjectsWithOuter(Destination, ExistingObjects, false);
	if (ExistingObjects.ContainsByPredicate([](UObject* Object) { return Object->IsAsset(); }))
	{
		Error = TEXT("An old prepared dependency was loaded again while copying: ") + DestinationName.ToString() +
			TEXT(". Close its editors or restart Unreal before refreshing; its saved files have been preserved.");
		return false;
	}
	for (UObject* Original : Originals)
	{
		FObjectDuplicationParameters Params = InitStaticDuplicateObjectParams(Original, Destination, Original->GetFName(), RF_AllFlags & ~RF_Transient);
		UObject* Duplicate = StaticDuplicateObjectEx(Params);
		if (!Duplicate || Duplicate->GetClass() != Original->GetClass()) { Error = TEXT("Could not duplicate dependency ") + Original->GetPathName(); return false; }
		Duplicate->SetFlags(RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(Duplicate);
	}
	// CreatePackage may reuse an empty unloaded UPackage with an older file on disk. All source
	// exports are present now; do not reload the old file or treat this replacement as partially loaded.
	Destination->MarkAsFullyLoaded();
	return SavePackage(Destination, Error);
}

bool ReinstanceDioramaActors(UWorld* Copy, const TMap<FName, FName>& Packages, FString& Error)
{
	TArray<UObject*> Objects;
	GetPreparationObjectsWithOuter(Copy, Objects, true);
	TSet<UClass*> UsedClasses;
	for (UObject* Object : Objects) if (IsValid(Object)) UsedClasses.Add(Object->GetClass());
	TMap<UClass*, UClass*> Classes;
	TMap<UClass*, TMap<UObject*, UObject*>> Templates;
	for (const auto& Pair : Packages)
	{
		UPackage* Source = FindPackage(nullptr, *Pair.Key.ToString());
		if (!Source) continue;
		TArray<UObject*> Exports;
		GetPreparationObjectsWithOuter(Source, Exports, false);
		for (UObject* Export : Exports)
		{
			UBlueprint* Blueprint = Cast<UBlueprint>(Export);
			if (!Blueprint || !Blueprint->GeneratedClass || !UsedClasses.Contains(Blueprint->GeneratedClass)) continue;
			const FString Path = Pair.Value.ToString() + Blueprint->GetPathName().Mid(Pair.Key.ToString().Len());
			UBlueprint* Prepared = FindObject<UBlueprint>(nullptr, *Path);
			if (!Prepared || !Prepared->GeneratedClass || Prepared->GeneratedClass == Blueprint->GeneratedClass) continue;
			Classes.Add(Blueprint->GeneratedClass, Prepared->GeneratedClass);
			TMap<UObject*, UObject*>& TemplateMap = Templates.Add(Blueprint->GeneratedClass);
			UObject* SourceCDO = Blueprint->GeneratedClass->GetDefaultObject();
			UObject* DestinationCDO = Prepared->GeneratedClass->GetDefaultObject();
			TemplateMap.Add(SourceCDO, DestinationCDO);
		}
	}
	if (Classes.IsEmpty()) return true;
	TSet<UObject*> ProtectedObjects;
	for (TObjectIterator<UObject> It; It; ++It)
		if (*It != Copy && !It->IsIn(Copy)) ProtectedObjects.Add(*It);
	FReplaceInstancesOfClassParameters Params;
	Params.InstancesThatShouldUseOldClass = &ProtectedObjects;
	Params.ObjectsThatShouldUseOldStuff = &ProtectedObjects;
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
    Params.OldToNewTemplates = &Templates;
#endif
	FBlueprintCompileReinstancer::BatchReplaceInstancesOfClass(Classes, Params);
	Objects.Reset();
	GetPreparationObjectsWithOuter(Copy, Objects, true);
	for (UObject* Object : Objects)
	{
		if (IsValid(Object) && Classes.Contains(Object->GetClass()))
		{
			Error = TEXT("A diorama object still uses its source Blueprint class: ") + Object->GetPathName();
			return false;
		}
	}
	TArray<UPackage*> ChangedPackages;
	AddDioramaPackages(Copy, ChangedPackages);
	for (UPackage* Package : ChangedPackages) Package->MarkPackageDirty();
	return true;
}

/** A reference archive changes class properties, but cannot change an existing UObject's
 * actual class. Recreate only copied SCS/child-actor templates through engine APIs, which
 * preserve their overrides, then remap the new objects and references to the old ones. */
bool RepairPreparedObjectTemplates(const TArray<UPackage*>& Packages, const FString& Mount,
	TMap<UObject*, UObject*>& ObjectMap, TMap<FSoftObjectPath, FSoftObjectPath>& SoftMap,
	const TFunction<bool()>& Cancelled, FString& Error)
{
	TArray<TPair<UPackage*, UPackage*>> PackagePairs;
	TSet<UObject*> RetiredTemplateObjects;
	for (const auto& Pair : ObjectMap)
		if (UPackage* Source = Cast<UPackage>(Pair.Key))
			if (UPackage* Destination = Cast<UPackage>(Pair.Value)) PackagePairs.Emplace(Source, Destination);
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	// A recreated object can itself own component/child-actor templates. Revisit only these
	// owned packages; do not follow references into source assets or a level.
	for (int32 Pass = 0; Pass < 32; ++Pass)
	{
		bool bRepaired = false;
		for (UPackage* Package : Packages)
		{
			if (!Package || !Package->GetName().StartsWith(Mount))
			{ Error = TEXT("The copied avatar template is outside its owned package."); return false; }
			TArray<UObject*> Objects;
			GetPreparationObjectsWithOuter(Package, Objects, true);
			for (UObject* Object : Objects)
			{
				if (Cancelled && Cancelled()) { Error = TEXT("Avatar preparation was cancelled."); return false; }
				// A previous repair can move an old nested template to /Engine/Transient.
				if (Object->GetOutermost() != Package) continue;
				USCS_Node* Node = Cast<USCS_Node>(Object);
				UChildActorComponent* ChildComponent = Cast<UChildActorComponent>(Object);
				UObject* PreviousTemplate = Node ? static_cast<UObject*>(Node->ComponentTemplate.Get()) :
					(ChildComponent ? static_cast<UObject*>(ChildComponent->GetChildActorTemplate()) : nullptr);
				UClass* ExpectedClass = Node ? Node->ComponentClass.Get() :
					(ChildComponent ? ChildComponent->GetChildActorClass().Get() : nullptr);
				if (!PreviousTemplate || !ExpectedClass || PreviousTemplate->GetClass() == ExpectedClass) continue;
				if (ObjectMap.FindRef(PreviousTemplate->GetClass()) != ExpectedClass) continue;
				bool bSafe = PreviousTemplate->GetOutermost() == Package && PreviousTemplate->IsTemplate() &&
					!PreviousTemplate->HasAnyFlags(RF_ClassDefaultObject) && ExpectedClass->GetOutermost()->GetName().StartsWith(Mount);
				if (Node)
				{
					const USimpleConstructionScript* SCS = Cast<USimpleConstructionScript>(Node->GetOuter());
					const UActorComponent* ComponentTemplate = Cast<UActorComponent>(PreviousTemplate);
					bSafe &= SCS && PreviousTemplate->GetOuter() == SCS->GetOwnerClass() && ComponentTemplate &&
						!ComponentTemplate->IsRegistered() && !ComponentTemplate->GetWorld() &&
						ExpectedClass->IsChildOf(UActorComponent::StaticClass()) &&
						ExpectedClass->GetPropertiesSize() >= PreviousTemplate->GetClass()->GetPropertiesSize();
				}
				else
				{
					bSafe &= ChildComponent && ChildComponent->IsTemplate() && !ChildComponent->IsRegistered() &&
						!ChildComponent->GetWorld() && !ChildComponent->GetChildActor() && PreviousTemplate->GetOuter() == ChildComponent;
				}
				if (!bSafe)
				{
					UE_LOG(LogTemp, Warning, TEXT("Cloud Avatars cannot rebuild an unsafe copied object template: %s"), *Object->GetPathName());
					Error = TEXT("Cloud Avatars could not finish updating a copied component. Choose Open log and share the log."); return false;
				}
				const FString PreviousPath = PreviousTemplate->GetPathName();
				TArray<UObject*> PreviousObjects { PreviousTemplate };
				GetPreparationObjectsWithOuter(PreviousTemplate, PreviousObjects, true);
				TMap<UObject*, FString> PreviousPaths;
				for (UObject* PreviousObject : PreviousObjects)
				{
					PreviousPaths.Add(PreviousObject, PreviousObject->GetPathName());
					RetiredTemplateObjects.Add(PreviousObject);
				}
				UObject* ExpectedOuter = PreviousTemplate->GetOuter();
				UObject* NewTemplate = nullptr;
				if (Node)
				{
					// DestClass is an exact copied Blueprint class. UE switches to tagged
					// property serialization for differing classes and duplicates owned
					// subobjects, retaining component defaults and author overrides.
					const FName PreviousName = PreviousTemplate->GetFName();
					FObjectDuplicationParameters Parameters(PreviousTemplate, GetTransientPackage());
					Parameters.DestClass = ExpectedClass;
					Parameters.DestName = MakeUniqueObjectName(GetTransientPackage(), ExpectedClass, PreviousName);
					Parameters.FlagMask &= ~RF_WasLoaded;
					NewTemplate = StaticDuplicateObjectEx(Parameters);
					if (!NewTemplate || !PreviousTemplate->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional) ||
						!NewTemplate->Rename(*PreviousName.ToString(), ExpectedOuter, REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional))
					{ Error = TEXT("Cloud Avatars could not rebuild a copied component. Choose Open log and share the log."); return false; }
					Node->ComponentTemplate = CastChecked<UActorComponent>(NewTemplate);
				}
				else
				{
					// Null asks UE to preserve the old child actor's property overrides.
					// The guards above exclude the branch that spawns a live child actor.
					ChildComponent->SetChildActorClass(ExpectedClass, nullptr);
					NewTemplate = ChildComponent->GetChildActorTemplate();
				}
				if (!NewTemplate || NewTemplate->GetClass() != ExpectedClass || NewTemplate->GetOuter() != ExpectedOuter)
				{ Error = TEXT("Cloud Avatars could not rebuild a copied component. Choose Open log and share the log."); return false; }
				for (const auto& Previous : PreviousPaths)
				{
					const FString NewPath = NewTemplate->GetPathName() + Previous.Value.Mid(PreviousPath.Len());
					if (UObject* Replacement = StaticFindObject(nullptr, nullptr, *NewPath))
					{
						ObjectMap.Add(Previous.Key, Replacement);
						if (Previous.Value != NewPath) SoftMap.Add(FSoftObjectPath(Previous.Value), FSoftObjectPath(Replacement));
					}
				}
				bRepaired = true;
			}
		}
		if (!bRepaired) return true;
		// The old map contains templates that the engine just moved to the transient
		// package. Resolve original export/subobject suffixes again, including any new
		// template subobjects, before another archive can reintroduce those old objects.
		for (const auto& Pair : PackagePairs)
		{
			TArray<UObject*> SourceObjects;
			GetPreparationObjectsWithOuter(Pair.Key, SourceObjects, true);
			for (UObject* SourceObject : SourceObjects)
			{
				const FString TargetPath = Pair.Value->GetName() + SourceObject->GetPathName().Mid(Pair.Key->GetName().Len());
				if (UObject* Target = StaticFindObject(nullptr, nullptr, *TargetPath))
				{
					ObjectMap.Add(SourceObject, Target);
					SoftMap.Add(FSoftObjectPath(SourceObject), FSoftObjectPath(Target));
				}
			}
		}
		for (auto& Pair : ObjectMap)
		{
			TSet<UObject*> Visited;
			while (RetiredTemplateObjects.Contains(Pair.Value) && !Pair.Value->GetOutermost()->GetName().StartsWith(Mount))
			{
				UObject* Replacement = ObjectMap.FindRef(Pair.Value);
				if (!Replacement || Visited.Contains(Pair.Value))
				{
					UE_LOG(LogTemp, Warning, TEXT("Cloud Avatars could not preserve a copied template subobject: %s"), *Pair.Value->GetPathName());
					Error = TEXT("Cloud Avatars could not preserve a copied component's settings. Choose Open log and share the log."); return false;
				}
				Visited.Add(Pair.Value);
				Pair.Value = Replacement;
			}
		}
		for (UPackage* Package : Packages)
		{
			TArray<UObject*> Objects;
			GetPreparationObjectsWithOuter(Package, Objects, true);
			for (UObject* Object : Objects)
			{
				ConvaiAvatarReferenceRemap::RemapNiagaraTypes(Object->GetClass(), Object, ObjectMap);
				ConvaiAvatarReferenceRemap::RemapNativeReferences(Object, ObjectMap);
				FArchiveReplaceObjectRef<UObject> Replace(Object, ObjectMap,
					EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
			}
			AssetTools.RenameReferencingSoftObjectPaths({Package}, SoftMap);
		}
	}
	Error = TEXT("Cloud Avatars could not finish updating nested components. Choose Open log and share the log.");
	return false;
}

bool CompilePreparedBlueprints(const TArray<UPackage*>& Packages, const TFunction<bool()>& Cancelled, FString& Error)
{
	TSet<UBlueprint*> Blueprints;
	for (UPackage* Package : Packages)
	{
		TArray<UObject*> Exports;
		GetPreparationObjectsWithOuter(Package, Exports, false);
		for (UObject* Export : Exports) if (UBlueprint* Blueprint = Cast<UBlueprint>(Export)) Blueprints.Add(Blueprint);
	}
	FScopedSlowTask Progress(static_cast<float>(Blueprints.Num()), NSLOCTEXT("ConvaiAvatarStudio", "CompilePreparedBlueprints", "Compiling prepared avatar Blueprints…"));
	Progress.MakeDialogDelayed(0.5f, true);
	TSet<UBlueprint*> Compiled, Visiting;
	TFunction<bool(UBlueprint*)> Compile = [&](UBlueprint* Blueprint)
	{
		if (Compiled.Contains(Blueprint)) return true;
		if ((Cancelled && Cancelled()) || Progress.ShouldCancel()) { Error = TEXT("Avatar preparation was cancelled while compiling its prepared Blueprints."); return false; }
		if (Visiting.Contains(Blueprint)) { Error = TEXT("The prepared Blueprint inheritance contains a cycle: ") + Blueprint->GetPathName(); return false; }
		Visiting.Add(Blueprint);
		UBlueprint* Parent = Blueprint->ParentClass ? Cast<UBlueprint>(Blueprint->ParentClass->ClassGeneratedBy) : nullptr;
		if (Parent && Blueprints.Contains(Parent))
		{
			if (!Compile(Parent)) return false;
			// Recompiling the parent can replace its generated class. Always compile the child
			// against the final prepared parent, preserving the same logical inheritance.
			Blueprint->ParentClass = Parent->GeneratedClass;
		}
		Progress.EnterProgressFrame(1, FText::FromString(TEXT("Compiling ") + Blueprint->GetName()));
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Results);
		if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass || Results.NumErrors > 0)
		{
			Error = TEXT("The prepared Blueprint could not compile after dependency remapping: ") + Blueprint->GetPathName() + TEXT(". Check its compiler messages in the Output Log.");
			return false;
		}
		Visiting.Remove(Blueprint);
		Compiled.Add(Blueprint);
		return true;
	};
	for (UBlueprint* Blueprint : Blueprints) if (!Compile(Blueprint)) return false;
	return true;
}

bool IsWorldPackage(const TArray<FAssetData>& Assets)
{
	for (const FAssetData& Asset : Assets)
	{
		if ((Asset.PackageFlags & PKG_ContainsMap) != 0 || Asset.AssetClassPath == UWorld::StaticClass()->GetClassPathName()) return true;
	}
	return false;
}

bool SharedRuntimeContent(const FString& Package, bool bIncludeConvaiContent = false)
{
	return (!bIncludeConvaiContent && Package.StartsWith(TEXT("/ConvAI/"), ESearchCase::IgnoreCase)) ||
		Package.StartsWith(TEXT("/ConvaiHTTP/"), ESearchCase::IgnoreCase) ||
		Package.StartsWith(TEXT("/Engine/EditorBlueprintResources/")) || Package.StartsWith(TEXT("/Engine/EditorResources/"));
}

void ResolveLegacyMetaHumanChoice(FConvaiAvatarPreparedAsset& Asset, const UBlueprint* Blueprint)
{
	check(IsInGameThread());
	if (!Asset.bHasMetaHumanChoice && Blueprint)
	{
		Asset.bIsMetaHuman = ConvaiAvatarStudio::BlueprintSetup::IsMetaHuman(Blueprint);
		Asset.bHasMetaHumanChoice = true;
	}
}

bool SetupPreparedBlueprint(FConvaiAvatarPreparedAsset& Asset, FString& Error, bool bSaveChanges = true)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset.EntryPoint.TryLoad());
	if (!Blueprint) { Error = TEXT("The prepared avatar Blueprint could not be loaded."); return false; }
	ResolveLegacyMetaHumanChoice(Asset, Blueprint);
	TArray<FString> Changes;
	TMap<FName, FName> SetupMappings = Asset.RetainedSourceToDestinationPackages;
	SetupMappings.Append(Asset.SourceToDestinationPackages);
	if (!ConvaiAvatarStudio::BlueprintSetup::PrepareAvatarBlueprint(Blueprint, Asset.bIsMetaHuman, Error, Changes, &SetupMappings)) return false;
	if (Blueprint->Status == BS_Error) { Error = TEXT("The prepared avatar Blueprint has compile errors after adding Convai components."); return false; }
	if (bSaveChanges && !Changes.IsEmpty())
	{
		if (!SavePackage(Blueprint->GetOutermost(), Error)) return false;
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanModifiedAssetFiles(
			{FPackageName::LongPackageNameToFilename(Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension())});
	}
	return true;
}

bool SetupAndCompilePreparedBlueprints(FConvaiAvatarPreparedAsset& Asset, const TArray<UPackage*>& Packages,
	const TFunction<bool()>& Cancelled, FString& Error)
{
	if (Cancelled && Cancelled()) { Error = TEXT("Avatar preparation was cancelled before setting up its copied Blueprint."); return false; }
	// A dependent Blueprint can read a component that setup adds to the selected actor.
	// Establish that selected class first, then validate the complete remapped closure.
	// Saving belongs to the caller, after every dependent Blueprint compiles successfully.
	return SetupPreparedBlueprint(Asset, Error, false) && CompilePreparedBlueprints(Packages, Cancelled, Error);
}

bool RemapAndCompilePreparedPackages(FConvaiAvatarPreparedAsset& Asset, const TArray<UPackage*>& Packages, const FString& Mount,
	TMap<UObject*, UObject*>& ObjectMap, TMap<FSoftObjectPath, FSoftObjectPath>& SoftMap, const TFunction<bool()>& Cancelled, FString& Error)
{
	// Frames belong to the old class hierarchy. Release every copied instance before changing any superclass.
	ON_SCOPE_EXIT
	{
		for (UPackage* Package : Packages)
		{
			TArray<UObject*> Objects;
			GetPreparationObjectsWithOuter(Package, Objects, true);
			for (UObject* Object : Objects)
				if (IsValid(Object)) Object->GetClass()->CreatePersistentUberGraphFrame(Object, true);
		}
	};
	for (UPackage* Package : Packages)
	{
		TArray<UObject*> Objects;
		GetPreparationObjectsWithOuter(Package, Objects, true);
		for (UObject* Object : Objects)
			if (IsValid(Object)) Object->GetClass()->DestroyPersistentUberGraphFrame(Object);
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	FScopedSlowTask Progress(static_cast<float>(Packages.Num()), NSLOCTEXT("ConvaiAvatarStudio", "RemapPreparedPackages", "Updating prepared avatar references…"));
	Progress.MakeDialogDelayed(0.5f, true);
	for (UPackage* Package : Packages)
	{
		if ((Cancelled && Cancelled()) || Progress.ShouldCancel()) { Error = TEXT("Avatar preparation was cancelled. Refresh from source to complete the saved staging copy."); return false; }
		Progress.EnterProgressFrame(1, FText::FromString(FString::Printf(TEXT("Updating references in %s"), *FPackageName::GetShortName(Package->GetName()))));
		TArray<UObject*> Objects;
		GetPreparationObjectsWithOuter(Package, Objects, true);
		for (UObject* Object : Objects)
		{
			ConvaiAvatarReferenceRemap::RemapNiagaraTypes(Object->GetClass(), Object, ObjectMap);
			ConvaiAvatarReferenceRemap::RemapNativeReferences(Object, ObjectMap);
			FArchiveReplaceObjectRef<UObject> Replace(Object, ObjectMap, EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
		}
		AssetTools.RenameReferencingSoftObjectPaths({Package}, SoftMap);
	}
	// StaticDuplicateObject invokes Blueprint PostDuplicate before the complete reference map
	// exists. Set up the selected copy before its dependents read newly added components,
	// then rebuild every prepared Blueprint against its final remapped hierarchy.
	return RepairPreparedObjectTemplates(Packages, Mount, ObjectMap, SoftMap, Cancelled, Error) &&
		SetupAndCompilePreparedBlueprints(Asset, Packages, Cancelled, Error);
}

bool UpdatePluginDependencies(const FConvaiAvatarPreparedAsset& Asset, FString& Error)
{
	const FString DescriptorFile = FPaths::Combine(Asset.PluginDirectory, Asset.PluginName + TEXT(".uplugin"));
	if (IsReparsePoint(DescriptorFile)) { Error = TEXT("The generated avatar plugin descriptor is redirected and will not be changed."); return false; }
	FPluginDescriptor Descriptor;
	FText Reason;
	if (!Descriptor.Load(DescriptorFile, Reason)) { Error = Reason.ToString(); return false; }
	Descriptor.EnabledByDefault = EPluginEnabledByDefault::Enabled;
	for (const FString& Name : Asset.RequiredPlugins)
	{
		FPluginReferenceDescriptor* Existing = Descriptor.Plugins.FindByPredicate([&Name](const FPluginReferenceDescriptor& Plugin) { return Plugin.Name.Equals(Name, ESearchCase::IgnoreCase); });
		if (Existing) Existing->bEnabled = true;
		else Descriptor.Plugins.Emplace(Name, true);
	}
	if (IsReparsePoint(DescriptorFile)) { Error = TEXT("The generated avatar plugin descriptor is redirected and will not be changed."); return false; }
	if (!Descriptor.Save(DescriptorFile, Reason)) { Error = Reason.ToString(); return false; }
	return true;
}

void GatherLoadedDependencyPackages(UPackage* Package, TArray<FName>& Dependencies)
{
	TSet<FName> ReferencedPackages;
	auto AddPath = [&ReferencedPackages](const FSoftObjectPath& Path)
	{
		const FString Name = Path.GetLongPackageName();
		if (FPackageName::IsValidTextForLongPackageName(Name) && !Name.StartsWith(TEXT("/Script/")) &&
			!Name.StartsWith(TEXT("/Temp/")) && Name != GetTransientPackage()->GetName()) ReferencedPackages.Add(FName(*Name));
	};
	TArray<UObject*> Objects;
	GetObjectsWithPackage(Package, Objects);
	for (UObject* Object : Objects)
	{
		if (Object->HasAnyFlags(RF_Transient)) continue;
		TArray<UObject*> References;
		FReferenceFinder Finder(References, nullptr, false, true, false, true);
		Finder.FindReferences(Object);
		for (UObject* Reference : References)
		{
			if (Reference && !Reference->HasAnyFlags(RF_Transient) && Reference->GetOutermost() != GetTransientPackage()) AddPath(FSoftObjectPath(Reference));
		}
		// Walk values only; do not load soft references or invoke an object's native
		// serializer. This includes freshly edited struct/array/map/set properties.
		for (FPropertyValueIterator It(FProperty::StaticClass(), Object->GetClass(), Object); It; ++It)
		{
			if (It.Key()->HasAnyPropertyFlags(CPF_Transient | CPF_SkipSerialization)) { It.SkipRecursiveProperty(); continue; }
			if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(It.Key())) AddPath(Soft->GetPropertyValue(It.Value()).ToSoftObjectPath());
			else if (const FStructProperty* Struct = CastField<FStructProperty>(It.Key()))
			{
				if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get()) AddPath(*static_cast<const FSoftObjectPath*>(It.Value()));
			}
		}
	}
	Dependencies.Append(ReferencedPackages.Array());
}

/** Default preparation rejects dirty packages. The optional read-only save review also follows loaded edits. */
bool Gather(const FName Root, const FString& DestinationMount, TMap<FName, FName>& Mapping,
	TArray<FString>& RequiredPlugins, TArray<FName>& Closure, TArray<FName>& MissingPackages,
	const TFunction<bool()>& Cancel, FString& Error, FName DioramaRoot = NAME_None, TArray<UPackage*>* DirtyPackages = nullptr,
	bool bIncludeConvaiContent = false, const TArray<FName>* AdditionalRoots = nullptr)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.WaitForCompletion();
	TArray<FName> Queue {Root};
	if (AdditionalRoots) Queue.Append(*AdditionalRoots);
	TSet<FName> Seen;
	FScopedSlowTask Progress(0, NSLOCTEXT("ConvaiAvatarStudio", "InspectDependencies", "Checking avatar dependencies…"));
	Progress.MakeDialogDelayed(0.5f, true);
	for (int32 Index = 0; Index < Queue.Num(); ++Index)
	{
		if ((Cancel && Cancel()) || Progress.ShouldCancel()) { Error = TEXT("Avatar preparation was cancelled."); return false; }
		const FName Package = Queue[Index];
		if (Seen.Contains(Package)) continue;
		Seen.Add(Package);
		if (Seen.Num() > MaximumPackages) { Error = TEXT("This Blueprint references too many packages. Remove scene/world references and try again."); return false; }
		const FString Name = Package.ToString();
		if (DirtyPackages && Name.StartsWith(TEXT("/ConvaiAvatar_")) && !Name.StartsWith(DestinationMount)) continue;
		Progress.EnterProgressFrame(0, FText::FromString(FString::Printf(TEXT("Checking %s (%d packages)"), *FPackageName::GetShortName(Name), Seen.Num())));
		if (Name.StartsWith(TEXT("/Script/")))
		{
			if (!AddScriptDependency(Name, RequiredPlugins, Error)) return false;
			continue;
		}
		if (!FPackageName::IsValidTextForLongPackageName(Name)) { Error = FString::Printf(TEXT("Invalid dependency package: %s"), *Name); return false; }
		UPackage* Loaded = FindPackage(nullptr, *Name);
		// Editor-generated exports need not carry RF_Transient themselves (for
		// example dynamic editor-data structs). Their owning package decides
		// whether they can be saved. Exclude them from save review, but retain
		// normal validation of any broken references already saved on disk.
		if (Loaded && (Loaded == GetTransientPackage() || Loaded->HasAnyFlags(RF_Transient)))
		{
			if (Package == Root) { Error = TEXT("Choose a saved avatar Blueprint from the Content Browser, not a temporary editor object."); return false; }
			if (DirtyPackages) continue;
		}
		if (!DirtyPackages && Loaded && Loaded->IsDirty()) { Error = FString::Printf(TEXT("Save your changes before creating and uploading the avatar. Unsaved asset: %s"), *Name); return false; }
		FString Filename;
		const bool bSaved = FPackageName::DoesPackageExist(Name, &Filename);
		if (!bSaved && !(DirtyPackages && Loaded))
		{
			if (Package == Root) { Error = TEXT("The selected avatar Blueprint has no saved package: ") + Name; return false; }
			MissingPackages.AddUnique(Package);
			continue; // Collect the complete reachable list; an absent package has no editable source to copy.
		}
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(Package, Assets);
		// The declared diorama is a deliberate upload input, so its own edits are reviewable; any other level is not.
		if (DirtyPackages && Package != DioramaRoot && ((Loaded && Loaded->ContainsMap()) || IsWorldPackage(Assets))) continue;
		if (IsWorldPackage(Assets) && Package != DioramaRoot) { Error = FString::Printf(TEXT("This avatar references a level (%s). Cloud Avatars V1 supports avatar Blueprints without scene assets."), *Name); return false; }
		if (DirtyPackages && Loaded && (Loaded->IsDirty() || !bSaved)) DirtyPackages->AddUnique(Loaded);
		Closure.AddUnique(Package);
		const bool bDestination = Name.StartsWith(DestinationMount);
		if (SharedRuntimeContent(Name, bIncludeConvaiContent))
		{
			if (const TSharedPtr<IPlugin> Plugin = PluginForMount(Name)) RequiredPlugins.AddUnique(Plugin->GetName());
			continue; // These runtime resources are already shipped by Convai products.
		}
		const bool bCopy = !bDestination;
		if (!bDestination && !Name.StartsWith(TEXT("/Engine/")) && !Name.StartsWith(TEXT("/Game/")))
		{
			const TSharedPtr<IPlugin> Plugin = PluginForMount(Name);
			if (!Plugin.IsValid() || !Plugin->IsEnabled()) { Error = FString::Printf(TEXT("The plugin required by %s is unavailable or disabled."), *Name); return false; }
			if (!Plugin->GetDescriptor().Modules.IsEmpty()) RequiredPlugins.AddUnique(Plugin->GetName());
		}
		if (bCopy) Mapping.Add(Package, FConvaiAvatarWorkspace::MakeDestinationPackage(Package, DestinationMount.Mid(1, DestinationMount.Len() - 2)));
		TArray<FName> Dependencies;
		Registry.GetDependencies(Package, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
		if (DirtyPackages && Loaded) GatherLoadedDependencyPackages(Loaded, Dependencies);
		Queue.Append(Dependencies);
		if (Package == DioramaRoot)
		{
			UWorld* World = LoadDioramaWorld(Package, Error);
			if (!World) return false;
			TArray<UPackage*> WorldPackages;
			AddDioramaPackages(World, WorldPackages);
			for (UPackage* WorldPackage : WorldPackages) Queue.Add(WorldPackage->GetFName());
		}
	}
	RequiredPlugins.Sort();
	MissingPackages.Sort(FNameLexicalLess());
	return true;
}

bool MapDioramaPackages(UWorld* World, FName Destination, const FConvaiAvatarDioramaFacts& Facts,
	TMap<FName, FName>& Mapping, TSet<FName>& GeneratedPackages, FString& Error)
{
	const FName Source = World->GetPackage()->GetFName();
	Mapping.Add(Source, Destination);
	GeneratedPackages.Add(Source);
	const FString SourceLeaf = FPackageName::GetShortName(Source);
	const FString DestinationLeaf = FPackageName::GetShortName(Destination);
	const FString ReplaceFrom = SourceLeaf + TEXT(".") + SourceLeaf + TEXT(":");
	const FString ReplaceTo = DestinationLeaf + TEXT(".") + DestinationLeaf + TEXT(":");
	for (UPackage* External : World->GetPackage()->GetExternalPackages())
	{
		if (UPackage::IsEmptyPackage(External)) continue;
		GeneratedPackages.Add(External->GetFName());
		Mapping.Remove(External->GetFName());
		bool bFoundObject = false;
		ForEachObjectWithPackage(External, [&](UObject* Object)
		{
			if (Object->GetExternalPackage() != External || !Object->IsAsset()) return true;
			bFoundObject = true;
			if (AActor* Actor = Cast<AActor>(Object); Actor && Facts.AvatarActorNames.Contains(Actor->GetFName())) return false;
			// Match the engine's duplication seed: external filenames hash the renamed source object path.
			const FString ObjectPath = Object->GetPathName().Replace(*ReplaceFrom, *ReplaceTo, ESearchCase::CaseSensitive);
			const FString Target = Object->IsA<AActor>()
				? ULevel::GetActorPackageName(ULevel::GetExternalActorsPath(Destination.ToString()), World->PersistentLevel->GetActorPackagingScheme(), ObjectPath)
				: FExternalPackageHelper::GetExternalPackageName(Destination.ToString(), ObjectPath);
			Mapping.Add(External->GetFName(), FName(*Target));
			return false;
		},
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			false);
#else
			EGetObjectsFlags::None);
#endif
		if (!bFoundObject)
		{
			Error = TEXT("Could not resolve an external object belonging to the diorama: ") + External->GetName();
			return false;
		}
	}
	if (World->PersistentLevel->MapBuildData)
	{
		const FName BuiltData = World->PersistentLevel->MapBuildData->GetPackage()->GetFName();
		if (BuiltData != Source)
		{
			Mapping.Add(BuiltData, FName(*(Destination.ToString() + TEXT("_BuiltData"))));
			GeneratedPackages.Add(BuiltData);
		}
	}
	return true;
}

bool GatherRequestedDiorama(const FConvaiAvatarPrepareRequest& Request, const FString& Mount,
	TMap<FName, FName>& Mapping, TArray<FString>& RequiredPlugins, TArray<FName>& Closure, TArray<FName>& Missing, FString& Error,
	TArray<UPackage*>* DirtyPackages = nullptr)
{
	if (!Request.bIncludeDiorama) return true;
	if (!FPackageName::IsValidLongPackageName(Request.DioramaSourceLevel))
	{
		Error = TEXT("Choose a saved level for the diorama before preparing it.");
		return false;
	}
	const FName Source(*Request.DioramaSourceLevel);
	return Gather(Source, Mount, Mapping, RequiredPlugins, Closure, Missing, Request.IsCancelled, Error, Source, DirtyPackages);
}

bool GatherForPreparation(UBlueprint* Blueprint, const FString& Mount, bool bIsMetaHuman, bool bIncludeConvaiContent,
	TMap<FName, FName>& Mapping, TArray<FString>& RequiredPlugins, TArray<FName>& Closure, TArray<FName>& Missing,
	const TFunction<bool()>& Cancel, FString& Error, FName DioramaRoot = NAME_None, TArray<UPackage*>* DirtyPackages = nullptr)
{
	TArray<FName> SetupPackages;
	// Setup changes only our eventual copy. Include precisely its planned SDK additions
	// in both the review and source fingerprint so an unchanged retry has the same graph.
	if (bIncludeConvaiContent && !Blueprint->GetOutermost()->GetName().StartsWith(Mount) &&
		!ConvaiAvatarStudio::BlueprintSetup::GetSetupDependencies(Blueprint, bIsMetaHuman, SetupPackages, Error)) return false;
	return Gather(Blueprint->GetOutermost()->GetFName(), Mount, Mapping, RequiredPlugins, Closure, Missing, Cancel, Error,
		DioramaRoot, DirtyPackages, bIncludeConvaiContent, &SetupPackages);
}

bool ReviewMissingPackages(const TArray<FName>& Missing, const TArray<FName>& Acknowledged,
	FConvaiAvatarDependencyReview& Review, FString& Error)
{
	Review = {};
	Review.MissingPackages = Missing;
	if (Acknowledged.Num() > MaximumPackages)
	{ Error = TEXT("The missing-reference review contains too many package paths."); return false; }
	for (FName Name : Acknowledged)
	{
		const FString Path = Name.ToString();
		if (Path.Len() > 1023 || !FPackageName::IsValidTextForLongPackageName(Path) || Path.StartsWith(TEXT("/Script/")))
		{ Error = TEXT("The missing-reference review must contain exact asset package paths."); return false; }
	}
	Review.bRequiresAcknowledgement = Missing.ContainsByPredicate([&Acknowledged](FName Name) { return !Acknowledged.Contains(Name); });
	return true;
}

// Prepare and ReviewSourceChanges must walk the identical package graph. A graph that differs
// by even one diorama package yields a different consent fingerprint, and the refresh a user
// approved in the review dialog would then be rejected by Prepare as an unreviewed revision.
bool GatherRevisionGraph(const FConvaiAvatarPrepareRequest& Request, UBlueprint* Blueprint, bool bIsMetaHuman, bool bIncludeConvaiContent,
	const FString& Mount, const FString& PluginName, TMap<FName, FName>& Mapping, TArray<FString>& RequiredPlugins, TArray<FName>& Closure,
	TArray<FName>& Missing, TSet<FName>& GeneratedDiorama, FName& OutDioramaDestination, UWorld*& OutDioramaWorld, FString& Error)
{
	const FName DioramaSource = Request.bIncludeDiorama ? FName(*Request.DioramaSourceLevel) : NAME_None;
	if (!GatherForPreparation(Blueprint, Mount, bIsMetaHuman, bIncludeConvaiContent, Mapping, RequiredPlugins, Closure, Missing,
		Request.IsCancelled, Error, DioramaSource) ||
		!GatherRequestedDiorama(Request, Mount, Mapping, RequiredPlugins, Closure, Missing, Error)) return false;
	if (!Request.bIncludeDiorama) return true;
	OutDioramaWorld = LoadDioramaWorld(DioramaSource, Error);
	if (!OutDioramaWorld) return false;
	OutDioramaDestination = FName(*(FConvaiAvatarWorkspace::MakeDestinationPackage(DioramaSource, PluginName).ToString() + TEXT("_Diorama")));
	return MapDioramaPackages(OutDioramaWorld, OutDioramaDestination, Request.DioramaFacts, Mapping, GeneratedDiorama, Error);
}

bool CaptureRevisionSource(const TMap<FName, FName>& Mapping, const TSet<FName>& GeneratedDiorama,
	TMap<FString, FString>& Files, TMap<FName, FString>& Hashes, const TFunction<bool()>& Cancelled, FString& Error)
{
	TMap<FName, FName> Packages = Mapping;
	// A diorama actor stripped from the copy is never mapped, but editing one still changes the map that is.
	for (FName Package : GeneratedDiorama)
		if (!Packages.Contains(Package) && FPackageName::DoesPackageExist(Package.ToString())) Packages.Add(Package, NAME_None);
	return ConvaiAvatarSourceRevision::CaptureSource(Packages, Files, Hashes, Cancelled, Error);
}

FString MissingReviewMessage(const FConvaiAvatarDependencyReview& Review)
{
	TArray<FString> Paths;
	for (FName Name : Review.MissingPackages) Paths.Add(Name.ToString());
	return TEXT("Review the missing asset references before continuing:\n") + FString::Join(Paths, TEXT("\n"));
}

FString UnresolvedDependencyMessage(const FConvaiAvatarPreparedAsset& Asset, const TMap<FName, FName>& Unresolved,
	const TArray<FName>& Closure, const TCHAR* Phase)
{
	TArray<FName> Sources; Unresolved.GetKeys(Sources); Sources.Sort(FNameLexicalLess());
	const FString Mount = TEXT("/") + Asset.PluginName + TEXT("/");
	FName FirstOwned;
	int32 ReportedPackages = 0, ReportedDetails = 0;
	for (FName Source : Sources)
	{
		const FName* Expected = Asset.SourceToDestinationPackages.Find(Source);
		if (!Expected) Expected = Asset.RetainedSourceToDestinationPackages.Find(Source);
		if (!Expected || *Expected != FConvaiAvatarWorkspace::MakeDestinationPackage(Source, Asset.PluginName)) continue;
		if (FirstOwned.IsNone()) FirstOwned = Source;
		if (++ReportedPackages > 3) break;
		const FString ExpectedFile = Asset.PluginDirectory / TEXT("Content") / Expected->ToString().Mid(Mount.Len()) + FPackageName::GetAssetPackageExtension();
		UE_LOG(LogTemp, Warning, TEXT("Cloud Avatars reference repair diagnostic: phase=%s; source=%s; expected=%s; expected_file_exists=%s"),
			Phase, *Source.ToString(), *Expected->ToString(), IFileManager::Get().FileExists(*ExpectedFile) ? TEXT("true") : TEXT("false"));
		TArray<FString> Details;
		const int32 Limit = FMath::Min(6, 12 - ReportedDetails);
		for (FName PackageName : Closure)
		{
			if (Details.Num() >= Limit) break;
			if (!PackageName.ToString().StartsWith(Mount)) continue;
			UPackage* Package = FindPackage(nullptr, *PackageName.ToString());
			if (!Package) continue; // Diagnostics must not load or modify further source assets.
			TArray<UObject*> Objects; GetPreparationObjectsWithOuter(Package, Objects, true);
			Objects.Sort([](const UObject& A, const UObject& B) { return A.GetPathName() < B.GetPathName(); });
			for (UObject* Object : Objects)
			{
				if (Details.Num() >= Limit) break;
				ConvaiAvatarReferenceRemap::DescribeReferencesToPackage(Object, Source, Limit, Details);
			}
		}
		for (const FString& Detail : Details)
			UE_LOG(LogTemp, Warning, TEXT("Cloud Avatars reference repair diagnostic: %s; expected=%s"), *Detail, *Expected->ToString());
		ReportedDetails += Details.Num();
		if (Details.IsEmpty() && Limit > 0)
			UE_LOG(LogTemp, Warning, TEXT("Cloud Avatars reference repair diagnostic: no matching persisted property/class was found among loaded prepared objects; source=%s; expected=%s"),
				*Source.ToString(), *Expected->ToString());
	}
	if (!FirstOwned.IsNone())
		return FString::Printf(TEXT("Cloud Avatars could not finish updating a copied asset: %s. Try again. If this happens again, choose Open log and share the log."), *FirstOwned.ToString());
	const FString Source = Sources.IsEmpty() ? FString() : Sources[0].ToString();
	return Asset.OriginalEntryPoint.IsNull()
		? FString::Printf(TEXT("Cloud Avatars could not prepare an asset referenced by this avatar: %s. Try again. If this happens again, choose Open log and share the log."), *Source)
		: FString::Printf(TEXT("Cloud Avatars could not prepare an asset referenced by your project Blueprint: %s. Save any changes to that Blueprint and its referenced assets, then try again. If this happens again, choose Open log and share the log."), *Source);
}

/** Add only the SDK subgraph actually referenced by the prepared entry. Existing mapped
 * copies are reused, including user edits; other external references remain validation errors. */
bool IncludeReferencedConvaiContent(FConvaiAvatarPreparedAsset& Asset, const TFunction<bool()>& Cancel, FString& Error)
{
	if (!Asset.bIncludeConvaiContent) return true;
	const FString Mount = TEXT("/") + Asset.PluginName + TEXT("/");
	TMap<FName, FName> Unresolved;
	TArray<FName> Closure, Missing;
	TArray<FString> Required;
	if (!Gather(FName(*Asset.EntryPoint.GetLongPackageName()), Mount, Unresolved, Required, Closure, Missing, Cancel, Error)) return false;
	if (!Unresolved.IsEmpty()) { Error = UnresolvedDependencyMessage(Asset, Unresolved, Closure, TEXT("include-content")); return false; }
	TArray<FName> Seeds;
	for (FName Package : Closure) if (Package.ToString().StartsWith(TEXT("/ConvAI/"), ESearchCase::IgnoreCase)) Seeds.Add(Package);
	if (Seeds.IsEmpty()) { Asset.bConvaiContentPending = false; return true; }
	TMap<FName, FName> Additions;
	TArray<FName> SdkClosure, SdkMissing;
	TArray<FString> SdkPlugins;
	if (!Gather(Seeds[0], Mount, Additions, SdkPlugins, SdkClosure, SdkMissing, Cancel, Error, NAME_None, nullptr, true, &Seeds)) return false;
	Missing.Append(SdkMissing);
	for (FName Package : Missing)
		if (Package.ToString().StartsWith(Mount)) { Error = TEXT("A file is missing from this avatar's plugin: ") + Package.ToString(); return false; }
	FConvaiAvatarDependencyReview MissingReview;
	if (!ReviewMissingPackages(Missing, Asset.AcknowledgedMissingPackages, MissingReview, Error)) return false;
	if (MissingReview.bRequiresAcknowledgement) { Error = MissingReviewMessage(MissingReview); return false; }
	TMap<FName, FName> ToCopy;
	for (const auto& Pair : Additions)
	{
		const FString DestinationFile = Asset.PluginDirectory / TEXT("Content") / Pair.Value.ToString().Mid(Mount.Len());
		const bool bExists = IFileManager::Get().FileExists(*(DestinationFile + FPackageName::GetAssetPackageExtension()));
		const FName* Existing = Asset.SourceToDestinationPackages.Find(Pair.Key);
		if (!Existing) Existing = Asset.RetainedSourceToDestinationPackages.Find(Pair.Key);
		if (bExists && (!Existing || *Existing != Pair.Value)) { Error = TEXT("The avatar already contains an unrelated file at ") + Pair.Value.ToString(); return false; }
		if (!bExists) ToCopy.Add(Pair.Key, Pair.Value);
	}
	TMap<FString, FString> BeforeFiles, SourceFiles;
	TMap<FName, FString> SourcePackages;
	if (!ConvaiAvatarSourceRevision::CapturePrepared(Asset.PluginDirectory, BeforeFiles, Cancel, Error) ||
		!ConvaiAvatarSourceRevision::CaptureSource(Additions, SourceFiles, SourcePackages, Cancel, Error)) return false;
	if (!UnloadForRefresh(Asset.PluginName, Error)) return false;
	TMap<FString, FString> AfterFiles, CurrentSourceFiles;
	TMap<FName, FString> CurrentSourcePackages;
	if (!ConvaiAvatarSourceRevision::CapturePrepared(Asset.PluginDirectory, AfterFiles, Cancel, Error) ||
		!ConvaiAvatarSourceRevision::CaptureSource(Additions, CurrentSourceFiles, CurrentSourcePackages, Cancel, Error)) return false;
	if (!BeforeFiles.OrderIndependentCompareEqual(AfterFiles) || !SourceFiles.OrderIndependentCompareEqual(CurrentSourceFiles))
	{ Error = TEXT("Avatar or Convai content changed before the update could start. Choose Upload changes again. No files were replaced."); return false; }
	if (Cancel && Cancel()) { Error = TEXT("Avatar preparation was cancelled."); return false; }
	// Ownership is persisted before the first copy. A partial operation can resume only
	// through this local marker, never through a downloaded archive's untrusted metadata.
	Asset.SourceToDestinationPackages.Append(Additions);
	for (const auto& Pair : Additions) Asset.RetainedSourceToDestinationPackages.Remove(Pair.Key);
	Asset.bConvaiContentPending = true;
	if (!SaveManifest(Asset, false, Error)) return false;
	TMap<FSoftObjectPath, FSoftObjectPath> PreviousRedirects;
	ON_SCOPE_EXIT
	{
		for (const auto& Pair : PreviousRedirects)
		{
			GRedirectCollector.RemoveAssetPathRedirection(Pair.Key);
			if (Pair.Value.IsValid()) GRedirectCollector.AddAssetPathRedirection(Pair.Key, Pair.Value);
		}
	};
	for (const auto& Pair : Additions)
	{
		const FString ObjectPath = Pair.Key.ToString() + TEXT(".") + FPackageName::GetShortName(Pair.Key);
		for (const FString& Path : {ObjectPath, ObjectPath + TEXT("_C")})
			PreviousRedirects.Add(FSoftObjectPath(Path), GRedirectCollector.GetAssetPathRedirection(FSoftObjectPath(Path)));
	}
	for (const auto& Pair : ToCopy)
	{
		if (Cancel && Cancel()) { Error = TEXT("Avatar content copying was cancelled. Choose Upload changes to resume."); return false; }
		if (!DuplicatePreparedPackage(Pair.Key, Pair.Value, Error)) return false;
	}
	TMap<UObject*, UObject*> ObjectMap;
	TMap<FSoftObjectPath, FSoftObjectPath> SoftMap;
	TSet<FName> RewriteNames;
	for (FName Package : Closure) if (Package.ToString().StartsWith(Mount)) RewriteNames.Add(Package);
	for (FName Package : SdkClosure) if (Package.ToString().StartsWith(Mount)) RewriteNames.Add(Package);
	for (const auto& Pair : Additions)
	{
		if (Cancel && Cancel()) { Error = TEXT("Avatar content copying was cancelled. Choose Upload changes to resume."); return false; }
		UPackage* Source = LoadPackage(nullptr, *Pair.Key.ToString(), LOAD_None);
		UPackage* Destination = LoadPackage(nullptr, *Pair.Value.ToString(), LOAD_None);
		if (!Source || !Destination) { Error = TEXT("Could not load both copies of the Convai dependency: ") + Pair.Key.ToString(); return false; }
		ObjectMap.Add(Source, Destination); RewriteNames.Add(Pair.Value);
		TArray<UObject*> SourceObjects;
		GetPreparationObjectsWithOuter(Source, SourceObjects, true);
		for (UObject* Object : SourceObjects)
		{
			const FString TargetPath = Pair.Value.ToString() + Object->GetPathName().Mid(Pair.Key.ToString().Len());
			if (UObject* Target = StaticFindObject(nullptr, nullptr, *TargetPath))
			{
				ObjectMap.Add(Object, Target); SoftMap.Add(FSoftObjectPath(Object), FSoftObjectPath(Target));
				const FSoftObjectPath RedirectKey = FSoftObjectPath(Object).GetWithoutSubPath();
				if (!PreviousRedirects.Contains(RedirectKey)) PreviousRedirects.Add(RedirectKey, GRedirectCollector.GetAssetPathRedirection(RedirectKey));
			}
			else if (Object->IsAsset()) { Error = TEXT("The copied Convai dependency is missing an asset export: ") + TargetPath; return false; }
		}
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	TArray<UPackage*> RewritePackages;
	for (FName Name : RewriteNames)
	{
		UPackage* Package = LoadPackage(nullptr, *Name.ToString(), LOAD_None);
		if (!Package) { Error = TEXT("Could not load the avatar package for its Convai reference update: ") + Name.ToString(); return false; }
		RewritePackages.Add(Package);
		TArray<UObject*> Objects;
		GetPreparationObjectsWithOuter(Package, Objects, true);
		for (UObject* Object : Objects)
		{
			ConvaiAvatarReferenceRemap::RemapNiagaraTypes(Object->GetClass(), Object, ObjectMap);
			ConvaiAvatarReferenceRemap::RemapNativeReferences(Object, ObjectMap);
			FArchiveReplaceObjectRef<UObject> Replace(Object, ObjectMap, EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
		}
		AssetTools.RenameReferencingSoftObjectPaths({Package}, SoftMap);
	}
	if (!RepairPreparedObjectTemplates(RewritePackages, Mount, ObjectMap, SoftMap, Cancel, Error) ||
		!CompilePreparedBlueprints(RewritePackages, Cancel, Error)) return false;
	TArray<FString> ModifiedFiles;
	for (UPackage* Package : RewritePackages)
	{
		if (Cancel && Cancel()) { Error = TEXT("Avatar content copying was cancelled. Choose Upload changes to resume."); return false; }
		if (!SavePackage(Package, Error)) return false;
		ModifiedFiles.Add(FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()));
	}
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanModifiedAssetFiles(ModifiedFiles);
	Asset.bConvaiContentPending = false;
	return true; // The caller still performs the ordinary strict closure/ownership validation.
}
} // namespace ConvaiAvatarWorkspacePrivate

using namespace ConvaiAvatarWorkspacePrivate;

FString FConvaiAvatarWorkspace::GetProxyDirectory()
{
	return Normalized(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ConvaiAvatarStudio/Uploader")));
}

FString FConvaiAvatarWorkspace::GetAvatarsDirectory()
{
	return Normalized(FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("ConvaiAvatars")));
}

FString FConvaiAvatarWorkspace::MakePluginName(const FString& AssetId)
{
	FTCHARToUTF8 Utf8(*AssetId);
	return TEXT("ConvaiAvatar_") + FMD5::HashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

bool FConvaiAvatarWorkspace::IsSafePluginName(const FString& PluginName)
{
	if (PluginName.IsEmpty() || PluginName.Len() > 100 || !FChar::IsAlpha(PluginName[0])) return false;
	for (TCHAR Character : PluginName) if (Character > 127 || (!FChar::IsAlnum(Character) && Character != TEXT('_'))) return false;
	return true;
}

FName FConvaiAvatarWorkspace::MakeDestinationPackage(FName Source, const FString& PluginName)
{
	const FString SourceName = Source.ToString();
	const FString Mount = TEXT("/") + PluginName + TEXT("/");
	if (SourceName.StartsWith(Mount)) return Source;
	// Keeping the original mount segment avoids /Game/Foo and /OtherPlugin/Foo collisions.
	return FName(*(Mount + SourceName.Mid(1)));
}

bool FConvaiAvatarWorkspace::FindPreparedAsset(const FString& AssetId, FConvaiAvatarPreparedAsset& OutAsset, FString& OutError)
{
	bool bReady = false;
	if (!FindLocalAssetRecord(AssetId, OutAsset, bReady, OutError)) return false;
	if (!bReady)
	{
		OutError = TEXT("This avatar's previous preparation did not finish. Prepare it again from the original Blueprint with Refresh from source enabled.");
		return false;
	}
	return MountPlugin(OutAsset, OutError);
}

bool FConvaiAvatarWorkspace::FindLocalAssetRecord(const FString& AssetId, FConvaiAvatarPreparedAsset& OutAsset, bool& bReady, FString& OutError)
{
	OutAsset = {};
	bReady = false;
	OutError.Reset();
	return ReadManifest(AssetId, OutAsset, bReady, OutError);
}

bool FConvaiAvatarWorkspace::ListLocalAssetRecords(TArray<FConvaiAvatarPreparedAsset>& OutAssets, TSet<FString>& OutIncompleteAssetIds, FString& OutError)
{
	OutAssets.Reset();
	OutIncompleteAssetIds.Reset();
	OutError.Reset();
	const FString Root = GetAvatarsDirectory();
	if (!IFileManager::Get().DirectoryExists(*Root)) return true;
	if (IsReparsePoint(Root)) { OutError = TEXT("The canonical avatar folder is redirected; local records cannot be listed safely."); return false; }
	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *(Root / TEXT("*")), false, true);
	Folders.Sort();
	TSet<FString> SeenIds;
	for (const FString& Folder : Folders)
	{
		const FString Directory = Root / Folder;
		if (!IsSafePluginName(Folder) || IsReparsePoint(Directory) || IsReparsePoint(Directory / ManifestName) ||
			IFileManager::Get().FileExists(*(Directory / TEXT("ConvaiAvatarPendingInstall.json")))) continue;
		TSharedPtr<FJsonObject> Record;
		FString Id, Name;
		int32 Version = 0;
		bool bReady = false;
		if (!ReadWorkspaceJson(Directory / ManifestName, Record) || !Record->TryGetNumberField(TEXT("version"), Version) || Version != ManifestVersion ||
			!Record->TryGetStringField(TEXT("asset_id"), Id) || Id.IsEmpty() || !Record->TryGetStringField(TEXT("plugin_name"), Name) || Name != Folder ||
			!Record->TryGetBoolField(TEXT("ready"), bReady)) continue;
		if (!ReconcileBindingReceipt(Directory, Record, OutError)) return false;
		Record->TryGetStringField(TEXT("asset_id"), Id);
		if (SeenIds.Contains(Id)) { OutError = TEXT("More than one canonical plugin claims an asset ID. Resolve the duplicate local records before continuing."); return false; }
		SeenIds.Add(Id);
		FConvaiAvatarPreparedAsset Asset;
		if (!FindLocalAssetRecord(Id, Asset, bReady, OutError)) return false;
		if (Asset.PluginName != Folder) { OutError = TEXT("A canonical avatar's workspace record points to a different plugin. Resolve the duplicate local records before continuing."); return false; }
		OutAssets.Add(MoveTemp(Asset));
		if (!bReady) OutIncompleteAssetIds.Add(Id);
	}
	return true;
}

bool FConvaiAvatarWorkspace::DiscardLocalDraft(const FString& AssetId, FString& OutError)
{
	check(IsInGameThread());
	FConvaiAvatarPreparedAsset Asset;
	bool bReady = false;
	if (!FindLocalAssetRecord(AssetId, Asset, bReady, OutError)) return false;
	if (!Asset.bIsDraft || !Asset.bIsStaging)
	{
		OutError = TEXT("Only a local upload draft can be discarded here. This avatar is already bound to the cloud or is an imported avatar.");
		return false;
	}
	if (!LoadedAvatarPackages(Asset.PluginName).IsEmpty())
	{
		OutError = TEXT("The draft is still loaded. Close its editors and remove its scene instances, then restart the editor before discarding it. Your original avatar will be kept.");
		return false;
	}
	if (IFileManager::Get().FileExists(*(Asset.PluginDirectory / TEXT("ConvaiAvatarPendingInstall.json"))))
	{
		OutError = TEXT("This avatar has a pending download installation. Finish recovering that installation before discarding anything.");
		return false;
	}
	if (!ManagedDirectory(Asset.PluginDirectory, OutError) || IsReparsePoint(Asset.PluginDirectory)) return false;
	const FString DescriptorFile = Asset.PluginDirectory / (Asset.PluginName + TEXT(".uplugin"));
	FPluginDescriptor Descriptor;
	FText Reason;
	if (!Descriptor.Load(DescriptorFile, Reason)) { OutError = Reason.ToString(); return false; }
	if (!Descriptor.Modules.IsEmpty() || !Descriptor.bCanContainContent || Descriptor.bCanContainVerse || Descriptor.bIsPluginExtension)
	{
		OutError = TEXT("The draft folder is no longer a content-only avatar plugin and will not be deleted.");
		return false;
	}
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Asset.PluginName);
	if (Plugin.IsValid() && !Normalized(Plugin->GetBaseDir()).Equals(Asset.PluginDirectory, ESearchCase::IgnoreCase))
	{
		OutError = TEXT("A different plugin uses this draft's name. It will not be changed.");
		return false;
	}
	// Preflight a bounded, non-recursive walk. Never traverse links, even within an owned plugin.
	TArray<FString> Directories{Asset.PluginDirectory};
	TArray<FString> Files;
	for (int32 Index = 0; Index < Directories.Num(); ++Index)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Directories[Index] / TEXT("*")), true, true);
		for (const FString& Name : Names)
		{
			const FString Path = Normalized(Directories[Index] / Name);
			if (!IsInside(Path, Asset.PluginDirectory) || IsReparsePoint(Path) || IFileManager::Get().IsReadOnly(*Path))
			{
				OutError = FString::Printf(TEXT("The draft contains a redirected, read-only, or unexpected path: %s. It will not be deleted."), *Path);
				return false;
			}
			if (IFileManager::Get().DirectoryExists(*Path)) Directories.Add(Path);
			else Files.Add(Path);
			if (Directories.Num() + Files.Num() > 100000) { OutError = TEXT("The draft contains too many files to discard safely."); return false; }
		}
	}
	const bool bHasLink = IsReparsePoint(Asset.ProxyPluginDirectory) || IFileManager::Get().DirectoryExists(*Asset.ProxyPluginDirectory);
	if (bHasLink)
	{
		if (!ConvaiAvatarWorkspaceLinks::Ensure(FPaths::ProjectDir(), Asset.PluginName, OutError)) return false;
		FString ResolvedSource, ResolvedLink;
		if (!ManagedDirectory(FPaths::GetPath(Asset.ProxyPluginDirectory), OutError)) return false;
		if (!IsReparsePoint(Asset.ProxyPluginDirectory) || !ResolveDirectory(Asset.PluginDirectory, ResolvedSource) ||
			!ResolveDirectory(Asset.ProxyPluginDirectory, ResolvedLink) || !ResolvedLink.Equals(ResolvedSource, ESearchCase::IgnoreCase))
		{
			OutError = TEXT("The uploader link no longer points to this draft. It will not be removed.");
			return false;
		}
	}
	if (!FConvaiAvatarProjectConfiguration::SetStagingCookExclusion(FPaths::ProjectDir(), Asset.PluginName, false, OutError)) return false;
	// Keep a recoverable incomplete record until all content has gone. Failure midway can be retried.
	Descriptor.EnabledByDefault = EPluginEnabledByDefault::Disabled;
	if (Plugin.IsValid() && Plugin->IsEnabled())
	{
		// UE exposes safe content unmount only for explicitly loaded plugins. This owned draft
		// has no code and no loaded packages; opt it in before using that supported unmount path.
		Descriptor.bExplicitlyLoaded = true;
        if (!Plugin->UpdateDescriptor(Descriptor, Reason) ||
#if UE_VERSION_OLDER_THAN(5, 4, 0)
            !IPluginManager::Get().UnmountExplicitlyLoadedPlugin(Asset.PluginName, &Reason))
#else
            !IPluginManager::Get().UnmountExplicitlyLoadedPlugin(Asset.PluginName, &Reason, false))
#endif
		{
			OutError = TEXT("Unreal could not release this draft's content mount. Restart the editor before discarding it. ") + Reason.ToString();
			return false;
		}
	}
	else if (!Descriptor.Save(DescriptorFile, Reason)) { OutError = Reason.ToString(); return false; }
	if (!IPluginManager::Get().RemoveFromPluginsList(DescriptorFile, &Reason)) { OutError = Reason.ToString(); return false; }
	if (!SaveManifest(Asset, false, OutError)) return false;
#if PLATFORM_WINDOWS
	if (bHasLink && !RemoveDirectoryW(*Asset.ProxyPluginDirectory)) { OutError = TEXT("Could not remove the draft's uploader junction. Its source was kept."); return false; }
	if (bHasLink) IFileManager::Get().Delete(*ConvaiAvatarWorkspaceLinks::ReceiptPath(FPaths::ProjectDir(), Asset.PluginName), false, false, true);
#else
	if (bHasLink) { OutError = TEXT("Discarding linked avatar workspaces is currently supported on Windows only."); return false; }
#endif
	const FString Sidecar = Asset.PluginDirectory / ManifestName;
	for (const FString& File : Files)
	{
		if (File.Equals(Sidecar, ESearchCase::IgnoreCase) || File.Equals(DescriptorFile, ESearchCase::IgnoreCase)) continue;
		if (IsReparsePoint(File) || !IFileManager::Get().Delete(*File, false, false, true))
		{
			OutError = TEXT("Some draft files could not be removed. Close programs using them and retry Discard draft.");
			return false;
		}
	}
	for (int32 Index = Directories.Num() - 1; Index > 0; --Index)
	{
		if (IsReparsePoint(Directories[Index]) || !IFileManager::Get().DeleteDirectory(*Directories[Index], false, false))
		{
			OutError = TEXT("Some draft folders could not be removed. Close programs using them and retry Discard draft.");
			return false;
		}
	}
	if (!IFileManager::Get().Delete(*DescriptorFile, false, false, true) || !IFileManager::Get().Delete(*Sidecar, false, false, true) ||
		!IFileManager::Get().DeleteDirectory(*Asset.PluginDirectory, false, false))
	{
		OutError = TEXT("The draft's remaining metadata could not be removed. Check its folder permissions.");
		return false;
	}
	IFileManager::Get().Delete(*IndexFile(AssetId), false, true, true);
	return true;
}

bool FConvaiAvatarWorkspace::RebindPreparedAsset(FConvaiAvatarPreparedAsset& Asset, const FString& NewAssetId, FString& OutError)
{
	check(IsInGameThread());
	if (NewAssetId == Asset.AssetId) return true;
	TSharedPtr<FJsonObject> Collision;
	bool bHasCollision = false;
	if (!FindManifestDocument(NewAssetId, Collision, bHasCollision, OutError)) return false;
	FString ExistingPlugin;
	if (bHasCollision && Collision->TryGetStringField(TEXT("plugin_name"), ExistingPlugin) && ExistingPlugin == Asset.PluginName)
	{
		FConvaiAvatarPreparedAsset Recovered;
		bool bReady = false;
		if (!FindLocalAssetRecord(NewAssetId, Recovered, bReady, OutError) || !bReady || Recovered.bIsDraft ||
			!Normalized(Asset.PluginDirectory).Equals(Recovered.PluginDirectory, ESearchCase::IgnoreCase))
		{
			if (OutError.IsEmpty()) OutError = TEXT("The existing cloud binding does not match this prepared draft.");
			return false;
		}
		Asset = MoveTemp(Recovered);
		return true;
	}
	if (NewAssetId.TrimStartAndEnd().IsEmpty() || NewAssetId.Len() > 256 || bHasCollision)
	{
		OutError = TEXT("The remote asset ID is invalid or already has local prepared content. The draft mount was preserved.");
		return false;
	}
	FConvaiAvatarPreparedAsset Previous;
	bool bReady = false;
	if (!ReadManifest(Asset.AssetId, Previous, bReady, OutError) || !bReady || !Previous.bIsDraft || Previous.PluginName != Asset.PluginName ||
		!Normalized(Asset.PluginDirectory).Equals(Previous.PluginDirectory, ESearchCase::IgnoreCase) || Asset.EntryPoint != Previous.EntryPoint)
	{
		if (OutError.IsEmpty()) OutError = TEXT("Only a complete locally owned draft can be bound to a remote asset.");
		return false;
	}
	const FString PreviousId = Asset.AssetId;
	TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(TEXT("previous_asset_id"), PreviousId);
	Receipt->SetStringField(TEXT("asset_id"), NewAssetId);
	Receipt->SetStringField(TEXT("plugin_name"), Asset.PluginName);
	if (!WriteWorkspaceJson(Asset.PluginDirectory / BindingReceiptName, Receipt, OutError))
	{
		OutError += FString::Printf(TEXT(" The avatar was already created in the cloud as %s; do not create another one."), *NewAssetId);
		return false;
	}
	Asset.AssetId = NewAssetId;
	Asset.bIsDraft = false;
	if (!SaveManifest(Asset, true, OutError))
	{
		Asset.AssetId = PreviousId;
		Asset.bIsDraft = Previous.bIsDraft;
		FString RestoreError;
		if (!SaveManifest(Previous, true, RestoreError)) OutError += TEXT(" The local record could not be restored: ") + RestoreError;
		return false;
	}
	// This is a single owned metadata file, never a directory or avatar content.
	IFileManager::Get().Delete(*IndexFile(PreviousId), false, true, true);
	IFileManager::Get().Delete(*(Asset.PluginDirectory / BindingReceiptName), false, true, true);
	return true;
}

bool FConvaiAvatarWorkspace::CollectDirtyPackages(const FConvaiAvatarPrepareRequest& Request, TArray<UPackage*>& OutPackages, FString& OutError)
{
	check(IsInGameThread());
	OutPackages.Reset(); OutError.Reset();
	if (Request.AssetId.TrimStartAndEnd().IsEmpty() || Request.AssetId.Len() > 256 || !Request.Blueprint.IsValid())
	{ OutError = TEXT("Choose an avatar Blueprint and a valid asset ID before continuing."); return false; }
	if (Request.IsCancelled && Request.IsCancelled()) { OutError = TEXT("Avatar preparation was cancelled."); return false; }
	UBlueprint* Blueprint = Cast<UBlueprint>(Request.Blueprint.ResolveObject());
	if (!Blueprint && FPackageName::DoesPackageExist(Request.Blueprint.GetLongPackageName())) Blueprint = Cast<UBlueprint>(Request.Blueprint.TryLoad());
	if (!Blueprint)
	{ OutError = TEXT("The avatar Blueprint cannot be found at ") + Request.Blueprint.ToString() + TEXT(". Restore it at that path before uploading this avatar."); return false; }
	UClass* ActorClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get();
	if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
	{ OutError = TEXT("Choose an Actor Blueprint, such as the assembled MetaHuman Blueprint."); return false; }

	FConvaiAvatarPreparedAsset Previous;
	TSharedPtr<FJsonObject> Document;
	bool bFound = false, bReady = false;
	if (!FindManifestDocument(Request.AssetId, Document, bFound, OutError, false)) return false;
	if (bFound && !ReadManifest(Request.AssetId, Previous, bReady, OutError, false)) return false;
	if (bFound && Request.bRefreshFromSource && !Previous.OriginalEntryPoint.IsNull() && Request.Blueprint != Previous.OriginalEntryPoint)
	{ OutError = TEXT("Upload changes must use this avatar's original project Blueprint: ") + Previous.OriginalEntryPoint.ToString(); return false; }
	const FString Plugin = bFound ? Previous.PluginName : MakePluginName(Request.AssetId);
	const FString Mount = TEXT("/") + Plugin + TEXT("/");
	TMap<FName, FName> Mapping;
	TArray<FString> RequiredPlugins;
	TArray<FName> Closure, Missing;
	TArray<UPackage*> Dirty;
	if (!GatherForPreparation(Blueprint, Mount, Request.bIsMetaHuman, Request.bIncludeConvaiContent, Mapping, RequiredPlugins, Closure, Missing, Request.IsCancelled, OutError, NAME_None, &Dirty) ||
		!GatherRequestedDiorama(Request, Mount, Mapping, RequiredPlugins, Closure, Missing, OutError, &Dirty)) return false;
	if (bFound && ((Request.bRefreshFromSource && !Previous.OriginalEntryPoint.IsNull()) || Request.bIncludeConvaiContent || Previous.bConvaiContentPending))
	{
		// An authored refresh may unload this avatar's full generated copy. Include
		// only its loaded dirty packages, never another avatar or the user's level.
		for (UPackage* Package : LoadedAvatarPackages(Plugin))
		{
			if (Package->IsDirty() && !Package->ContainsMap() && !Package->HasAnyFlags(RF_Transient)) Dirty.AddUnique(Package);
		}
	}
	if (Request.IsCancelled && Request.IsCancelled()) { OutError = TEXT("Avatar preparation was cancelled."); return false; }
	Dirty.Sort([](const UPackage& A, const UPackage& B) { return A.GetName() < B.GetName(); });
	OutPackages = MoveTemp(Dirty);
	return true;
}

bool FConvaiAvatarWorkspace::ReviewDependencies(const FConvaiAvatarPrepareRequest& Request, FConvaiAvatarDependencyReview& OutReview, FString& OutError)
{
	check(IsInGameThread());
	OutReview = {};
	OutError.Reset();
	if (Request.AssetId.TrimStartAndEnd().IsEmpty() || Request.AssetId.Len() > 256 || !Request.Blueprint.IsValid())
	{ OutError = TEXT("Choose an avatar Blueprint and a valid asset ID before continuing."); return false; }
	if (Request.IsCancelled && Request.IsCancelled()) { OutError = TEXT("Avatar preparation was cancelled."); return false; }
	const FString Root = Request.Blueprint.GetLongPackageName();
	if (!FPackageName::DoesPackageExist(Root))
	{ OutError = TEXT("The selected avatar Blueprint has no saved package: ") + Root + TEXT(". Save your changes before creating and uploading the avatar."); return false; }
	UBlueprint* Blueprint = Cast<UBlueprint>(Request.Blueprint.TryLoad());
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("The avatar Blueprint cannot be found at %s. Restore it at that path before uploading this avatar."), *Request.Blueprint.ToString());
		return false;
	}
	if (!Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
	{ OutError = TEXT("Choose an Actor Blueprint, such as the assembled MetaHuman Blueprint."); return false; }
	TMap<FName, FName> Mapping;
	TArray<FString> RequiredPlugins;
	TArray<FName> Closure, Missing;
	const FString Mount = TEXT("/") + MakePluginName(Request.AssetId) + TEXT("/");
	if (!GatherForPreparation(Blueprint, Mount, Request.bIsMetaHuman, Request.bIncludeConvaiContent, Mapping, RequiredPlugins, Closure, Missing, Request.IsCancelled, OutError,
		Request.bIncludeDiorama ? FName(*Request.DioramaSourceLevel) : NAME_None) ||
		!GatherRequestedDiorama(Request, Mount, Mapping, RequiredPlugins, Closure, Missing, OutError)) return false;
	return ReviewMissingPackages(Missing, Request.AcknowledgedMissingPackages, OutReview, OutError);
}

bool FConvaiAvatarWorkspace::ReviewSourceChanges(const FConvaiAvatarPrepareRequest& Request, FConvaiAvatarSourceReview& OutReview, FString& OutError)
{
	check(IsInGameThread());
	OutReview = {}; OutError.Reset();
	TSharedPtr<FJsonObject> Document;
	bool bFound = false;
	if (!FindManifestDocument(Request.AssetId, Document, bFound, OutError)) return false;
	if (!bFound) return true;
	FConvaiAvatarPreparedAsset Previous, Current;
	bool bReady = false;
	if (!ReadManifest(Request.AssetId, Previous, bReady, OutError)) return false;
	if (Previous.OriginalEntryPoint.IsNull() || !Request.bRefreshFromSource) return true;
	if (Request.Blueprint != Previous.OriginalEntryPoint)
	{ OutError = TEXT("Upload changes must use this avatar's original project Blueprint: ") + Previous.OriginalEntryPoint.ToString(); return false; }
	if (!FPackageName::DoesPackageExist(Previous.OriginalEntryPoint.GetLongPackageName()))
	{ OutError = TEXT("The original project Blueprint is missing: ") + Previous.OriginalEntryPoint.ToString() + TEXT(". Restore it at this path before uploading changes. The prepared copy has been kept."); return false; }
	Current = Previous;
	Current.bIncludeConvaiContent = Request.bIncludeConvaiContent;
	Current.SourceToDestinationPackages.Reset(); Current.RequiredPlugins.Reset();
	if (Previous.bIsDraft && Request.bIsNewDraft) Current.bIsMetaHuman = Request.bIsMetaHuman;
	TArray<FName> Closure, Missing;
	UBlueprint* Blueprint = Cast<UBlueprint>(Request.Blueprint.TryLoad());
	if (!Blueprint) { OutError = TEXT("The original avatar Blueprint could not be loaded for its source review."); return false; }
	TSet<FName> GeneratedDioramaPackages;
	FName DioramaDestination;
	UWorld* DioramaWorld = nullptr;
	if (!GatherRevisionGraph(Request, Blueprint, Current.bIsMetaHuman, Current.bIncludeConvaiContent, TEXT("/") + Previous.PluginName + TEXT("/"), Previous.PluginName,
		Current.SourceToDestinationPackages, Current.RequiredPlugins, Closure, Missing, GeneratedDioramaPackages, DioramaDestination, DioramaWorld, OutError)) return false;
	TMap<FString, FString> PreparedFiles;
	if (!CaptureRevisionSource(Current.SourceToDestinationPackages, GeneratedDioramaPackages, Current.SourceFileHashes,
			Current.SourcePackageHashes, Request.IsCancelled, OutError) ||
		!ConvaiAvatarSourceRevision::CapturePrepared(Previous.PluginDirectory, PreparedFiles, Request.IsCancelled, OutError)) return false;
	ConvaiAvatarSourceRevision::Compare(Previous, Current, PreparedFiles, OutReview);
	if (!bReady)
	{
		OutReview.bNeedsRefresh = true;
		OutReview.bRequiresConfirmation = OutReview.bPreparedChanged;
	}
	return true;
}

bool FConvaiAvatarWorkspace::Prepare(const FConvaiAvatarPrepareRequest& Request, FConvaiAvatarPreparedAsset& OutAsset, FString& OutError, FConvaiAvatarDependencyReview* OutReview)
{
	check(IsInGameThread());
	OutAsset = {};
	OutError.Reset();
	FConvaiAvatarDependencyReview InitialReview;
	if (!ReviewDependencies(Request, InitialReview, OutError))
	{ if (OutReview) *OutReview = MoveTemp(InitialReview); return false; }
	if (OutReview) *OutReview = InitialReview;
	if (InitialReview.bRequiresAcknowledgement) { OutError = MissingReviewMessage(InitialReview); return false; }
	if (Request.AssetId.TrimStartAndEnd().IsEmpty() || Request.AssetId.Len() > 256 || !Request.Blueprint.IsValid())
	{
		OutError = TEXT("Choose an avatar Blueprint and a valid asset ID before preparing it.");
		return false;
	}
	if (Request.IsCancelled && Request.IsCancelled()) { OutError = TEXT("Avatar preparation was cancelled."); return false; }
	UBlueprint* Blueprint = Cast<UBlueprint>(Request.Blueprint.TryLoad());
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("The original avatar Blueprint cannot be found at %s. Restore it at that path before uploading changes. The prepared copy has been kept."), *Request.Blueprint.ToString());
		return false;
	}
	if (!Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
	{
		OutError = TEXT("Choose an Actor Blueprint, such as the assembled MetaHuman Blueprint.");
		return false;
	}
	const FName SourcePackage(*Request.Blueprint.GetLongPackageName());
	FConvaiAvatarPreparedAsset Previous;
	TSharedPtr<FJsonObject> ExistingRecord;
	bool bHasIndex = false;
	if (!FindManifestDocument(Request.AssetId, ExistingRecord, bHasIndex, OutError)) return false;
	bool bPreviousReady = false;
	if (bHasIndex && !ReadManifest(Request.AssetId, Previous, bPreviousReady, OutError)) return false;
	if (bHasIndex && Request.Blueprint == Previous.EntryPoint)
	{
		if (!bPreviousReady && !(Previous.bConvaiContentPending && Previous.bIncludeConvaiContent && Request.bIncludeConvaiContent))
		{ OutError = Previous.bConvaiContentPending ? TEXT("Finish the pending Convai content update with Include Convai content selected before changing that option.") : TEXT("The prepared copy is incomplete. Refresh it from the original Blueprint or finish downloading its source first."); return false; }
		const FName PreviousLevel = Previous.Diorama.IsSet() ? FName(*Previous.Diorama->Level) : NAME_None;
		const FName* OriginalLevel = Previous.SourceToDestinationPackages.FindKey(PreviousLevel);
		const FName RequestedLevel(*Request.DioramaSourceLevel);
		if (Request.bIncludeDiorama != Previous.Diorama.IsSet() ||
			(Request.bIncludeDiorama && RequestedLevel != PreviousLevel && (!OriginalLevel || RequestedLevel != *OriginalLevel)))
		{
			OutError = TEXT("To change Include environment or its level, refresh from the original Blueprint instead of updating the prepared copy directly.");
			return false;
		}
		OutAsset = Previous;
		OutAsset.bIncludeConvaiContent = Request.bIncludeConvaiContent;
		OutAsset.AcknowledgedMissingPackages = InitialReview.MissingPackages;
		if (Previous.bIsDraft && Request.bIsNewDraft)
		{
			OutAsset.bIsMetaHuman = Request.bIsMetaHuman;
			OutAsset.bHasMetaHumanChoice = true;
		}
		if (!SetupPreparedBlueprint(OutAsset, OutError)) return false;
		Blueprint = nullptr; // Only the selected generated packages may be unloaded by inclusion.
		if (!IncludeReferencedConvaiContent(OutAsset, Request.IsCancelled, OutError)) return false;
		if (!ValidatePreparedAsset(OutAsset, OutError)) return false;
		return UpdatePluginDependencies(OutAsset, OutError) && SaveManifest(OutAsset, true, OutError) && EnsureProxyLink(OutAsset, OutError);
	}
	if (bHasIndex && Request.Blueprint != Previous.OriginalEntryPoint)
	{
		OutError = TEXT("This asset ID already belongs to another Blueprint. Choose its prepared Blueprint or create a new avatar.");
		return false;
	}
	if (bHasIndex && !Request.bRefreshFromSource)
	{
		OutError = TEXT("Upload changes from the avatar's original project Blueprint to refresh its prepared files.");
		return false;
	}

	OutAsset.AssetId = Request.AssetId;
	OutAsset.DisplayName = Request.DisplayName;
	OutAsset.PluginName = bHasIndex ? Previous.PluginName : MakePluginName(Request.AssetId);
	OutAsset.bIsStaging = true;
	OutAsset.bIsDraft = bHasIndex ? Previous.bIsDraft : Request.bIsNewDraft;
	OutAsset.bIsMetaHuman = bHasIndex && !(Previous.bIsDraft && Request.bIsNewDraft) ? Previous.bIsMetaHuman : Request.bIsMetaHuman;
	OutAsset.bHasMetaHumanChoice = !bHasIndex || (Previous.bIsDraft && Request.bIsNewDraft) || Previous.bHasMetaHumanChoice;
	OutAsset.bIncludeConvaiContent = Request.bIncludeConvaiContent;
	OutAsset.OriginalEntryPoint = Request.Blueprint;
	OutAsset.AcknowledgedMissingPackages = InitialReview.MissingPackages;
	FillDirectories(OutAsset);
	const FString Mount = TEXT("/") + OutAsset.PluginName + TEXT("/");
	const TSharedPtr<IPlugin> NameCollision = IPluginManager::Get().FindPlugin(OutAsset.PluginName);
	if ((!bHasIndex && IFileManager::Get().DirectoryExists(*OutAsset.PluginDirectory)) ||
		(NameCollision.IsValid() && !Normalized(NameCollision->GetBaseDir()).Equals(OutAsset.PluginDirectory, ESearchCase::IgnoreCase)))
	{
		OutError = FString::Printf(TEXT("The plugin folder %s already exists and is not owned by this avatar. It will not be overwritten."), *OutAsset.PluginName);
		return false;
	}
	TArray<FName> Closure, Missing;
	const FName DioramaSource = Request.bIncludeDiorama ? FName(*Request.DioramaSourceLevel) : NAME_None;
	FName DioramaDestination;
	UWorld* DioramaWorld = nullptr;
	TSet<FName> GeneratedDioramaPackages;
	if (!GatherRevisionGraph(Request, Blueprint, OutAsset.bIsMetaHuman, OutAsset.bIncludeConvaiContent, Mount, OutAsset.PluginName,
		OutAsset.SourceToDestinationPackages, OutAsset.RequiredPlugins, Closure, Missing, GeneratedDioramaPackages, DioramaDestination, DioramaWorld, OutError)) return false;
	if (Request.bIncludeDiorama)
	{
		FConvaiAvatarDioramaRecord Record;
		if (!ConvaiAvatarDiorama::ReadRecord(ConvaiAvatarDiorama::MakeRecord(Request.DioramaFacts, DioramaDestination.ToString()), Record))
		{ OutError = TEXT("The diorama scan cannot be recorded; rescan before uploading."); return false; }
		OutAsset.Diorama = MoveTemp(Record);
	}
	if (bHasIndex)
	{
		OutAsset.RetainedSourceToDestinationPackages = Previous.RetainedSourceToDestinationPackages;
		// A diorama copy is renamed and regenerated on every refresh, so only same-path copies can be retained.
		for (const auto& Pair : Previous.SourceToDestinationPackages)
			if (!OutAsset.SourceToDestinationPackages.Contains(Pair.Key) && Pair.Value == MakeDestinationPackage(Pair.Key, OutAsset.PluginName))
				OutAsset.RetainedSourceToDestinationPackages.Add(Pair.Key, Pair.Value);
		for (const auto& Pair : OutAsset.SourceToDestinationPackages) OutAsset.RetainedSourceToDestinationPackages.Remove(Pair.Key);
	}
	FConvaiAvatarDependencyReview CurrentReview;
	if (!ReviewMissingPackages(Missing, Request.AcknowledgedMissingPackages, CurrentReview, OutError)) return false;
	if (OutReview) *OutReview = CurrentReview;
	if (CurrentReview.bRequiresAcknowledgement) { OutError = MissingReviewMessage(CurrentReview); return false; }
	OutAsset.AcknowledgedMissingPackages = CurrentReview.MissingPackages;
	if (!OutAsset.SourceToDestinationPackages.Contains(SourcePackage))
	{
		OutError = TEXT("Choose an avatar outside the shared Convai runtime content to prepare a separate avatar plugin.");
		return false;
	}
	const FString DestinationPackage = OutAsset.SourceToDestinationPackages[SourcePackage].ToString();
	OutAsset.EntryPoint = FSoftObjectPath(DestinationPackage + Request.Blueprint.ToString().Mid(SourcePackage.ToString().Len()));
	OutAsset.PackageCount = OutAsset.SourceToDestinationPackages.Num();
	for (const auto& Pair : OutAsset.SourceToDestinationPackages)
	{
		// The destination mount need not exist yet. Check its validated disk path directly,
		// so collision detection also works on an incomplete, disabled plugin after restart.
		const FString ExistingFilename = OutAsset.PluginDirectory / TEXT("Content") / Pair.Value.ToString().Mid(Mount.Len());
		if ((IFileManager::Get().FileExists(*(ExistingFilename + FPackageName::GetAssetPackageExtension())) ||
			IFileManager::Get().FileExists(*(ExistingFilename + FPackageName::GetMapPackageExtension()))) &&
			(!bHasIndex || (Previous.SourceToDestinationPackages.FindRef(Pair.Key) != Pair.Value && Previous.RetainedSourceToDestinationPackages.FindRef(Pair.Key) != Pair.Value)))
		{
			OutError = FString::Printf(TEXT("The destination already contains an unrelated package: %s. It will not be overwritten."), *Pair.Value.ToString());
			return false;
		}
	}
	if (!CaptureRevisionSource(OutAsset.SourceToDestinationPackages, GeneratedDioramaPackages, OutAsset.SourceFileHashes,
		OutAsset.SourcePackageHashes, Request.IsCancelled, OutError)) return false;
	TSet<FName> ReusedEnumPackages;
	TArray<TStrongObjectPtr<UObject>> ReusedEnumObjects;
	if (bHasIndex)
	{
		TMap<FString, FString> PreparedFiles;
		if (!ConvaiAvatarSourceRevision::CapturePrepared(Previous.PluginDirectory, PreparedFiles, Request.IsCancelled, OutError)) return false;
		FConvaiAvatarSourceReview SourceReview;
		ConvaiAvatarSourceRevision::Compare(Previous, OutAsset, PreparedFiles, SourceReview);
		if (!bPreviousReady) { SourceReview.bNeedsRefresh = true; SourceReview.bRequiresConfirmation = SourceReview.bPreparedChanged; }
		if (OutReview) OutReview->SourceChanges = SourceReview;
		if (SourceReview.bRequiresConfirmation && Request.AcknowledgedSourceConflict != SourceReview.ConflictFingerprint)
		{
			OutError = TEXT("Review changes to the generated avatar before updating it from your project Blueprint. Choose Upload changes again to review the files.");
			return false;
		}
		if (!SourceReview.bNeedsRefresh && bPreviousReady)
		{
			// Keep plugin-only changes. Never establish a new baseline here: a later original edit
			// must still detect that these files diverged from the last successful source refresh.
			OutAsset = Previous;
			Blueprint = nullptr;
			if (!IncludeReferencedConvaiContent(OutAsset, Request.IsCancelled, OutError)) return false;
			if (!ValidatePreparedAsset(OutAsset, OutError)) return false;
			return UpdatePluginDependencies(OutAsset, OutError) && SaveManifest(OutAsset, true, OutError) && EnsureProxyLink(OutAsset, OutError);
		}
		// Niagara keeps registered enum types alive even after their consumers unload.
		// Keep only proven unchanged leaf enums in place, including their type handles;
		// invalidating the registry could break live consumers outside this avatar.
		if (bPreviousReady && !ConvaiAvatarReusableEnums::Find(Previous, OutAsset, PreparedFiles, ReusedEnumPackages, OutError)) return false;
		for (FName PackageName : ReusedEnumPackages)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName.ToString());
			if (!Package) { OutError = TEXT("An avatar dependency changed while checking the upload. Try Upload changes again."); return false; }
			TArray<UObject*> Objects;
			GetObjectsWithPackage(Package, Objects);
			for (UObject* Object : Objects) ReusedEnumObjects.Emplace(Object);
		}
		if (!UnloadForRefreshPreserving(OutAsset.PluginName, ReusedEnumPackages, OutError)) return false;
		// Unloading may invoke editor callbacks. Recheck before any prepared file is replaced.
		FConvaiAvatarSourceReview Latest;
		if (!ReviewSourceChanges(Request, Latest, OutError)) return false;
		if (Latest.ConflictFingerprint != SourceReview.ConflictFingerprint)
		{ OutError = TEXT("Avatar files changed before the refresh could start. Choose Upload changes again to review the new revision. The prepared files have not been replaced."); return false; }
		TSet<FName> RecheckedEnums;
		FConvaiAvatarPreparedAsset RecheckedSource = OutAsset;
		// Recheck only kept packages: probing other old enum exports here could
		// reload a dependency that was just unloaded for replacement.
		for (auto It = RecheckedSource.SourceToDestinationPackages.CreateIterator(); It; ++It)
			if (!ReusedEnumPackages.Contains(It.Value())) It.RemoveCurrent();
		TMap<FString, FString> RecheckedFiles;
		if (!ReusedEnumPackages.IsEmpty() &&
			(!ConvaiAvatarSourceRevision::CaptureSource(RecheckedSource.SourceToDestinationPackages, RecheckedSource.SourceFileHashes,
				RecheckedSource.SourcePackageHashes, Request.IsCancelled, OutError) ||
			!ConvaiAvatarSourceRevision::CapturePrepared(Previous.PluginDirectory, RecheckedFiles, Request.IsCancelled, OutError) ||
			!ConvaiAvatarReusableEnums::Find(Previous, RecheckedSource, RecheckedFiles, RecheckedEnums, OutError))) return false;
		bool bSameEnums = RecheckedEnums.Num() == ReusedEnumPackages.Num();
		for (FName Package : ReusedEnumPackages) bSameEnums &= RecheckedEnums.Contains(Package);
		for (const TStrongObjectPtr<UObject>& Object : ReusedEnumObjects)
			bSameEnums &= IsValid(Object.Get()) && ReusedEnumPackages.Contains(Object->GetOutermost()->GetFName()) && !Object->GetOutermost()->IsDirty();
		if (!bSameEnums)
		{ OutError = TEXT("An avatar dependency changed while checking the upload. Try Upload changes again."); return false; }
		OutAsset.PreparedFileHashes = Previous.PreparedFileHashes;
		if (Previous.Diorama.IsSet() && FName(*Previous.Diorama->Level) != DioramaDestination)
		{
			if (!MountPlugin(OutAsset, OutError)) return false;
			// Keep the previous map's ownership available if deletion fails and this refresh is retried.
			if (!SaveManifest(Previous, false, OutError) || !DeletePreviousDiorama(Previous, DioramaDestination, OutError)) return false;
		}
	}
	FPluginDescriptor Descriptor;
	Descriptor.Version = 1;
	Descriptor.VersionName = TEXT("1.0");
	Descriptor.FriendlyName = Request.DisplayName.IsEmpty() ? TEXT("Convai Avatar") : Request.DisplayName;
	Descriptor.Description = TEXT("Avatar source managed by Convai Cloud Avatars.");
	Descriptor.Category = TEXT("Convai Avatars");
	Descriptor.bCanContainContent = true;
	Descriptor.EnabledByDefault = EPluginEnabledByDefault::Disabled;
	for (const FString& Name : OutAsset.RequiredPlugins) Descriptor.Plugins.Emplace(Name, true);
	TArray<FString> CreatedDirectories;
	bool bWorkspaceValidated = false;
	bool bPreparationComplete = false;
	ON_SCOPE_EXIT
	{
		if (!bPreparationComplete)
		{
			if (bWorkspaceValidated)
			{
				// Save ownership before the descriptor: a failed descriptor write stays resumable.
				FString IgnoredError;
				const bool bManifestSaved = SaveManifest(OutAsset, false, IgnoredError);
				TSharedPtr<FJsonObject> Ownership;
				FString OwnerId;
				if (bManifestSaved || (ReadWorkspaceJson(OutAsset.PluginDirectory / ManifestName, Ownership) &&
					Ownership->TryGetStringField(TEXT("asset_id"), OwnerId) && OwnerId == OutAsset.AssetId))
				{
					Descriptor.EnabledByDefault = EPluginEnabledByDefault::Disabled;
					FText IgnoredReason;
					const FString DescriptorFile = OutAsset.PluginDirectory / (OutAsset.PluginName + TEXT(".uplugin"));
					if (!IsReparsePoint(DescriptorFile)) Descriptor.Save(DescriptorFile, IgnoredReason);
				}
			}
			RemoveNewEmptyDirectories(CreatedDirectories);
		}
	};
	if (!ManagedDirectory(OutAsset.PluginDirectory, OutError, &CreatedDirectories)) return false;
	if (!bHasIndex && !CreatedDirectories.ContainsByPredicate([&OutAsset](const FString& Directory) { return Directory.Equals(OutAsset.PluginDirectory, ESearchCase::IgnoreCase); }))
	{
		OutError = TEXT("The avatar plugin folder appeared during preparation and is not owned by this draft. It will not be overwritten.");
		return false;
	}
	bWorkspaceValidated = true;
	// A failed or cancelled copy is never advertised as uploadable, including across editor restarts.
	if (!SaveManifest(OutAsset, false, OutError)) return false;
	if (!ManagedDirectory(OutAsset.PluginDirectory / TEXT("Content"), OutError, &CreatedDirectories)) return false;
	FText Reason;
	const FString DescriptorFile = OutAsset.PluginDirectory / (OutAsset.PluginName + TEXT(".uplugin"));
	if (IsReparsePoint(DescriptorFile)) { OutError = TEXT("The generated avatar plugin descriptor is redirected and will not be changed."); return false; }
	if (!Descriptor.Save(DescriptorFile, Reason))
	{
		OutError = Reason.ToString();
		return false;
	}
	if (!MountPlugin(OutAsset, OutError)) return false;
	if (!SetHostCookExclusion(OutAsset, true, OutError)) return false;
	if (Request.IsCancelled && Request.IsCancelled()) { OutError = TEXT("Avatar preparation was cancelled."); return false; }

	FScopedSlowTask Progress(static_cast<float>(OutAsset.SourceToDestinationPackages.Num() * 2 + 3), NSLOCTEXT("ConvaiAvatarStudio", "PrepareAvatar", "Preparing avatar source…"));
	Progress.MakeDialog(true);
	Progress.EnterProgressFrame(1, NSLOCTEXT("ConvaiAvatarStudio", "CopyAvatar", "Copying avatar and dependencies…"));
	// Soft-reference rename installs session-global redirects. Restore the prior
	// state on every exit so a later save of the creator's source cannot silently point into our copy.
	TMap<FSoftObjectPath, FSoftObjectPath> PreviousRedirects;
	for (const auto& Pair : OutAsset.SourceToDestinationPackages)
	{
		const FString ObjectPath = Pair.Key.ToString() + TEXT(".") + FPackageName::GetShortName(Pair.Key);
		for (const FString& Path : {ObjectPath, ObjectPath + TEXT("_C")})
		{
			const FSoftObjectPath SourcePath(Path);
			PreviousRedirects.Add(SourcePath, GRedirectCollector.GetAssetPathRedirection(SourcePath));
		}
	}
	ON_SCOPE_EXIT
	{
		for (const auto& Pair : PreviousRedirects)
		{
			GRedirectCollector.RemoveAssetPathRedirection(Pair.Key);
			if (Pair.Value.IsValid()) GRedirectCollector.AddAssetPathRedirection(Pair.Key, Pair.Value);
		}
	};
	if (Request.bIncludeDiorama && !DuplicateDioramaLevel(DioramaSource, DioramaDestination, Request.DioramaFacts, OutError)) return false;
	for (const auto& Pair : OutAsset.SourceToDestinationPackages)
	{
		if ((Request.IsCancelled && Request.IsCancelled()) || Progress.ShouldCancel()) { OutError = TEXT("Avatar preparation was cancelled."); return false; }
		Progress.EnterProgressFrame(1, FText::FromString(TEXT("Copying dependency ") + Pair.Key.ToString()));
		if (ReusedEnumPackages.Contains(Pair.Value)) continue;
		if (!GeneratedDioramaPackages.Contains(Pair.Key) && !DuplicatePreparedPackage(Pair.Key, Pair.Value, OutError)) return false;
	}

	TMap<UObject*, UObject*> ObjectMap;
	TMap<FSoftObjectPath, FSoftObjectPath> SoftMap;
	TArray<UPackage*> DestinationPackages;
	for (const auto& Pair : OutAsset.SourceToDestinationPackages)
	{
		if ((Request.IsCancelled && Request.IsCancelled()) || Progress.ShouldCancel()) { OutError = TEXT("Avatar preparation was cancelled. Refresh from source to complete the saved staging copy."); return false; }
		UPackage* Source = LoadPackage(nullptr, *Pair.Key.ToString(), LOAD_None);
		UPackage* Destination = LoadPackage(nullptr, *Pair.Value.ToString(), LOAD_None);
		if (!Source || !Destination) { OutError = FString::Printf(TEXT("Could not load both copies of %s for reference validation."), *Pair.Key.ToString()); return false; }
		ObjectMap.Add(Source, Destination);
		if (!ReusedEnumPackages.Contains(Pair.Value)) DestinationPackages.AddUnique(Destination);
		TArray<UObject*> SourceObjects;
		GetPreparationObjectsWithOuter(Source, SourceObjects, true);
		for (UObject* Object : SourceObjects)
		{
			// Preserve the complete object suffix: generated classes, CDOs and nested subobjects all need remapping.
			FString TargetPath;
			if (DioramaWorld && (Object == DioramaWorld || Object->IsIn(DioramaWorld)))
			{
				const AActor* Actor = Cast<AActor>(Object) ? Cast<AActor>(Object) : Object->GetTypedOuter<AActor>();
				if (Actor && Request.DioramaFacts.AvatarActorNames.Contains(Actor->GetFName())) continue;
				const FString WorldPath = DioramaWorld->GetPathName();
				TargetPath = DioramaDestination.ToString() + TEXT(".") + FPackageName::GetShortName(DioramaDestination) + Object->GetPathName().Mid(WorldPath.Len());
			}
			else
			{
				FString Suffix = Object->GetPathName().Mid(Pair.Key.ToString().Len());
				if (GeneratedDioramaPackages.Contains(Pair.Key))
				{
					const FString OldRoot = TEXT(".") + FPackageName::GetShortName(Pair.Key);
					if (Suffix.StartsWith(OldRoot)) Suffix = TEXT(".") + FPackageName::GetShortName(Pair.Value) + Suffix.Mid(OldRoot.Len());
				}
				TargetPath = Pair.Value.ToString() + Suffix;
			}
			if (UObject* Target = StaticFindObject(nullptr, nullptr, *TargetPath))
			{
				ObjectMap.Add(Object, Target);
				SoftMap.Add(FSoftObjectPath(Object), FSoftObjectPath(Target));
				const FSoftObjectPath RedirectKey = FSoftObjectPath(Object).GetWithoutSubPath();
				if (!PreviousRedirects.Contains(RedirectKey)) PreviousRedirects.Add(RedirectKey, GRedirectCollector.GetAssetPathRedirection(RedirectKey));
			}
			else if (Object->IsAsset())
			{
				OutError = TEXT("The prepared package is missing a source asset export: ") + Object->GetPathName();
				return false;
			}
		}
	}
	TArray<FString> ModifiedFiles;

	Progress.EnterProgressFrame(1, NSLOCTEXT("ConvaiAvatarStudio", "RemapAvatar", "Updating prepared avatar references…"));
	if (!RemapAndCompilePreparedPackages(OutAsset, DestinationPackages, Mount, ObjectMap, SoftMap, [&Request, &Progress]()
		{ return (Request.IsCancelled && Request.IsCancelled()) || Progress.ShouldCancel(); }, OutError)) return false;
	if (Request.bIncludeDiorama)
	{
		UWorld* Copy = LoadDioramaWorld(DioramaDestination, OutError);
		if (!Copy || !ReinstanceDioramaActors(Copy, OutAsset.SourceToDestinationPackages, OutError) ||
			!DioramaCopyMatches(Copy, Request.DioramaFacts, OutError)) return false;
	}
	for (UPackage* Package : DestinationPackages)
	{
		if ((Request.IsCancelled && Request.IsCancelled()) || Progress.ShouldCancel()) { OutError = TEXT("Avatar preparation was cancelled while saving its prepared source."); return false; }
		Progress.EnterProgressFrame(1, FText::FromString(TEXT("Saving ") + FPackageName::GetShortName(Package->GetName())));
		if (!SavePackage(Package, OutError)) return false;
		ModifiedFiles.Add(FPackageName::LongPackageNameToFilename(Package->GetName(), Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension()));
	}
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanModifiedAssetFiles(ModifiedFiles);
	Progress.EnterProgressFrame(1, NSLOCTEXT("ConvaiAvatarStudio", "VerifyAvatar", "Verifying the prepared avatar…"));
	Blueprint = nullptr;
	if (!IncludeReferencedConvaiContent(OutAsset, Request.IsCancelled, OutError)) return false;
	if (!ValidatePreparedAsset(OutAsset, OutError) || !EnsureProxyLink(OutAsset, OutError)) return false;
	if (!ConvaiAvatarSourceRevision::CapturePrepared(OutAsset.PluginDirectory, OutAsset.PreparedFileHashes, Request.IsCancelled, OutError)) return false;
	if (!ReusedEnumPackages.IsEmpty())
	{
		// Compilation and asset notifications must not silently mutate a kept type.
		TSet<FName> VerifiedEnums;
		FConvaiAvatarPreparedAsset VerifiedSource = OutAsset;
		for (auto It = VerifiedSource.SourceToDestinationPackages.CreateIterator(); It; ++It)
			if (!ReusedEnumPackages.Contains(It.Value())) It.RemoveCurrent();
		if (!ConvaiAvatarSourceRevision::CaptureSource(VerifiedSource.SourceToDestinationPackages, VerifiedSource.SourceFileHashes,
			VerifiedSource.SourcePackageHashes, Request.IsCancelled, OutError) ||
			!ConvaiAvatarReusableEnums::Find(Previous, VerifiedSource, OutAsset.PreparedFileHashes, VerifiedEnums, OutError)) return false;
		for (FName Package : ReusedEnumPackages)
		{
			if (!VerifiedEnums.Contains(Package))
			{ OutError = TEXT("An avatar dependency changed during preparation. Save your changes and retry the upload."); return false; }
		}
		for (const TStrongObjectPtr<UObject>& Object : ReusedEnumObjects)
		{
			if (!IsValid(Object.Get()) || !ReusedEnumPackages.Contains(Object->GetOutermost()->GetFName()) || Object->GetOutermost()->IsDirty())
			{ OutError = TEXT("An avatar dependency changed during preparation. Save your changes and retry the upload."); return false; }
		}
	}
	bPreparationComplete = UpdatePluginDependencies(OutAsset, OutError) && SaveManifest(OutAsset, true, OutError);
	return bPreparationComplete;
}

bool FConvaiAvatarWorkspace::ValidatePreparedAsset(FConvaiAvatarPreparedAsset& Asset, FString& OutError)
{
	check(IsInGameThread());
	const FString Mount = TEXT("/") + Asset.PluginName + TEXT("/");
	if (!IsSafePluginName(Asset.PluginName) || !Asset.EntryPoint.GetLongPackageName().StartsWith(Mount) ||
		!Normalized(Asset.PluginDirectory).Equals(Normalized(FPaths::Combine(GetAvatarsDirectory(), Asset.PluginName)), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("The avatar entry point or plugin folder is outside its managed plugin.");
		return false;
	}
	if (!MountPlugin(Asset, OutError)) return false;
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset.EntryPoint.TryLoad());
	if (!Blueprint || !Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()) || Blueprint->Status == BS_Error)
	{
		OutError = TEXT("The prepared avatar Blueprint cannot load or has compile errors. Open and compile it before uploading.");
		return false;
	}
	TMap<FName, FName> Unresolved;
	ResolveLegacyMetaHumanChoice(Asset, Blueprint);
	TArray<FName> Closure, Missing;
	TArray<FString> RequiredPlugins;

	const FName DioramaRoot = Asset.Diorama.IsSet() ? FName(*Asset.Diorama->Level) : NAME_None;
	if (Asset.Diorama.IsSet() && !Asset.Diorama->Level.StartsWith(Mount))
	{ OutError = TEXT("The diorama level is outside its managed avatar plugin."); return false; }
	if (!Gather(FName(*Asset.EntryPoint.GetLongPackageName()), Mount, Unresolved, RequiredPlugins, Closure, Missing, {}, OutError, DioramaRoot, nullptr, Asset.bIncludeConvaiContent)) return false;
	if (Asset.Diorama.IsSet() && !Gather(DioramaRoot, Mount, Unresolved, RequiredPlugins, Closure, Missing, {}, OutError, DioramaRoot)) return false;

	for (FName Package : Missing)
	{
		if (Package.ToString().StartsWith(Mount))
		{
			OutError = FString::Printf(TEXT("This avatar still references a missing asset: %s. Restore that asset or remove its references, save, and retry."), *Package.ToString());
			return false;
		}
	}
	FConvaiAvatarDependencyReview Review;
	if (!ReviewMissingPackages(Missing, Asset.AcknowledgedMissingPackages, Review, OutError)) return false;
	if (Review.bRequiresAcknowledgement) { OutError = MissingReviewMessage(Review); return false; }
	if (!Unresolved.IsEmpty())
	{
		OutError = UnresolvedDependencyMessage(Asset, Unresolved, Closure, TEXT("validate-prepared"));
		return false;
	}
	TSet<FName> Reachable;
	for (FName Package : Closure) Reachable.Add(Package);
	for (FName Package : Missing) Reachable.Add(Package);
	TArray<FName> RemovedMappings;
	for (const auto& Pair : Asset.SourceToDestinationPackages)
	{
		if (!FPackageName::DoesPackageExist(Pair.Value.ToString()))
		{
			if (Reachable.Contains(Pair.Value))
			{
				OutError = FString::Printf(TEXT("This avatar still references a missing asset: %s. Restore that asset or remove its references, save, and retry."), *Pair.Value.ToString());
				return false;
			}
			RemovedMappings.Add(Pair.Key);
		}
	}
	// A saved edit can remove a dependency that was copied during an earlier preparation.
	// Forget only absent, unreachable destinations after all validation succeeds; never delete files.
	for (FName Source : RemovedMappings)
	{
		Asset.SourceToDestinationPackages.Remove(Source);
		Asset.SourcePackageHashes.Remove(Source);
	}
	Asset.RequiredPlugins = MoveTemp(RequiredPlugins);
	Asset.AcknowledgedMissingPackages = MoveTemp(Review.MissingPackages);
	Asset.PackageCount = 0;
	for (FName Package : Closure) if (Package.ToString().StartsWith(Mount)) ++Asset.PackageCount;
	return true;
}

bool FConvaiAvatarWorkspace::SetHostCookExclusion(FConvaiAvatarPreparedAsset& Asset, bool bExclude, FString& OutError)
{
	if (!FConvaiAvatarProjectConfiguration::SetStagingCookExclusion(FPaths::ProjectDir(), Asset.PluginName, bExclude, OutError)) return false;
	Asset.bIsStaging = bExclude;
	TSharedPtr<FJsonObject> ExistingRecord;
	if (ReadWorkspaceJson(IndexFile(Asset.AssetId), ExistingRecord))
	{
		bool bReady = false;
		ExistingRecord->TryGetBoolField(TEXT("ready"), bReady);
		return SaveManifest(Asset, bReady, OutError);
	}
	return true;
}

bool FConvaiAvatarWorkspace::RegisterDownloadedAsset(const FString& AssetId, const FString& PluginDirectory,
	const FSoftObjectPath& EntryPoint, FConvaiAvatarPreparedAsset& OutAsset, FString& OutError, TOptional<bool> MetaHumanChoice,
	TOptional<TArray<FName>> AcknowledgedMissingPackages, TOptional<bool> IncludeConvaiContent,
	TOptional<TMap<FName, FName>> SourceToDestinationPackages)
{
	check(IsInGameThread());
	OutAsset = {};
	OutAsset.AssetId = AssetId;
	OutAsset.PluginName = FPaths::GetCleanFilename(Normalized(PluginDirectory));
	OutAsset.EntryPoint = EntryPoint;
	OutAsset.bIsStaging = false;
	bool bPreviouslyStaging = false;
	OutAsset.bHasMetaHumanChoice = false;
	FillDirectories(OutAsset);
	if (AssetId.TrimStartAndEnd().IsEmpty() || AssetId.Len() > 256 || !IsSafePluginName(OutAsset.PluginName) ||
		!Normalized(PluginDirectory).Equals(OutAsset.PluginDirectory, ESearchCase::IgnoreCase))
	{
		OutError = TEXT("Extract the avatar content-only plugin into this project's Plugins/ConvaiAvatars folder before importing it.");
		return false;
	}
	TSharedPtr<FJsonObject> Ownership;
	FString OwnerId;
	if (ReadWorkspaceJson(FPaths::Combine(PluginDirectory, ManifestName), Ownership) &&
		Ownership->TryGetStringField(TEXT("asset_id"), OwnerId) && OwnerId != AssetId)
	{
		OutError = TEXT("This plugin belongs to a different asset ID. It will not be adopted.");
		return false;
	}
	TSharedPtr<FJsonObject> PreviousDocument;
	bool bHasPreviousRecord = false;
	if (!FindManifestDocument(AssetId, PreviousDocument, bHasPreviousRecord, OutError)) return false;
	if (bHasPreviousRecord)
	{
		FConvaiAvatarPreparedAsset Previous;
		bool bReady = false;
		if (!ReadManifest(AssetId, Previous, bReady, OutError)) return false;
		if (Previous.PluginName != OutAsset.PluginName) { OutError = TEXT("This asset ID already has a local plugin with a different mount name."); return false; }
		bPreviouslyStaging = Previous.bIsStaging;
		OutAsset.bIsMetaHuman = Previous.bIsMetaHuman;
		OutAsset.bHasMetaHumanChoice = Previous.bHasMetaHumanChoice;
		OutAsset.Diorama = Previous.Diorama;
		OutAsset.AcknowledgedMissingPackages = Previous.AcknowledgedMissingPackages;
		// Get latest updates a local author's prepared copy, not the source binding they edit.
		// Retain both baselines: newly downloaded bytes must still conflict with a later original edit.
		OutAsset.OriginalEntryPoint = Previous.OriginalEntryPoint;
		OutAsset.SourceToDestinationPackages = Previous.SourceToDestinationPackages;
		OutAsset.RetainedSourceToDestinationPackages = Previous.RetainedSourceToDestinationPackages;
		OutAsset.SourcePackageHashes = Previous.SourcePackageHashes;
		OutAsset.SourceFileHashes = Previous.SourceFileHashes;
		OutAsset.PreparedFileHashes = Previous.PreparedFileHashes;
	}
	// Source bytes determine this policy. A legacy archive may intentionally retain
	// /ConvAI references even when an older local revision copied those assets.
	OutAsset.bIncludeConvaiContent = IncludeConvaiContent.Get(false);
	if (SourceToDestinationPackages.IsSet())
	{
		const auto& Copies = SourceToDestinationPackages.GetValue();
		if (Copies.Num() > MaximumPackages) { OutError = TEXT("The downloaded avatar has too many copied package records."); return false; }
		TSet<FName> Targets;
		const FString AvatarMount = TEXT("/") + OutAsset.PluginName + TEXT("/");
		for (const auto& Pair : Copies)
		{
			const FString Source = Pair.Key.ToString(), Destination = Pair.Value.ToString();
			if (Source.Len() > 1023 || !FPackageName::IsValidTextForLongPackageName(Source) || Source.StartsWith(TEXT("/Script/")) || Source.StartsWith(AvatarMount) ||
				Pair.Value != MakeDestinationPackage(Pair.Key, OutAsset.PluginName) || Targets.Contains(Pair.Value))
			{ OutError = TEXT("The downloaded avatar contains an invalid or duplicate source-copy mapping."); return false; }
			const FString File = OutAsset.PluginDirectory / TEXT("Content") / Destination.Mid(AvatarMount.Len()) + FPackageName::GetAssetPackageExtension();
			if (!IsInside(File, OutAsset.PluginDirectory) || !IFileManager::Get().FileExists(*File))
			{ OutError = TEXT("The downloaded avatar is missing the recorded copied package: ") + Destination; return false; }
			for (FString Cursor = File; IsInside(Cursor, OutAsset.PluginDirectory); Cursor = FPaths::GetPath(Cursor))
				if (IsReparsePoint(Cursor)) { OutError = TEXT("The downloaded avatar's copied package path is redirected."); return false; }
			Targets.Add(Pair.Value);
			OutAsset.RetainedSourceToDestinationPackages.Add(Pair.Key, Pair.Value);
		}
	}
	if (AcknowledgedMissingPackages.IsSet())
	{
		FConvaiAvatarDependencyReview Review;
		if (!ReviewMissingPackages({}, AcknowledgedMissingPackages.GetValue(), Review, OutError)) return false;
		const FString AvatarMount = TEXT("/") + OutAsset.PluginName + TEXT("/");
		if (AcknowledgedMissingPackages.GetValue().ContainsByPredicate([&AvatarMount](FName Name) { return Name.ToString().StartsWith(AvatarMount); }))
		{ OutError = TEXT("The downloaded avatar cannot acknowledge missing files inside its own plugin."); return false; }
		OutAsset.AcknowledgedMissingPackages.Reset();
		for (FName Name : AcknowledgedMissingPackages.GetValue()) OutAsset.AcknowledgedMissingPackages.AddUnique(Name);
		OutAsset.AcknowledgedMissingPackages.Sort(FNameLexicalLess());
	}
	FPluginDescriptor Descriptor;
	FText Reason;
	if (!Descriptor.Load(FPaths::Combine(PluginDirectory, OutAsset.PluginName + TEXT(".uplugin")), Reason)) { OutError = Reason.ToString(); return false; }
	if (!Descriptor.bCanContainContent || !Descriptor.Modules.IsEmpty() || Descriptor.bCanContainVerse || Descriptor.bIsPluginExtension)
	{
		OutError = TEXT("Avatar downloads must contain a content-only plugin. Runtime code dependencies must already be installed separately.");
		return false;
	}
	OutAsset.DisplayName = Descriptor.FriendlyName;
	if (MetaHumanChoice.IsSet())
	{
		OutAsset.bIsMetaHuman = MetaHumanChoice.GetValue();
		OutAsset.bHasMetaHumanChoice = true;
	}
	for (const FPluginReferenceDescriptor& Dependency : Descriptor.Plugins)
	{
		if (!Dependency.bEnabled) continue;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Dependency.Name);
		if (!Plugin.IsValid() || !Plugin->IsEnabled()) { OutError = FString::Printf(TEXT("Install and enable %s, restart the editor, and import the avatar again."), *Dependency.Name); return false; }
		OutAsset.RequiredPlugins.AddUnique(Dependency.Name);
	}
	if (!ValidatePreparedAsset(OutAsset, OutError) || !EnsureProxyLink(OutAsset, OutError)) return false;
	// Normal imported avatars may be used in scenes and keep the host's normal cooking behavior.
	if (bPreviouslyStaging && !SetHostCookExclusion(OutAsset, false, OutError)) return false;
	return SaveManifest(OutAsset, true, OutError);
}

bool FConvaiAvatarWorkspace::EnsureProxyLink(const FConvaiAvatarPreparedAsset& Asset, FString& OutError)
{
	if (!Normalized(GetProxyDirectory()).Equals(Normalized(FPaths::ProjectDir() / TEXT("Saved/ConvaiAvatarStudio/Uploader")), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("Cloud Avatars needs this project's Saved folder for its uploader cache. Restart Unreal without a custom user-data path and retry.");
		return false;
	}
	const FString ExpectedSource = Normalized(FPaths::Combine(GetAvatarsDirectory(), Asset.PluginName));
	const FString ExpectedLink = Normalized(FPaths::Combine(GetProxyDirectory(), TEXT("Plugins/ConvaiAvatars"), Asset.PluginName));
	if (!IsSafePluginName(Asset.PluginName) || !Normalized(Asset.PluginDirectory).Equals(ExpectedSource, ESearchCase::IgnoreCase) ||
		!Normalized(Asset.ProxyDirectory).Equals(Normalized(GetProxyDirectory()), ESearchCase::IgnoreCase) ||
		!Normalized(Asset.ProxyPluginDirectory).Equals(ExpectedLink, ESearchCase::IgnoreCase))
	{
		OutError = TEXT("The avatar link must connect its managed host plugin to this project's uploader cache.");
		return false;
	}
	// Packaging reads cached descriptors for disabled avatars as well as the selected one.
	// Verify every shortcut first so a copied project cannot inspect another project's source.
	return ConvaiAvatarWorkspaceLinks::EnsureAll(FPaths::ProjectDir(), Asset.PluginName, OutError);
}
