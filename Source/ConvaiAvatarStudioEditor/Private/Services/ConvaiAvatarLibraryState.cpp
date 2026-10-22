// Copyright Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarLibraryState.h"
#include "Services/ConvaiAvatarAssetsClient.h"

bool FConvaiAvatarLibraryState::RecordDeleteResult(const FString& AssetId, const FString& Error)
{
    if (!Error.IsEmpty() || AssetId.TrimStartAndEnd().IsEmpty()) return false;
    DeletedAssetIds.Add(AssetId);
    PendingUploads.Remove(AssetId);
    return true;
}

void FConvaiAvatarLibraryState::RemoveDeleted(TArray<FConvaiAvatarAsset>& Assets) const
{
    Assets.RemoveAll([this](const FConvaiAvatarAsset& Asset) { return DeletedAssetIds.Contains(Asset.AssetId); });
}

bool FConvaiAvatarLibraryState::IsDeleted(const FString& AssetId) const
{
    return DeletedAssetIds.Contains(AssetId);
}

void FConvaiAvatarLibraryState::BeginArtifactUpload(const FString& AssetId, const FString& Version)
{
    if (!AssetId.IsEmpty() && !Version.IsEmpty()) PendingUploads.FindOrAdd(AssetId).Add(Version);
}

void FConvaiAvatarLibraryState::CompleteArtifactUpload(const FString& AssetId, const FString& Version)
{
    if (auto* Versions = PendingUploads.Find(AssetId))
    {
        Versions->Remove(Version);
        if (Versions->IsEmpty()) PendingUploads.Remove(AssetId);
    }
}

TArray<FString> FConvaiAvatarLibraryState::UnconfirmedUploads(const FString& AssetId) const
{
    TArray<FString> Result;
    if (const auto* Versions = PendingUploads.Find(AssetId)) Result = Versions->Array();
    Result.Sort();
    return Result;
}

void FConvaiAvatarLibraryState::Reset()
{
    DeletedAssetIds.Reset();
    PendingUploads.Reset();
}
