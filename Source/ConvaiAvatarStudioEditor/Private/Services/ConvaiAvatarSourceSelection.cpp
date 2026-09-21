// Copyright Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarSourceSelection.h"
#include "Misc/PackageName.h"

FSoftObjectPath ConvaiAvatarSourceSelection::EditableBlueprint(const FConvaiAvatarPreparedAsset& Asset)
{
	return Asset.OriginalEntryPoint.IsNull() ? Asset.EntryPoint : Asset.OriginalEntryPoint;
}

bool ConvaiAvatarSourceSelection::ResolveEditableBlueprint(const FConvaiAvatarPreparedAsset& Asset, FSoftObjectPath& OutBlueprint, FString& OutError)
{
	OutBlueprint.Reset();
	OutError.Reset();
	const FSoftObjectPath Selected = EditableBlueprint(Asset);
	if (!Selected.IsValid() || !FPackageName::DoesPackageExist(Selected.GetLongPackageName()))
	{
		OutError = Asset.OriginalEntryPoint.IsNull()
			? FString::Printf(TEXT("The downloaded avatar Blueprint could not be found at %s. Download its source again before continuing."), *Selected.ToString())
			: FString::Printf(TEXT("The original avatar Blueprint could not be found at %s. Restore it at that path, save your changes, and retry."), *Selected.ToString());
		return false;
	}
	OutBlueprint = Selected;
	return true;
}

bool ConvaiAvatarSourceSelection::BuildUploadRequest(const FConvaiAvatarPreparedAsset& Asset, bool bReady,
	FConvaiAvatarPrepareRequest& OutRequest, FString& OutError)
{
	OutRequest = {};
	OutError.Reset();
	const bool bResumeInclusion = Asset.bConvaiContentPending && Asset.bIncludeConvaiContent;
	if (!bReady && Asset.OriginalEntryPoint.IsNull() && !bResumeInclusion)
	{
		OutError = TEXT("This avatar's download is incomplete. Finish downloading its source before uploading changes.");
		return false;
	}
	FSoftObjectPath Selected;
	if (bResumeInclusion)
	{
		Selected = Asset.EntryPoint;
		if (!Selected.IsValid() || !FPackageName::DoesPackageExist(Selected.GetLongPackageName()))
		{
			OutError = TEXT("The prepared avatar Blueprint needed to finish its Convai content update is missing: ") + Selected.ToString() + TEXT(". Restore that file before retrying.");
			return false;
		}
	}
	else if (!ResolveEditableBlueprint(Asset, Selected, OutError)) return false;
	OutRequest.AssetId = Asset.AssetId;
	OutRequest.DisplayName = Asset.DisplayName;
	OutRequest.Blueprint = Selected;
	OutRequest.bRefreshFromSource = !bResumeInclusion && !Asset.OriginalEntryPoint.IsNull();
	OutRequest.bIsMetaHuman = Asset.bIsMetaHuman;
	OutRequest.bIncludeConvaiContent = Asset.bIncludeConvaiContent;
	OutRequest.AcknowledgedMissingPackages = Asset.AcknowledgedMissingPackages;
	return true;
}
