// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "LipSyncInterface.h"

#include "Components/SceneComponent.h"
#include "Containers/Map.h"
#include "ConvaiDefinitions.h"
#include "ConvaiFaceSync.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiFaceSyncLog, Log, All);

UCLASS(meta = (BlueprintSpawnableComponent), DisplayName = "Convai Face Sync")
class CONVAI_API UConvaiFaceSyncComponent : public USceneComponent, public IConvaiLipSyncInterface
{
	GENERATED_BODY()
public:
	UConvaiFaceSyncComponent();

	virtual ~UConvaiFaceSyncComponent();

	// UActorComponent interface
	virtual void BeginPlay() override;
	// virtual void OnRegister() override;
	// virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
	// End UActorComponent interface

	// IConvaiLipSyncInterface
	virtual void ConvaiInferFacialDataFromAudio(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels) override { return; }
	virtual void ConvaiStopLipSync() override;
	virtual void ConvaiPauseLipSync() override;
	virtual void ConvaiResumeLipSync() override;
	virtual TArray<float> ConvaiGetFacialData() override
	{
		TArray<float> FacialDataValues;
		CurrentBlendShapesMap.GenerateValueArray(FacialDataValues);
		return FacialDataValues;
	}
	virtual TArray<FString> ConvaiGetFacialDataNames() override { return ConvaiConstants::VisemeNames; }
	// End IConvaiLipSyncInterface interface

	// IConvaiLipSyncExtendedInterface
	virtual void ConvaiApplyPrecomputedFacialAnimation(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels, FAnimationSequence FaceSequence) override;
	virtual void ConvaiApplyFacialFrame(FAnimationFrame FaceFrame, float Duration) override;
	virtual bool RequiresPrecomputedFaceData() override { return true; }
	virtual bool GeneratesFacialDataAsBlendshapes() override { return ToggleBlendshapeOrViseme; }
	virtual TMap<FName, float> ConvaiGetFaceBlendshapes() override { return CurrentBlendShapesMap; }
	// End IConvaiLipSyncExtendedInterface interface

	virtual void Apply_StartEndFrames_PostProcessing(const int& CurrentFrameIndex, const int& NextFrameIndex, float& Alpha, TMap<FName, float>& StartFrame, TMap<FName, float>& EndFrame){}
	virtual void ApplyPostProcessing() {}

	// UFUNCTION(BlueprintCallable, Category = "Convai|LipSync")
	void StartRecordingLipSync();

	// UFUNCTION(BlueprintCallable, Category = "Convai|LipSync")
	FAnimationSequenceBP FinishRecordingLipSync();

	// UFUNCTION(BlueprintCallable, Category = "Convai|LipSync")
	bool PlayRecordedLipSync(FAnimationSequenceBP RecordedLipSync, int StartFrame, int EndFrame, float OverwriteDuration);

	bool IsValidSequence(const FAnimationSequence &Sequence);

	bool IsPlaying();

	// Record the current time if this is the first LipSync sequence to be received after silence
	virtual void CalculateStartingTime();

	virtual void ForceRecalculateStartTime() override;

	void ClearMainSequence();

	TMap<FName, float> InterpolateFrames(const TMap<FName, float>& StartFrame, const TMap<FName, float>& EndFrame, float Alpha);

	// Virtual function to get the curve names for interpolation - can be overridden by derived classes
	virtual const TArray<FString>& GetCurveNames()
	{
		return GeneratesFacialDataAsBlendshapes() ? ConvaiConstants::BlendShapesNames : ConvaiConstants::VisemeNames;
	}

	virtual TMap<FName, float> GenerateZeroFrame() { return GeneratesFacialDataAsBlendshapes() ? ZeroBlendshapeFrame : ZeroVisemeFrame; }

	virtual void SetCurrentFrametoZero()
	{
		if (GeneratesFacialDataAsBlendshapes())
			CurrentBlendShapesMap = ZeroBlendshapeFrame;
		else
			CurrentBlendShapesMap = ZeroVisemeFrame;
	}

	TMap<FName, float> GetCurrentFrame() { return CurrentBlendShapesMap; }

	const static TMap<FName, float> ZeroBlendshapeFrame;
	const static TMap<FName, float> ZeroVisemeFrame;

	//UPROPERTY(EditAnywhere, Category = "Convai|LipSync")
	float AnchorValue = 0.5;

	// If true, interpolation between frames is enabled. If false, uses end frame directly for better performance.
	UPROPERTY(EditAnywhere, Category = "Convai|LipSync")
	bool bEnableInterpolation = true;

	UPROPERTY(EditAnywhere, Category = "Convai|LipSync")
	bool ToggleBlendshapeOrViseme = false;

protected:
	float CurrentSequenceTimePassed;
	TMap<FName, float> CurrentBlendShapesMap;
	FAnimationSequence MainSequenceBuffer;
	FAnimationSequence RecordedSequenceBuffer;
	FCriticalSection SequenceCriticalSection;
	FCriticalSection RecordingCriticalSection;
	bool Stopping;
	bool IsRecordingLipSync;
	double StartTime;
	bool bIsPlaying;
	bool bIsPaused;
	double PauseStartTime;
	double TotalPausedDuration;
};