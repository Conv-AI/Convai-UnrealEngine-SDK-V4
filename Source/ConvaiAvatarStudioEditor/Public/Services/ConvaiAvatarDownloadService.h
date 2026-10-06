// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Services/ConvaiAvatarAssetsClient.h"

/** A value-only snapshot: FJsonObject shared pointers must not cross archive worker threads. */
struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarDownloadSource
{
	FString AssetId;
	FString AvatarName;
	FString PluginName;
	FString EntryPoint;
	FString SourceVersion;
	/** Absent only for legacy records. Explicit false must survive download/restart. */
	TOptional<bool> MetaHumanChoice;
};

struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarPendingDownload
{
	FString JobId;
	FString AssetId;
	FString AvatarName;
	FString PluginName;
	FString EntryPoint;
	FString SourceVersion;
	TOptional<bool> MetaHumanChoice;
	FString JobDirectory;
	/** Present only when the source explicitly records exact missing-reference approvals. */
	TOptional<TArray<FName>> AcknowledgedMissingPackages;
	/** Taken only from this source archive; absent legacy sources default to false. */
	TOptional<bool> IncludeConvaiContent;
	/** Validated copy identities from the selected source; never original-project provenance. */
	TOptional<TMap<FName, FName>> SourceToDestinationPackages;
	FString StagedDirectory;
	FString BackupDirectory;
	TArray<FString> RequiredPlugins;
	/** Validated archive paths relative to host Content. Never arbitrary absolute paths. */
	TMap<FString, FString> SupportFiles;
	/** Windows volume/file identities acquired before each owned support file is moved into Content. */
	TMap<FString, FString> SupportOwnedIdentities;
	FString SupportPhase = TEXT("pending");
	bool bReplaceExisting = false;
	bool bReplacementConfirmed = false;
	bool bHasPreviousWorkspaceState = false;
	bool bPreviousCookExcluded = false;
	FString Phase = TEXT("staged");
};

enum class EConvaiAvatarDownloadApplyResult : uint8 { Applied, Pending, Failed };

/** Coordinates validated downloads, dependency prompts, persistent retries, and local replacements. */
class CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarDownloadService
{
public:
	/** Game thread. Snapshot only the fields required for local validation; no URLs or credentials. */
	static bool SnapshotSource(const FConvaiAvatarAsset& Asset, FConvaiAvatarDownloadSource& OutSource, FString& OutError);
	/** Worker thread. No existing plugin is modified. Consent is supplied by the editor controller. */
	static bool Stage(const FString& Zip, const FConvaiAvatarDownloadSource& Source, bool bReplaceConfirmed,
		FConvaiAvatarPendingDownload& OutJob, FString& OutError);
	/** Game thread; missing/disabled dependencies and loaded content keep the job pending on disk.
	 * bRegister=false is available during module startup, before the editor opens a map.
	 * Only an explicit Download/Get latest action sets bUserInitiated; background calls never
	 * close editors or unload loaded content. Dirty changes always require fresh confirmation.
	 */
	static EConvaiAvatarDownloadApplyResult TryApply(FConvaiAvatarPendingDownload& Job, bool bPromptDependencies,
		FString& OutNotice, FString& OutError, bool bRegister = true, bool bUserInitiated = false);
	/** Explicit Download/Get latest lookup. Retires only untouched owned stages with
	 * missing required source, retaining their files and permitting a fresh download.
	 */
	static bool FindPending(const FString& AssetId, FConvaiAvatarPendingDownload& OutJob, FString& OutError);
	static void GetPending(TArray<FConvaiAvatarPendingDownload>& OutJobs, FString& OutError);
	static FString PendingRoot();
	static bool ReadJob(const FString& JobFile, FConvaiAvatarPendingDownload& OutJob, FString& OutError);
	static bool SaveJob(const FConvaiAvatarPendingDownload& Job, FString& OutError);
};
