// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "Workspace/ConvaiAvatarWorkspace.h"

namespace ConvaiAvatarSourceRevision
{
	bool CaptureSource(const TMap<FName, FName>& Packages, TMap<FString, FString>& Files,
		TMap<FName, FString>& LegacyHashes, const TFunction<bool()>& Cancelled, FString& Error);
	bool CapturePrepared(const FString& PluginDirectory, TMap<FString, FString>& Files,
		const TFunction<bool()>& Cancelled, FString& Error);
	void Compare(const FConvaiAvatarPreparedAsset& Previous, const FConvaiAvatarPreparedAsset& Current,
		const TMap<FString, FString>& PreparedFiles, FConvaiAvatarSourceReview& Review);
}
