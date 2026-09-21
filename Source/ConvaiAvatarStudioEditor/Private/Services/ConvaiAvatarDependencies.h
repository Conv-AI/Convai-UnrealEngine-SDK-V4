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
     *  Caller holds the shared uploader lease. Does not build or replace host plugins. */
    bool Resolve(const FString& ResultFile, const std::atomic<bool>& Cancelled, FString& Error);

    /** Prepare the resolved HTTP editor modules before cooking. Caller retains the
     *  uploader lease; host SDK files are never built or replaced by this step. */
    bool Prepare(const FString& ResolutionFile, const std::atomic<bool>& Cancelled, FString& Error);
}
