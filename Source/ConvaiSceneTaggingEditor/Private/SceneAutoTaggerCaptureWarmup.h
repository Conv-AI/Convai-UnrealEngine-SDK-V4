// Copyright Convai. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UPrimitiveComponent;
class UStreamableRenderAsset;

namespace ConvaiSceneAutoTagger::CaptureWarmup
{
struct FCaptureStreamingReadiness
{
	int32 TrackedAssets = 0;
	int32 ConventionalAssets = 0;
	int32 FullyResidentAssets = 0;
	int32 PendingAssets = 0;
	int32 VirtualAssets = 0;
	int32 TrackedMaterials = 0;
	int32 CompilingMaterials = 0;
	int32 UnknownResources = 0;
};

struct FCaptureStreamingLease
{
	TWeakObjectPtr<UStreamableRenderAsset> Asset;
	bool bWasForcedResident = false;
	bool bOriginalIgnoreStreamingMipBias = false;
};

bool CanUseWarmProbeGate(const FCaptureStreamingReadiness& Readiness,
    bool bKnownConventionalResources, bool bRepeatedPrimedView, bool bNearbyInitialGate);
bool ShouldContinueProbeGate(int32 DistinctFrames, double ElapsedSeconds,
    int32 PendingRenderWork, bool bUseWarmGate);
bool IsRecentNearbyCapture(double LastCaptureSeconds, double NowSeconds,
    const FVector& LastCenter, const FVector& CurrentCenter);
bool HasKnownConventionalCaptureResources(const TArray<UPrimitiveComponent*>& Components,
    const TArray<FCaptureStreamingLease>& Assets,
    const TArray<TWeakObjectPtr<UMaterialInterface>>& Materials);
}
