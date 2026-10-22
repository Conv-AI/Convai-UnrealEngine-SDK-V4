// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

struct FConvaiAvatarAsset;

enum class EConvaiAvatarArtifactHealth : uint8 { Unknown, Ready, NoCompletedWindows, TargetWindowsMissing };

/** Read-only observations about published storage objects, not pak validation or upload history. */
struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarArtifactHealth
{
	EConvaiAvatarArtifactHealth Status = EConvaiAvatarArtifactHealth::Unknown;
	FString Warning;
	FString TargetWindowsVersion;
	FString LatestCompletedWindowsVersion;
	FString SourceVersion, WindowsVersion, LinuxVersion;
	TOptional<int64> SourceBytes, WindowsBytes, LinuxBytes;
};

namespace ConvaiAvatarArtifactHealth
{
	/** Target is the known published engine (major.minor or major.minor.patch), never guessed from the host. */
	CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarArtifactHealth Evaluate(const FConvaiAvatarAsset& Asset, const FString& TargetEngineVersion);
}
