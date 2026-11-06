// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <convai/convai_client.h>

struct FConvaiConnectionParams;

/**
 * Everything a connection thread needs from settings, read on the game thread
 * before the thread starts. Blueprint setters, editor sign-in and Project
 * Settings rewrite those settings unlocked, so the thread reads this copy.
 */
struct FConvaiConnectSnapshot
{
	convai::ConvaiAECConfig AEC;
	/** Empty when unset: no hint is sent. */
	FString AECStreamDelayMs;
	FString AuthHeader;
	FString AuthKey;
	FString StreamURL;
	FString PluginVersion;
	FString PlatformName;
	FString EngineVersion;
	FString AppName;
	FString AppClientVersion;
	FString ClientVersionOverride;

	static FConvaiConnectSnapshot Capture();
};

/**
 * The stamp an editor build sends in client_version, PIE_<stamp>. Taken when
 * the first UConvaiSubsystem of a play session initializes, so the clients of
 * a multi-client PIE session share it and the next session gets its own. On a
 * worker thread the engine's PIE id is always -1, so it cannot key the stamp.
 * Game thread only.
 */
namespace ConvaiPlaySessionStamp
{
	void SubsystemInitialized(const UObject* Subsystem);
	void SubsystemDeinitialized(const UObject* Subsystem);
}

/**
 * The library's AEC and connect configuration for one attempt, built from its
 * snapshot and params. The connect configuration points into strings this
 * owns, so it lives until Connect returns and never moves.
 */
struct FConvaiConnectConfiguration
{
	FConvaiConnectConfiguration(const FConvaiConnectSnapshot& Snapshot, const FConvaiConnectionParams& Params);
	UE_NONCOPYABLE(FConvaiConnectConfiguration);

	convai::ConvaiAECConfig AEC;
	TOptional<int32> StreamDelayMs;
	convai::ConvaiConnectionConfig Connection;

	FString InvocationClientVersion;
	FString NativeClientVersion;
	FString ExtraMetadata;

private:
	TArray<TArray<ANSICHAR>> Utf8Strings;
	convai::ConvaiVadParams VadParams;
	convai::ConvaiInvocationMetadata InvocationMetadata;
};
