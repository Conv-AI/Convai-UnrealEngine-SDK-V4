// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include <atomic>

namespace ConvaiAvatarDependencies
{
    /** Read-only preflight for the shipped files needed by upload and download operations.
     *  Call before preparing avatar packages so a partial installation fails early. */
    bool ValidateInstallation(const FString& PluginDirectory, FString& Error);

    /** Resolve once per operation from the V0 remote configuration. The result fixes the
     *  downloaded archive/version used by all transfers belonging to that operation.
     *  Caller holds the shared uploader lease. Does not build or replace host plugins.
     *  PluginDirectory is the Convai plugin's base directory, read on the game thread when the job
     *  starts: these steps run on workers, and the plugin manager is not safe to read there. */
    bool Resolve(const FString& PluginDirectory, const FString& ResultFile, const std::atomic<bool>& Cancelled, FString& Error);

    /** Prepare the resolved HTTP editor modules before cooking or a transfer. Caller retains the
     *  uploader lease; host SDK files are never built or replaced by this step. PluginDirectory as for Resolve. */
    bool Prepare(const FString& PluginDirectory, const FString& ResolutionFile, const std::atomic<bool>& Cancelled, FString& Error);
}
