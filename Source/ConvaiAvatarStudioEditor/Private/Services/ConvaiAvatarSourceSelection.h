// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Workspace/ConvaiAvatarWorkspace.h"

/** Shared controller policy for Browse, portraits, and upload preparation. No package is loaded here. */
namespace ConvaiAvatarSourceSelection
{
	/** OriginalEntryPoint is the persisted local-authoring provenance; imports deliberately leave it empty. */
	FSoftObjectPath EditableBlueprint(const FConvaiAvatarPreparedAsset& Asset);
	/** Refuses a missing original rather than substituting an older prepared copy. */
	bool ResolveEditableBlueprint(const FConvaiAvatarPreparedAsset& Asset, FSoftObjectPath& OutBlueprint, FString& OutError);
	/** Workspace preparation decides whether refreshing would conflict with saved prepared-only edits. */
	bool BuildUploadRequest(const FConvaiAvatarPreparedAsset& Asset, bool bReady, FConvaiAvatarPrepareRequest& OutRequest, FString& OutError);
}
