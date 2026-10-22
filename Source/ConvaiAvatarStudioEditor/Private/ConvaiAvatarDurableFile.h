// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Misc/FileHelper.h"

/** Replaces a small state document so that a crash, a kill or a full disk leaves either its previous bytes or the new ones. */
namespace ConvaiAvatarDurableFile
{
/** Writes Bytes to a new GUID-named sibling of Path, flushes it to disk, then renames it over Path with a write-through
 * replace. Path is never deleted first, so any failure leaves its previous bytes, and the only file ever deleted is the
 * sibling this call made. Refuses a Path that is a symbolic link, junction or other reparse point. A destination another
 * process holds open is retried for less than half a second. The parent folder must already exist. */
bool Replace(const FString& Path, TConstArrayView<uint8> Bytes);
/** Replace with Text encoded byte for byte as FFileHelper::SaveStringToFile encodes it with Encoding. */
bool Replace(const FString& Path, FStringView Text, FFileHelper::EEncodingOptions Encoding);
/** Deletes the siblings Replace leaves in Directory when its process dies between writing one and renaming it: the files
 * named exactly as Replace names them, <name>.<32 uppercase hex digits>.tmp, and nothing else, nor anything below Directory.
 * The caller vouches that Directory is its own. Returns what stayed. */
TArray<FString> DeleteStraySiblings(const FString& Directory);

#if WITH_DEV_AUTOMATION_TESTS
/** Test tap: while one is alive, every Replace of Path fails at its final rename, after writing its sibling. Process-wide,
 * so only one may be alive at a time. */
class FScopedReplaceFault
{
public:
	explicit FScopedReplaceFault(const FString& Path);
	~FScopedReplaceFault();
	FScopedReplaceFault(const FScopedReplaceFault&) = delete;
	FScopedReplaceFault& operator=(const FScopedReplaceFault&) = delete;
};
#endif
}
