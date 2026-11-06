// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

struct FConvaiAvatarAsset;

/** Session-only reconciliation for one account. Reset when authentication changes.
 * A confirmed deletion must not reappear from an older list response. This state
 * does not delete, unbind, or otherwise change the avatar's local source files.
 */
class CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarLibraryState
{
public:
    /** Empty Error means the delete request completed successfully. Unknown or failed outcomes are never hidden. */
    bool RecordDeleteResult(const FString& AssetId, const FString& Error);
    void RemoveDeleted(TArray<FConvaiAvatarAsset>& Assets) const;
    bool IsDeleted(const FString& AssetId) const;
    /** Session-only outcomes of our uploads. An interrupted replacement does not invalidate an older cloud file. */
    void BeginArtifactUpload(const FString& AssetId, const FString& Version);
    void CompleteArtifactUpload(const FString& AssetId, const FString& Version);
    TArray<FString> UnconfirmedUploads(const FString& AssetId) const;
    void Reset();

private:
    TSet<FString> DeletedAssetIds;
    TMap<FString, TSet<FString>> PendingUploads;
};
