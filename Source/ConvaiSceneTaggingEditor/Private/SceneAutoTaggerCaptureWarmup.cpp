// Copyright Convai. All Rights Reserved.
#include "SceneAutoTaggerCaptureWarmup.h"
#include "SceneAutoTaggerCapture.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StreamableRenderAsset.h"
#include "Engine/Texture2D.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "Misc/EngineVersionComparison.h"
#include "UnrealEngine.h"

namespace ConvaiSceneAutoTagger::CaptureWarmup
{
bool CanUseWarmProbeGate(
	const FCaptureStreamingReadiness& Readiness,
	const bool bKnownConventionalResources,
	const bool bRepeatedPrimedView,
	const bool bNearbyInitialGate)
{
	return (bRepeatedPrimedView || bNearbyInitialGate)
		&& bKnownConventionalResources
		&& Readiness.TrackedAssets > 0
		&& Readiness.TrackedMaterials > 0
		&& Readiness.UnknownResources == 0
		&& Readiness.VirtualAssets == 0
		&& Readiness.PendingAssets == 0
		&& Readiness.CompilingMaterials == 0
		&& Readiness.FullyResidentAssets == Readiness.ConventionalAssets;
}

bool ShouldContinueProbeGate(
	const int32 DistinctFrames,
	const double ElapsedSeconds,
	const int32 PendingRenderWork,
	const bool bUseWarmGate)
{
	// Only the redundant wall-clock floor is waived. Keep the same three real
	// frames after an exact-view prime; final capture/profile settling is separate.
	return bUseWarmGate && PendingRenderWork == 0
		? DistinctFrames < 3
		: ConvaiSceneAutoTagger::ShouldContinueAutomaticProbeWarmup(
			DistinctFrames, ElapsedSeconds, PendingRenderWork);
}

bool IsRecentNearbyCapture(
	const double LastCaptureSeconds,
	const double NowSeconds,
	const FVector& LastCenter,
	const FVector& CurrentCenter)
{
	const double Age = NowSeconds - LastCaptureSeconds;
	return LastCaptureSeconds >= 0.0 && Age >= 0.0 && Age <= 3.0
		&& !LastCenter.ContainsNaN() && !CurrentCenter.ContainsNaN()
		&& FVector::DistSquared(LastCenter, CurrentCenter) <= FMath::Square(1000.0);
}

bool HasKnownConventionalCaptureResources(
	const TArray<UPrimitiveComponent*>& Components,
	const TArray<FCaptureStreamingLease>& Assets,
	const TArray<TWeakObjectPtr<UMaterialInterface>>& Materials)
{
	// Recheck the current components, not a cached "ready" bit. Material changes,
	// virtual resources, unsupported components and incomplete shader maps all
	// retain the conservative gate. This deliberately excludes skinned meshes.
	if (Components.IsEmpty())
	{
		return false;
	}
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	check(GetCachedScalabilityCVars().bInitialized);
	const auto MaterialQuality = GetCachedScalabilityCVars().MaterialQualityLevel;
#else
	const auto MaterialQuality = GetCurrentMaterialQualityLevelChecked();
#endif
	const auto IsTrackedAsset = [&Assets](const UStreamableRenderAsset* Asset)
	{
		return Assets.ContainsByPredicate([Asset](const FCaptureStreamingLease& Lease)
		{
			return Lease.Asset.Get() == Asset;
		});
	};
	for (UPrimitiveComponent* Component : Components)
	{
		UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component);
		UStaticMesh* Mesh = MeshComponent ? MeshComponent->GetStaticMesh() : nullptr;
		if (!IsValid(MeshComponent) || !MeshComponent->IsRegistered() || !IsValid(Mesh)
			|| Mesh->IsCompiling() || Mesh->IsNaniteEnabled() || Mesh->HasValidNaniteData()
			|| !IsTrackedAsset(Mesh) || !MeshComponent->GetRuntimeVirtualTextures().IsEmpty()
			|| MeshComponent->GetOverlayMaterial() || MeshComponent->GetNumMaterials() == 0)
		{
			return false;
		}
		for (int32 MaterialIndex = 0; MaterialIndex < MeshComponent->GetNumMaterials(); ++MaterialIndex)
		{
			UMaterialInterface* Material = MeshComponent->GetMaterial(MaterialIndex);
			if (!IsValid(Material) || Material->IsCompiling()
				|| !Materials.Contains(TWeakObjectPtr<UMaterialInterface>(Material))
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
				// Earlier engines only expose the component-wide overlay checked above.
				|| MeshComponent->GetOverlayMaterial(true, MaterialIndex)
#endif
				)
			{
				return false;
			}
            const FMaterialResource* Resource = Material->GetMaterialResource(
#if UE_VERSION_OLDER_THAN(5, 7, 0)
                GMaxRHIFeatureLevel, MaterialQuality);
#else
                GMaxRHIShaderPlatform, MaterialQuality);
#endif
			if (!Resource || !Resource->HasValidGameThreadShaderMap()
				|| !Resource->IsGameThreadShaderMapComplete()
				|| Resource->GetNumVirtualTextureStacks() != 0
				|| Resource->HasRuntimeVirtualTextureOutput()
				|| !Resource->GetUniformSparseVolumeTextureExpressions().IsEmpty()
				|| !Resource->GetUniformTextureCollectionExpressions().IsEmpty()
				|| Resource->GetGameThreadShaderMap()->GetUniformExpressionSet().HasExternalTextureExpressions())
			{
				return false;
			}
		}
		TArray<UTexture*> UsedTextures;
		MeshComponent->GetUsedTextures(UsedTextures, MaterialQuality);
		for (UTexture* Texture : UsedTextures)
		{
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (!IsValid(Texture2D) || Texture2D->IsCompiling() || !Texture2D->GetResource()
				|| Texture2D->VirtualTextureStreaming || Texture2D->IsCurrentlyVirtualTextured()
				|| !IsTrackedAsset(Texture2D))
			{
				return false;
			}
		}
	}
	return true;
}

}
