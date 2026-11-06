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
/** Worker thread. FConvaiAvatarDownloadService::Stage that stops once Cancelled is set, and saves no job.json: the game thread
 * saves it once the download is still wanted, so until then nothing applies it. Any failure, a cancel included, hands the job
 * folder it created, with whatever the helper had extracted into it, to DropJobFolder. */
bool Stage(const FString& Zip, const FConvaiAvatarDownloadSource& Source, bool bReplaceConfirmed, FConvaiAvatarPendingDownload& OutJob,
	FString& OutError, const std::atomic<bool>& Cancelled);
/** Any thread. Deletes a job folder under FConvaiAvatarDownloadService::PendingRoot() on the thread pool, without following links,
 * and logs what stayed. A folder whose delete is already queued or running is left to it. */
void DropJobFolder(const FString& JobDirectory);
/** Game thread. Closes the asset editors of the packages' assets; true when none of them stays open. */
bool CloseAvatarEditors(const TArray<UPackage*>& Packages, UAssetEditorSubsystem& Editors);
/** The ancestor walk that refuses symbolic links and junctions, stopped at Anchor when Path lies inside it: the anchor
 * itself may be reached through a link, as the workspace accepts for the project. A Path outside Anchor is walked to the root. */
bool NoLinksBelow(const FString& Path, const FString& Anchor, FString& Error);
/** Installed with no shared-support step in flight, but its staged source is gone and its avatar folder is missing or no
 * longer carries its receipt or ownership: nothing is left to finish. FindPending retires it, and so does a Refresh once its
 * avatar folder is gone. */
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
/** Game thread, the user's explicit discard of a pending download. Restores what the job replaced and retires it. An avatar that
 * cannot unload, or the user's first discard whose restore stops on a file in use or a failed write, keeps it pending until the
 * next editor start finishes the restore. A restore that can never finish, or one stopped again after the user discarded it before,
 * is reported in OutError and the job is retired anyway, so a new download can start. A staged job, not installed yet, is retired
 * after one rollback of the support files it added. Every file is kept. */
void Discard(FConvaiAvatarPendingDownload& Job, FString& OutNotice, FString& OutError);
/** Whether the job's saved journal says it is finished, complete or abandoned: nothing applies or reports it again. */
bool IsFinishedJob(const FConvaiAvatarPendingDownload& Job);
/** Unset unless the job is being discarded: why registration rejected its source, or empty for the user's own discard. */
TOptional<FString> DiscardingReason(const FConvaiAvatarPendingDownload& Job);
#if WITH_DEV_AUTOMATION_TESTS
/** A valid staged journal with a fresh identity under the pending-download root. Nothing is written. */
FConvaiAvatarPendingDownload MakePendingFixture(const FString& AvatarName);
/** Saves the fixture's journal as complete, so a stopped test leaves nothing for startup recovery, drops its plugin and mount, and
 * deletes the folders its fresh identity names without following links. Anything left is logged as a warning. */
void RemovePendingFixture(FConvaiAvatarPendingDownload& Job);
/** Test tap: the synchronous rescans RefreshMount has run. */
extern int32 RefreshMountScans;
#endif
}
