// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNodeBase.h"
#include "ConvaiDefinitions.h"
#include "AnimNode_ConvaiFaceSync.generated.h"

class UConvaiChatbotComponent;

/** How the node applies curve values to the output pose */
UENUM(BlueprintType)
enum class EConvaiFaceSyncApplyMode : uint8
{
	/** Adds to existing curve values (layers on top of other animations) */
	Add,
	/** Overwrites existing curve values */
	Override
};

/**
 * Custom animation node that reads facial blendshape data from a Convai chatbot component,
 * applies blendshape mapping/remapping, and writes curves with separate upper/lower face alphas.
 */
USTRUCT(BlueprintInternalUseOnly)
struct CONVAI_API FAnimNode_ConvaiFaceSync : public FAnimNode_Base
{
	GENERATED_BODY()

	/** Source pose input */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Links")
	FPoseLink SourcePose;

	/** Optional explicit chatbot component reference. If left unset, the node will
	 *  auto-discover the first UConvaiChatbotComponent on the owning actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai", meta = (PinShownByDefault))
	UConvaiChatbotComponent* ConvaiChatbotComponent = nullptr;

	/** Names of blendshapes that belong to the upper face (brow, eyes, etc.).
	 *  Everything NOT in this list is treated as lower face. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Face Split", meta = (PinHiddenByDefault))
	TArray<FName> UpperFaceBlendshapeNames;

	/** Alpha applied to upper face curves */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Face Split", meta = (PinHiddenByDefault, ClampMin = "0.0", ClampMax = "1.0"))
	float UpperFaceAlpha = 0.8f;

	/** Alpha applied to lower face curves */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Face Split", meta = (PinHiddenByDefault, ClampMin = "0.0", ClampMax = "1.0"))
	float LowerFaceAlpha = 1.0f;

	/** How to apply curve values to the output pose */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Apply")
	EConvaiFaceSyncApplyMode ApplyMode = EConvaiFaceSyncApplyMode::Add;

	// ── Smoothing ──

	/** Enable exponential moving average smoothing for the lower face */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Smoothing")
	bool bEnableLowerFaceSmoothing = false;

	/** Smoothing interp speed for lower face. Higher = faster response, lower = smoother.
	 *  Typical range: 5-15. Uses FMath::FInterpTo (frame-rate independent). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Smoothing", meta = (PinHiddenByDefault, ClampMin = "0.0", EditCondition = "bEnableLowerFaceSmoothing"))
	float LowerFaceSmoothingSpeed = 10.0f;

	/** Enable exponential moving average smoothing for the upper face */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Smoothing")
	bool bEnableUpperFaceSmoothing = false;

	/** Smoothing interp speed for upper face. Higher = faster response, lower = smoother. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Smoothing", meta = (PinHiddenByDefault, ClampMin = "0.0", EditCondition = "bEnableUpperFaceSmoothing"))
	float UpperFaceSmoothingSpeed = 10.0f;

	// ── Mapping ──

	/**
	 * Optional blendshape remapping table.
	 * Key = source blendshape name from Convai.
	 * Value = mapping parameters (target names, multiplier, offset, clamping, etc.)
	 * If empty, blendshapes are applied 1:1 with no remapping.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Mapping", meta = (PinHiddenByDefault))
	TMap<FName, FConvaiBlendshapeParameters> BlendshapeMapping;

	/** Global multiplier applied to all mapped blendshape values (unless IgnoreGlobalModifiers is set) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Mapping", meta = (PinHiddenByDefault))
	float GlobalMultiplier = 1.0f;

	/** Global offset applied to all mapped blendshape values (unless IgnoreGlobalModifiers is set) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Mapping", meta = (PinHiddenByDefault))
	float GlobalOffset = 0.0f;

public:
	FAnimNode_ConvaiFaceSync();

	// FAnimNode_Base interface
	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;

private:
	/** Cached set of upper face names for O(1) lookup */
	TSet<FName> UpperFaceNameSet;

	/** Whether the upper face set needs rebuilding */
	bool bUpperFaceSetDirty = true;

	/** Last known upper face names count — used to detect when the array changes */
	int32 CachedUpperFaceNamesCount = -1;

	/** Previous frame's smoothed values for EMA */
	TMap<FName, float> SmoothedValues;

	/** Cached DeltaTime from the last Update call */
	float CachedDeltaTime = 0.0f;

	/** Resolved component — either from the pin or auto-discovered */
	TWeakObjectPtr<UConvaiChatbotComponent> ResolvedComponent;

	/** Resolves the chatbot component: uses the pin value if set, otherwise auto-discovers from owning actor */
	UConvaiChatbotComponent* ResolveComponent(const FAnimationBaseContext& Context);
};
