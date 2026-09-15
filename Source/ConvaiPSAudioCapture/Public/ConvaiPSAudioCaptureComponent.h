// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PixelStreamingAudioComponent.h"
#include "Interface/ConvaiAudioCaptureInterface.h"
#include "ConvaiPSAudioCaptureComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiPSAudioCaptureLog, Log, All);

/**
 * Pixel streaming audio capture component for ConvAI
 * Inherits from UPixelStreamingAudioComponent to receive audio from Pixel Streaming
 * Implements IConvaiAudioCaptureInterface to be discoverable by the ConvAI player
 */
UCLASS(ClassGroup = (Convai), meta = (BlueprintSpawnableComponent), DisplayName = "Convai Pixel Streaming Audio Capture")
class CONVAIPSAUDIOCAPTURE_API UConvaiPSAudioCaptureComponent : public UPixelStreamingAudioComponent, public IConvaiAudioCaptureInterface
{
	GENERATED_BODY()

public:
	UConvaiPSAudioCaptureComponent(const FObjectInitializer& ObjectInitializer);

	// IConvaiAudioCaptureInterface implementation
	virtual void Start() override;
	virtual void Stop() override;
	virtual void SetVolumeMultiplier(float VolumeMultiplier) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
};
