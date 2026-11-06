// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Workspace/ConvaiAvatarWorkspace.h"
#include <atomic>

/** Shared runtime assets may import /Game packages. Preserve only their reachable runtime support,
 * at its original mount, as cooker context; it never belongs in the selected avatar pak. */
namespace ConvaiAvatarBaseContent
{
bool Gather(const FConvaiAvatarPreparedAsset& Asset, TMap<FString, FString>& OutFiles,
    TArray<FString>& OutPluginDependencies, const std::atomic<bool>& Cancelled, FString& Error);
bool Snapshot(const FString& HostContent, const FString& ProxyDirectory, const FString& SelectedPlugin,
    const TMap<FString, FString>& Files, const std::atomic<bool>& Cancelled, FString& Error);
}
