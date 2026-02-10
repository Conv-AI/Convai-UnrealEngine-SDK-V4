// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "LipSyncInterface.h"

#include "Components/SceneComponent.h"
#include "Containers/Map.h"
#include "ConvaiDefinitions.h"
#include "ConvaiUtils.h"
#include "ConvaiFaceSync.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiFaceSyncLog, Log, All);

UCLASS(meta = (BlueprintSpawnableComponent), DisplayName = "Convai Face Sync")
class CONVAI_API UConvaiFaceSyncComponent : public USceneComponent, public IConvaiLipSyncInterface
{
	GENERATED_BODY()

public:
	//~ Construction & Destruction
	UConvaiFaceSyncComponent();
	virtual ~UConvaiFaceSyncComponent() override {}

	//~ UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	//~ IConvaiLipSyncInterface
	virtual void ConvaiInferFacialDataFromAudio(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels) override { return; }
	virtual void ConvaiStopLipSync() override;
	virtual void ConvaiPauseLipSync() override;
	virtual void ConvaiResumeLipSync() override;
	virtual TArray<float> ConvaiGetFacialData() const override;
	virtual TArray<FString> ConvaiGetFacialDataNames() const override;
	virtual int32 ConvaiGetFacialDataCount() const override;
	virtual EC_LipSyncMode GetLipSyncMode() const override;
	virtual bool GeneratesFacialDataAsBlendshapes() const override;
	virtual void ConvaiApplyPrecomputedFacialAnimation(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels, FAnimationSequence FaceSequence) override;
	virtual void ConvaiApplyFacialFrame(FAnimationFrame FaceFrame, double Duration) override;
	virtual bool RequiresPrecomputedFaceData() const override { return true; }
	virtual TMap<FName, float> ConvaiGetFaceBlendshapes() const override { return CurrentBlendShapesMap; }
	virtual void ForceRecalculateStartTime() override;

	//~ Post-Processing Hooks
	virtual void Apply_StartEndFrames_PostProcessing(const int& CurrentFrameIndex, const int& NextFrameIndex, float& Alpha, TMap<FName, float>& StartFrame, TMap<FName, float>& EndFrame) {}
	virtual void ApplyPostProcessing() {}

	//~ Recording
	void StartRecordingLipSync();
	FAnimationSequenceBP FinishRecordingLipSync();
	bool PlayRecordedLipSync(FAnimationSequenceBP RecordedLipSync, int StartFrame, int EndFrame, float OverwriteDuration);

	//~ Playback Control
	virtual void CalculateStartingTime();
	void ClearMainSequence();
	void ResetTimeState();
	bool IsPlaying() const;

	//~ Frame Utilities
	TMap<FName, float> InterpolateFrames(const TMap<FName, float>& StartFrame, const TMap<FName, float>& EndFrame, float Alpha);
	virtual TMap<FName, float> GenerateZeroFrame() { return GetZeroFrameForMode(GetLipSyncMode()); }
	virtual void SetCurrentFrameToZero() { CurrentBlendShapesMap = GetZeroFrameForMode(GetLipSyncMode()); }
	TMap<FName, float> GetCurrentFrame() { return CurrentBlendShapesMap; }
	bool CalculateFrameSelection(FFrameSelectionResult& OutResult);

	//~ Static Utilities
	static bool IsValidSequence(const FAnimationSequence& Sequence);
	static const TMap<FName, float>& GetZeroFrameForMode(EC_LipSyncMode Mode);

	//~ Static Data
	const static TMap<FName, float> ZeroBlendShapeFrame;
	const static TMap<FName, float> ZeroVisemeFrame;
	const static TMap<FName, float> ZeroArKitFrame;

	//~ Public Properties
	float AnchorValue = 0.5;

	UPROPERTY(EditAnywhere, Category = "Convai|LipSync")
	bool bEnableInterpolation = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|LipSync")
	EC_LipSyncMode LipSyncMode = EC_LipSyncMode::BS_MHA;

	//~ Deprecated
	/** @deprecated Use LipSyncMode instead. */
	UE_DEPRECATED(5.0, "Use LipSyncMode instead.")
	UPROPERTY(meta=(DeprecatedProperty, DeprecationMessage="Use LipSyncMode instead."))
	bool ToggleBlendshapeOrViseme = true;

protected:
	//~ Recording Helpers
	void AddFrameToRecordingBuffer(const FAnimationFrame& Frame, double Duration);
	void AddSequenceToRecordingBuffer(const FAnimationSequence& Sequence);

	//~ State
	float CurrentSequenceTimePassed;
	TMap<FName, float> CurrentBlendShapesMap;
	FAnimationSequence MainSequenceBuffer;
	FAnimationSequence RecordedSequenceBuffer;
	double StartTime;
	double PauseStartTime;
	double TotalPausedDuration;

	//~ Flags
	bool bStopping;
	bool bIsRecordingLipSync;
	bool bIsPlaying;
	bool bIsPaused;

	//~ Thread Synchronization
	FCriticalSection SequenceCriticalSection;
	FCriticalSection RecordingCriticalSection;
};
