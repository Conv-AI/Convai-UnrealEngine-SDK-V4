// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include <atomic>

class UAssetEditorSubsystem;
class UPackage;
struct FConvaiAvatarDownloadSource;
struct FConvaiAvatarPendingDownload;

namespace ConvaiAvatarDownloadServicePrivate
{
/** Worker thread. FConvaiAvatarDownloadService::Stage that stops once Cancelled is set; a cancelled Stage deletes the job
 * folder it created, with whatever the stopped helper had extracted into it. */
bool Stage(const FString& Zip, const FConvaiAvatarDownloadSource& Source, bool bReplaceConfirmed, FConvaiAvatarPendingDownload& OutJob,
	FString& OutError, const std::atomic<bool>& Cancelled);
/** Game thread. Closes the asset editors of the packages' assets; true when none of them stays open. */
bool CloseAvatarEditors(const TArray<UPackage*>& Packages, UAssetEditorSubsystem& Editors);
/** The ancestor walk that refuses symbolic links and junctions, stopped at Anchor when Path lies inside it: the anchor
 * itself may be reached through a link, as the workspace accepts for the project. A Path outside Anchor is walked to the root. */
bool NoLinksBelow(const FString& Path, const FString& Anchor, FString& Error);
/** Installed with no shared-support step in flight, but its staged source is gone and its avatar folder is missing or no
 * longer carries its receipt or ownership: nothing is left to finish. FindPending retires it; a Refresh only reports it. */
bool IsDeadInstalledJob(const FConvaiAvatarPendingDownload& Job);
/** Deletes a complete job's staged copies, its Source/ and SupportInstall/ folders, and returns what stayed. Nothing reads
 * them once the job is complete. Its job.json and its backup under Saved/ConvaiAvatarStudio/Backups stay. */
TArray<FString> DropStagedFiles(const FString& JobDirectory);
/** Game thread. Removes the avatar's /<PluginName>/ mount point, or registers it on Directory/Content and scans it. A mount
 * point that is still registered is left unscanned: every move of the avatar's folder comes after a remove. */
void RefreshMount(const FString& PluginName, const FString& Directory, bool bRemove);
/** Writes the job's job.json exactly as SaveJob does, without SaveJob's validation, whose link walks repeat for every support
 * file. Only for a job validated earlier in the same game-thread operation: nothing here checks where it writes. */
bool PersistJob(const FConvaiAvatarPendingDownload& Job, FString& Error);
#if WITH_DEV_AUTOMATION_TESTS
/** A valid staged journal with a fresh identity under the pending-download root. Nothing is written. */
FConvaiAvatarPendingDownload MakePendingFixture(const FString& AvatarName);
/** Test tap: the synchronous rescans RefreshMount has run. */
extern int32 RefreshMountScans;
#endif
}
