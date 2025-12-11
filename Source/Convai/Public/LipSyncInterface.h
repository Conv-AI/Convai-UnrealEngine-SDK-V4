// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ConvaiDefinitions.h"
#include "LipSyncInterface.generated.h"

DECLARE_DELEGATE(FOnFacialDataReadySignature);

UINTERFACE()
class CONVAI_API UConvaiLipSyncInterface : public UInterface
{
	GENERATED_BODY()
};

class IConvaiLipSyncInterface
{
	GENERATED_BODY()

public:
	FOnFacialDataReadySignature OnFacialDataReady;

	virtual void ConvaiInferFacialDataFromAudio(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels) = 0;
	virtual void MarkEndOfAudio() {};
	virtual void ConvaiStopLipSync() = 0;
	virtual void ConvaiPauseLipSync() = 0;
	virtual void ConvaiResumeLipSync() = 0;
	virtual TArray<float> ConvaiGetFacialData() = 0;
	virtual TArray<FString> ConvaiGetFacialDataNames() = 0;
	virtual void ConvaiApplyPrecomputedFacialAnimation(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels, FAnimationSequence FaceSequence) = 0;
	virtual void ConvaiApplyFacialFrame(FAnimationFrame FaceFrame, float Duration) = 0;
	virtual bool RequiresPrecomputedFaceData() = 0;
	virtual bool GeneratesFacialDataAsBlendshapes() = 0;
	virtual TMap<FName, float> ConvaiGetFaceBlendshapes() = 0;
	virtual void ForceRecalculateStartTime() = 0;
};