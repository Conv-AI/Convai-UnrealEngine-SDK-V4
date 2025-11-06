// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ConvaiAudioProcessingInterface.generated.h"

// Forward declarations
class IConvaiProcessedAudioReceiver;

UINTERFACE()
class CONVAI_API UConvaiAudioProcessingInterface : public UInterface
{
	GENERATED_BODY()
};

/**
 * Interface for audio processing components
 * Similar to IConvaiLipSyncInterface pattern
 */
class CONVAI_API IConvaiAudioProcessingInterface
{
	GENERATED_BODY()

public:
	/**
	 * Process audio data through the audio processing pipeline
	 * @param AudioData - Raw audio data to process
	 * @param NumSamples - Number of samples in the audio data
	 * @param SampleRate - Sample rate of the audio data
	 */
	virtual void ProcessAudioData(const int16* AudioData, int32 NumSamples, int32 SampleRate) = 0;

	/**
	 * Set the receiver for processed audio data
	 * @param Receiver - Component that will receive processed audio data
	 */
	virtual void SetProcessedAudioReceiver(IConvaiProcessedAudioReceiver* Receiver) = 0;

	virtual bool UpdateVAD(bool EnasbleVAD) = 0;
};

UINTERFACE()
class CONVAI_API UConvaiProcessedAudioReceiver : public UInterface
{
	GENERATED_BODY()
};

/**
 * Interface for components that receive processed audio data
 * Components implement this to receive cleaned/processed audio from audio processing components
 */
class CONVAI_API IConvaiProcessedAudioReceiver
{
	GENERATED_BODY()

public:
	/**
	 * Called when processed audio data is ready
	 * @param ProcessedAudioData - The processed/cleaned audio data
	 * @param NumSamples - Number of samples in the processed audio data
	 * @param SampleRate - Sample rate of the processed audio data
	 */
	virtual void OnProcessedAudioDataReceived(const int16* ProcessedAudioData, int32 NumSamples, int32 SampleRate) = 0;
};
