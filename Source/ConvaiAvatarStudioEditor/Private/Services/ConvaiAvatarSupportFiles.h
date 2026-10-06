// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
struct FConvaiAvatarPendingDownload;

/** Missing-only shared content installation. All mutation entry points run on the game thread. */
namespace ConvaiAvatarSupportFiles
{
	bool Validate(const FConvaiAvatarPendingDownload& Job, FString& Error);
	bool Preflight(const FConvaiAvatarPendingDownload& Job, FString& Error);
	bool Install(FConvaiAvatarPendingDownload& Job, FString& Error);
	bool Rollback(FConvaiAvatarPendingDownload& Job, FString& Error);
	void RefreshRegistry(const FConvaiAvatarPendingDownload& Job);
}
