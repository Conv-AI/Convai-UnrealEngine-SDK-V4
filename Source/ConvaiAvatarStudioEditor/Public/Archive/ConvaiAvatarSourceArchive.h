// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

struct FConvaiAvatarPreparedAsset;

/** Streamed Zip64 source archives. Runs a bundled helper synchronously; invoke from a worker. */
class CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarSourceArchive
{
public:
	/** Resolve the bundled helper on the game thread before invoking archive work. */
	static bool Initialize(FString& OutError);
	/** ExpectedBaseManifestHash binds proxy support files to the completed cook; required when that snapshot exists. */
	static bool Create(const FConvaiAvatarPreparedAsset& Asset, const FString& ProxyUprojectPath,
		const FString& OutputZip, FString& OutError, const FString& ExpectedBaseManifestHash = FString());
	/** Installs only the selected content-only plugin. Existing destinations are never replaced.
	 * Call Workspace::RegisterDownloadedAsset on the game thread after enabling returned dependencies.
	 */
	static bool Import(const FString& Zip, const FString& AssetId, const FString& MetadataPluginName,
		const FSoftObjectPath& EntryPoint, FString& OutInstalledDirectory,
		TArray<FString>& OutRequiredPlugins, FString& OutError, TOptional<bool>* OutMetaHumanChoice = nullptr,
		TOptional<TArray<FName>>* OutAcknowledgedMissingPackages = nullptr, TOptional<bool>* OutIncludeConvaiContent = nullptr,
		TOptional<TMap<FName, FName>>* OutSourceToDestinationPackages = nullptr);
	/** Validate and extract into an isolated staging root for dependency review or a pending update. */
	static bool Stage(const FString& Zip, const FString& AssetId, const FString& MetadataPluginName,
		const FSoftObjectPath& EntryPoint, const FString& StagingRoot, FString& OutStagedDirectory,
		TArray<FString>& OutRequiredPlugins, FString& OutError, TMap<FString, FString>* OutSupportFiles = nullptr,
		TOptional<bool>* OutMetaHumanChoice = nullptr, TOptional<TArray<FName>>* OutAcknowledgedMissingPackages = nullptr,
		TOptional<bool>* OutIncludeConvaiContent = nullptr, TOptional<TMap<FName, FName>>* OutSourceToDestinationPackages = nullptr);
};
