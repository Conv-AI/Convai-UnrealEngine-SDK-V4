// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ConvaiAudioCaptureInterface.generated.h"

// This class does not need to be modified.
UINTERFACE()
class UConvaiAudioCaptureInterface : public UInterface
{
	GENERATED_BODY()
};

/**
 * 
 */
class CONVAI_API IConvaiAudioCaptureInterface
{
	GENERATED_BODY()

public:
	virtual void Start() = 0;
	virtual void Stop() = 0;
	virtual void SetVolumeMultiplier(float VolumeMultiplier) = 0;
};
