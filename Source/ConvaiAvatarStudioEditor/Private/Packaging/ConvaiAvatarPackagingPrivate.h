// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Misc/SecureHash.h"
#include <atomic>

struct FConvaiAvatarPreparedAsset;

namespace ConvaiAvatarPackagingPrivate
{
/** What a delete removed. */
struct FDeletedFiles
{
    int32 Files = 0;
    int64 Bytes = 0;
};
/** Deletes Directory, which must lie below Owner, and returns what stayed. An empty path, a folder outside Owner or a link
 * anywhere from Owner down to Directory refuses the whole delete. A link inside is kept and never entered. Files go first,
 * then folders bottom-up, each on its own, so a file in use stays with its folders. It never fails its caller. Once
 * Cancelled is set it stops between entries, and what it had not reached stays unreported. Deleted, when given, gains
 * each file it removed. */
TArray<FString> DeleteOwnedTree(const FString& Directory, const FString& Owner, const std::atomic<bool>* Cancelled = nullptr,
    FDeletedFiles* Deleted = nullptr);
/** One Warning naming what a delete left behind, if anything. */
void WarnScratchKept(const TArray<FString>& Left);
/** Run under the uploader lease. Deletes the earlier runs in Artifacts, the folders named with 32 hex digits, each with its
 * Resolutions/<run>.json and <run>.json.prepare.json. Keeps CurrentRun and the newest other run by its packaging.log time
 * (its folder's time without a log): the lease is released before archiving and uploading, so a second editor may still
 * upload that run, and Open log may still point at its log. Stops like DeleteOwnedTree once Cancelled is set. Logs how many
 * files and bytes it deleted, which Deleted also receives. Returns what stayed. */
TArray<FString> PruneArtifactRuns(const FString& Artifacts, const FString& Resolutions, const FString& CurrentRun,
    const std::atomic<bool>* Cancelled = nullptr, FDeletedFiles* Deleted = nullptr);
/** A platform's Cooked folder, deleted once its pak is verified or its step failed: nothing reads it after UnrealPak -Test.
 * Its cook.log, pak-files.txt and pak stay. A cancelled run leaves it to a later prune. Returns what stayed. */
TArray<FString> DeleteCookedTree(const FString& PlatformOutput, const std::atomic<bool>* Cancelled = nullptr);
/** FConvaiAvatarPackaging::CalculateInputFingerprint that stops once Cancelled is set: both file walks watch it, and the
 * hash loop checks it before each file. nullptr never cancels. OutStamps, when given, receives each file's FileStamp keyed
 * by its path relative to the plugin, for CheckInputStamps. */
bool CalculateInputFingerprint(const FConvaiAvatarPreparedAsset& Asset, FString& OutFingerprint, FString& OutError, const std::atomic<bool>* Cancelled,
    TMap<FString, FString>* OutStamps = nullptr);
/** The recheck between platforms: walks the canonical avatar again and fails at the first file added, removed, or whose
 * stamp differs from Stamps. It reads no file's bytes, so an edit that keeps both size and time passes it; the full checks
 * before the cook loop and at the end of preparation still hash every byte. */
bool CheckInputStamps(const FConvaiAvatarPreparedAsset& Asset, const TMap<FString, FString>& Stamps, FString& OutError, const std::atomic<bool>* Cancelled);
/** FConvaiAvatarPackaging::ValidatePackagedInputs over the cancellable fingerprint. */
bool ValidatePackagedInputs(const FConvaiAvatarPreparedAsset& Asset, const FString& ExpectedFingerprint, FString& OutError, const std::atomic<bool>* Cancelled);
/** The MD5 of a file's bytes. The avatar fingerprint and the two mirrors read files only through this. */
FMD5Hash Md5OfFile(const FString& File, TArray<uint8>* Buffer = nullptr);
/** True for exactly 32 hex digits. */
bool IsMd5Hex(const FString& Value);
/** Path as IPlatformFile hands it to Windows: absolute, backslashes, and \\?\ (\\?\UNC\ for a share) from MAX_PATH characters
 * on. Without that prefix a Win32 call on a longer path fails unless the system enables long paths, and a link check would read
 * the failure as "not a link" while the engine's own calls still reach the path. */
FString NativePath(const FString& Path);
/** True when Path is a symbolic link, junction or other reparse point. Every link check in the module reads attributes
 * through this, on NativePath, so a link whose own path is MAX_PATH characters or longer is seen without system long-path
 * support too. */
bool IsReparsePoint(const FString& Path);
/** "<size>:<last write time>" of a file at the file system's own precision, empty when it cannot be read. A mirror reuses a
 * recorded hash only while the stamps of both the source and its copy still equal the recorded ones. */
FString FileStamp(const FString& File);
/** The output callback FConvaiAvatarPackaging::Run gives a cook or UnrealPak run: appends what the child writes to Log,
 * byte for byte, through one writer that closes when the last copy of the callback goes. */
TFunction<void(const TArray<uint8>&)> MakeLogWriter(const FString& Log);
/** Mirrors a plugin's deployment folders into Dest, which must be inside the uploader cache. Each file's record in
 * Dest/.convai-plugin-snapshot.json holds its MD5 with the stamps of the source and the copy; while both still match, the
 * file is neither read nor copied. A bare MD5 string, as older builds wrote, is a record without stamps. */
bool CopyRuntimePlugin(const FString& Source, const FString& Dest, const std::atomic<bool>& Cancelled, FString& Error);
#if WITH_DEV_AUTOMATION_TESTS
/** Test taps: the files Md5OfFile has read, and a hook the fingerprint runs after each file it hashes. */
extern std::atomic<int32> HashedFiles;
extern TFunction<void()> AfterFingerprintFile;
#endif
}
