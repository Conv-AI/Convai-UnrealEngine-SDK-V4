// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include <atomic>

class FJsonObject;
struct FConvaiAvatarPreparedAsset;

namespace ConvaiAvatarSourceArchivePrivate
{
/** powershell.exe arguments that run the helper Script on one request and result file. The three paths are generated
 * locally and cannot hold a Windows filename quote; every value from an archive travels in the request JSON. */
FString HelperArguments(const FString& Script, const FString& RequestPath, const FString& ResultPath);
/** Runs Script, the bundled Archive.ps1 that Initialize resolved, on Request in its own ArchiveJobs folder and reads its
 * result. Invoke from a worker. Once Cancelled is set, or after 6 h 2 min, it terminates the helper's whole process tree
 * and fails; a create request then has its <output_zip>.<32 hex digits>.part deleted. An import stopped that way leaves
 * the helper's .<plugin>-import-<guid> and .base-import-<guid> folders in its destination root for the caller. */
bool RunHelper(const FString& Script, const TSharedRef<FJsonObject>& Request, TSharedPtr<FJsonObject>& Result, FString& Error,
	const std::atomic<bool>& Cancelled);
/** FConvaiAvatarSourceArchive::Create that stops once Cancelled is set. */
bool Create(const FConvaiAvatarPreparedAsset& Asset, const FString& ProxyUprojectPath, const FString& OutputZip, FString& OutError,
	const FString& ExpectedBaseManifestHash, const std::atomic<bool>& Cancelled);
/** FConvaiAvatarSourceArchive::Stage that stops once Cancelled is set. */
bool Stage(const FString& Zip, const FString& AssetId, const FString& MetadataPluginName, const FSoftObjectPath& EntryPoint,
	const FString& StagingRoot, FString& OutStagedDirectory, TArray<FString>& OutRequiredPlugins, FString& OutError,
	TMap<FString, FString>* OutSupportFiles, TOptional<bool>* OutMetaHumanChoice, TOptional<TArray<FName>>* OutAcknowledgedMissingPackages,
	TOptional<bool>* OutIncludeConvaiContent, TOptional<TMap<FName, FName>>* OutSourceToDestinationPackages, const std::atomic<bool>& Cancelled, FString* OutNativeRuntimeContract = nullptr);
}
