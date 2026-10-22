// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "Workspace/ConvaiAvatarWorkspace.h"

class IAssetRegistry;
class UEnum;

namespace ConvaiAvatarReusableEnums
{
	/** Find unchanged, leaf user-enum copies that can keep their existing UObject identity.
	 * Does not change assets or Niagara registrations. Unproven candidates are not reused.
	 * The caller must recapture fingerprints and repeat this selection after unload callbacks. */
	bool Find(const FConvaiAvatarPreparedAsset& Previous, const FConvaiAvatarPreparedAsset& Current,
		const TMap<FString, FString>& CurrentPreparedFiles, TSet<FName>& OutDestinations, FString& Error);
	/** The saved registry lists no package dependency outside the package itself and /Script/. */
	bool RegistryLeaf(IAssetRegistry& Registry, FName Name);
	/** A clean user enum, alone in its package, whose objects reference nothing outside it and /Script/. */
	bool LoadedLeaf(UEnum* Enum, FName PackageName);
}
