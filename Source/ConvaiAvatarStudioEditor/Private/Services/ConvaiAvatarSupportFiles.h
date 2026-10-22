// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include <atomic>
#endif
struct FConvaiAvatarPendingDownload;

/** Missing-only shared content installation. All mutation entry points run on the game thread. */
namespace ConvaiAvatarSupportFiles
{
	bool Validate(const FConvaiAvatarPendingDownload& Job, FString& Error);
	bool Preflight(const FConvaiAvatarPendingDownload& Job, FString& Error);
	bool Install(FConvaiAvatarPendingDownload& Job, FString& Error);
	bool Rollback(FConvaiAvatarPendingDownload& Job, FString& Error);
	void RefreshRegistry(const FConvaiAvatarPendingDownload& Job);
#if WITH_DEV_AUTOMATION_TESTS
	/** Test tap: the Validate calls made so far. */
	extern std::atomic<int32> ValidateCalls;
#endif
}
