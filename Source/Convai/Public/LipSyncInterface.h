// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ConvaiDefinitions.h"
#include "LipSyncInterface.generated.h"

DECLARE_DELEGATE(FOnFacialDataReadySignature);
DECLARE_DELEGATE_RetVal(double, FGetAudioPlaybackTimeDelegate);

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
	virtual TArray<float> ConvaiGetFacialData() const = 0;
	virtual TArray<FString> ConvaiGetFacialDataNames() const = 0;
	virtual int32 ConvaiGetFacialDataCount() const = 0;
	virtual void ConvaiApplyPrecomputedFacialAnimation(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels, FAnimationSequence FaceSequence) = 0;
	virtual void ConvaiApplyFacialFrame(FAnimationFrame FaceFrame, double Duration) = 0;
	virtual bool RequiresPrecomputedFaceData() const = 0;
	
	/**
	 * @deprecated Use GetLipSyncMode() instead and check for BS_MHA or BS_ARKit modes.
	 */
	UE_DEPRECATED(5.0, "Use GetLipSyncMode() instead and check for BS_MHA or BS_ARKit modes.")
	virtual bool GeneratesFacialDataAsBlendshapes() const = 0;
	
	virtual EC_LipSyncMode GetLipSyncMode() const { return EC_LipSyncMode::Off; }
	virtual TMap<FName, float> ConvaiGetFaceBlendshapes() const = 0;
	virtual void ForceRecalculateStartTime() = 0;

	/**
	 * Sets a delegate that provides the current audio playback time.
	 * This allows lipsync to be synchronized with actual audio playback position
	 * rather than wall-clock time, making it robust to app freezes or stutters.
	 * @param InDelegate A delegate that returns the current audio playback time in seconds
	 */
	virtual void SetAudioPlaybackTimeProvider(FGetAudioPlaybackTimeDelegate InDelegate) { AudioPlaybackTimeProvider = InDelegate; }

protected:
	/** Delegate to get audio playback time from audio streamer */
	FGetAudioPlaybackTimeDelegate AudioPlaybackTimeProvider;
};