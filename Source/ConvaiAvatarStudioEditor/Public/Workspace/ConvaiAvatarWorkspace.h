// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Diorama/ConvaiAvatarDiorama.h"
#include "UObject/SoftObjectPath.h"

class UPackage;

/** Local preparation only. Call on the editor game thread; HTTP and cooking are separate jobs. */
struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarPrepareRequest
{
	FString AssetId;
	FString DisplayName;
	FSoftObjectPath Blueprint;
	/** Explicit create/retry choice; cloud-bound prepared avatars retain their saved choice on updates. */
	bool bIsMetaHuman = true;
	bool bIncludeDiorama = false;
	FString DioramaSourceLevel;
	FConvaiAvatarDioramaFacts DioramaFacts;
	/** Copy referenced Convai plugin assets into this avatar; native runtime modules remain required. */
	bool bIncludeConvaiContent = false;
	/** Check the locally authored original and refresh only when its saved dependency graph changed. */
	bool bRefreshFromSource = false;
	/** A new local draft that has not been bound to an asset in the service. */
	bool bIsNewDraft = false;
	/** Exact absent package paths explicitly reviewed by the user; never a wildcard or permission to skip copy failures. */
	TArray<FName> AcknowledgedMissingPackages;
	/** Exact revision returned by ReviewSourceChanges; authorizes replacing the reviewed prepared edits from the original. */
	FString AcknowledgedSourceConflict;
	TFunction<bool()> IsCancelled;
};

/** Saved source/copy comparison. The fingerprint binds confirmation to the exact files reviewed. */
struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarSourceReview
{
	/** Refresh may be needed to establish missing history or finish an incomplete copy without proven source edits. */
	bool bNeedsRefresh = false;
	bool bOriginalChanged = false;
	bool bPreparedChanged = false;
	bool bLegacyBaseline = false;
	bool bRequiresConfirmation = false;
	TArray<FString> ChangedOriginalFiles;
	TArray<FString> ChangedPreparedFiles;
	FString ConflictFingerprint;
};

/** Read-only dependency review. Missing references can be acknowledged; invalid selected avatars remain fatal. */
struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarDependencyReview
{
	TArray<FName> MissingPackages;
	bool bRequiresAcknowledgement = false;
	FConvaiAvatarSourceReview SourceChanges;
};

struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarPreparedAsset
{
	FString AssetId;
	FString DisplayName;
	FString PluginName;
	FString PluginDirectory;
	FString ProxyDirectory;
	FString ProxyPluginDirectory;
	FSoftObjectPath EntryPoint;
	FSoftObjectPath OriginalEntryPoint;
	TMap<FName, FName> SourceToDestinationPackages;
	/** Ownership of retained copies no longer in the active source graph; never fingerprinted or recopied automatically. */
	TMap<FName, FName> RetainedSourceToDestinationPackages;
	/** MD5 fingerprints identify source revisions; they are not security signatures. */
	TMap<FName, FString> SourcePackageHashes;
	/** Full package families from the last successful source refresh, including bulk/optional payloads. */
	TMap<FString, FString> SourceFileHashes;
	/** Every saved Content package file at the same refresh; retained during plugin-only edits. */
	TMap<FString, FString> PreparedFileHashes;
	TArray<FString> RequiredPlugins;
	/** Only missing references from an explicit preparation review, retained for later validation. */
	TArray<FName> AcknowledgedMissingPackages;
	int32 PackageCount = 0;
	bool bIsStaging = true;
	bool bIsMetaHuman = false;
	TOptional<FConvaiAvatarDioramaRecord> Diorama;
	/** Missing in legacy records means false; never infer this from package names. */
	bool bIncludeConvaiContent = false;
	/** Local-only resumable inclusion phase; portable source/downloads never grant this state. */
	bool bConvaiContentPending = false;
	/** False only for legacy records with no choice; resolve when their Blueprint is already loaded. */
	bool bHasMetaHumanChoice = true;
	bool bIsDraft = false;
};

class CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarWorkspace
{
public:
	static FString GetProxyDirectory();
	static FString GetAvatarsDirectory();
	static FString MakePluginName(const FString& AssetId);
	static bool IsSafePluginName(const FString& PluginName);
	static FName MakeDestinationPackage(FName Source, const FString& PluginName);
	/** Read-only save review of the selected avatar's dirty dependencies and owned refresh targets. Never includes levels or other managed avatars. */
	static bool CollectDirtyPackages(const FConvaiAvatarPrepareRequest& Request, TArray<UPackage*>& OutPackages, FString& OutError);

	/** Returns true for a completed review, including missing dependencies; inspect bRequiresAcknowledgement before continuing. Writes no files. */
	static bool ReviewDependencies(const FConvaiAvatarPrepareRequest& Request, FConvaiAvatarDependencyReview& OutReview, FString& OutError);
	/** Compares the saved original and canonical copy without unloading or writing avatar content. */
	static bool ReviewSourceChanges(const FConvaiAvatarPrepareRequest& Request, FConvaiAvatarSourceReview& OutReview, FString& OutError);
	/** Repeats the dependency review before workspace mutation; an updated review is returned when new missing paths need acknowledgement. */
	static bool Prepare(const FConvaiAvatarPrepareRequest& Request, FConvaiAvatarPreparedAsset& OutAsset, FString& OutError, FConvaiAvatarDependencyReview* OutReview = nullptr);
	static bool FindPreparedAsset(const FString& AssetId, FConvaiAvatarPreparedAsset& OutAsset, FString& OutError);
	/** Reads/rebuilds the local index without mounting incomplete content. */
	static bool FindLocalAssetRecord(const FString& AssetId, FConvaiAvatarPreparedAsset& OutAsset, bool& bReady, FString& OutError);
	static bool ListLocalAssetRecords(TArray<FConvaiAvatarPreparedAsset>& OutAssets, TSet<FString>& OutIncompleteAssetIds, FString& OutError);
	/** Explicit local-draft discard only. Refuses cloud-bound, redirected, or loaded content. */
	static bool DiscardLocalDraft(const FString& AssetId, FString& OutError);
	/** Bind a locally prepared draft to its newly-created remote ID without renaming the plugin mount. */
	static bool RebindPreparedAsset(FConvaiAvatarPreparedAsset& Asset, const FString& NewAssetId, FString& OutError);
	static bool ValidatePreparedAsset(FConvaiAvatarPreparedAsset& Asset, FString& OutError);
	/** Validates an already-extracted content-only plugin before adopting it. Does not extract archives. */
	static bool RegisterDownloadedAsset(const FString& AssetId, const FString& PluginDirectory,
		const FSoftObjectPath& EntryPoint, FConvaiAvatarPreparedAsset& OutAsset, FString& OutError, TOptional<bool> MetaHumanChoice = {},
		TOptional<TArray<FName>> AcknowledgedMissingPackages = {}, TOptional<bool> IncludeConvaiContent = {},
		TOptional<TMap<FName, FName>> SourceToDestinationPackages = {});
	/** Verify all cached avatar shortcuts against this project; repair owned relocated links, refusing unknown ones. */
	static bool EnsureProxyLink(const FConvaiAvatarPreparedAsset& Asset, FString& OutError);
	/** Removes only this managed staging exclusion, e.g. when the user chooses to ship the local avatar. */
	static bool SetHostCookExclusion(FConvaiAvatarPreparedAsset& Asset, bool bExclude, FString& OutError);
};
