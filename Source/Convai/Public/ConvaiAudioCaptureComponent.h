// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AudioCapture.h"
#include "Components/SynthComponent.h"
#include "AudioCaptureCore.h"
#include "AudioCaptureDeviceInterface.h"
#include <atomic>
#include "ConvaiAudioCaptureComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiAudioLog, Log, All);

namespace Audio
{
	/** Class which contains an FAudioCapture object and performs analysis on the audio stream, only outputing audio if it matches a detection criteria. */
	class FConvaiAudioCaptureSynth
	{
	public:
		FConvaiAudioCaptureSynth();
		virtual ~FConvaiAudioCaptureSynth();

		TArray<FCaptureDeviceInfo> GetCaptureDevicesAvailable();

		// Gets the default capture device info
		bool GetDefaultCaptureDeviceInfo(FCaptureDeviceInfo& OutInfo);

		bool GetCaptureDeviceInfo(FCaptureDeviceInfo& OutInfo, int32 DeviceIndex);

		// Opens up a stream to the default capture device
		bool OpenDefaultStream();

		bool OpenStream(int32 DeviceIndex);

		void CloseStream();

		// Starts capturing audio
		bool StartCapturing();

		// Stops capturing audio
		void StopCapturing();

		// Immediately stop capturing audio
		void AbortCapturing();

		// Returned if the capture synth is closed
		bool IsStreamOpen() const;

		// Returns true if the capture synth is capturing audio
		bool IsCapturing() const;

		// Seconds since the device last delivered a buffer (or since StartCapturing). A stream whose
		// device died underneath it stays open and capturing but never calls back again.
		double SecondsSinceLastCapture() const;

		// Retrieves audio data from the capture synth.
		// This returns audio only if there was non-zero audio since this function was last called.
		bool GetAudioData(TArray<float>& OutAudioData);

		// Returns the number of samples enqueued in the capture synth
		int32 GetNumSamplesEnqueued();

		FAudioCapture* GetAudioCapture();

		// Flip the shared destroyed flag so any in-flight or about-to-fire callback
		// short-circuits before touching members of this object. Safe to call from
		// any thread; callable before the synth dtor runs (e.g. from BeginDestroy).
		void MarkBeingDestroyed();

	private:
		/** Handles audio capture callback processing */
		void OnAudioCaptured(const float* AudioData, int32 NumFrames, int32 NumChannels);

		// Number of samples enqueued
		int32 NumSamplesEnqueued;

		// Information about the default capture device we're going to use
		FCaptureDeviceInfo CaptureInfo;

		// Audio capture object dealing with getting audio callbacks
		FAudioCapture AudioCapture;

		// Critical section to prevent reading and writing from the captured buffer at the same time
		FCriticalSection CaptureCriticalSection;

		// Buffer of audio capture data, yet to be copied to the output
		TArray<float> AudioCaptureData;

		// If the object has been initialized
		bool bInitialized;

		// If we're capturing data
		bool bIsCapturing;

		// Written on the capture thread, read on the game thread.
		std::atomic<double> LastCaptureTime;

		// Shared flag to signal the WASAPI callback that this object is being destroyed.
		// Shared via TSharedPtr so the lambda callback can safely check it even after
		// the FConvaiAudioCaptureSynth is freed.
		TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> bIsBeingDestroyed;
	};

};


UCLASS(ClassGroup = Synth, meta = (BlueprintSpawnableComponent))
class UConvaiAudioCaptureComponent : public USynthComponent
{
	GENERATED_BODY()

protected:

	UConvaiAudioCaptureComponent(const FObjectInitializer& ObjectInitializer);

	//~ Begin USynthComponent interface
	virtual bool Init(int32& SampleRate) override;
	virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override;
	virtual void OnBeginGenerate() override;
	virtual void OnEndGenerate() override;
	//~ End USynthComponent interface

	//~ Begin UObject interface
	virtual void BeginDestroy();
	virtual bool IsReadyForFinishDestroy() override;
	virtual void FinishDestroy() override;
	//~ End UObject interface

public:
	/**
	*   Induced latency in audio frames to use to account for jitter between mic capture hardware and audio render hardware.
	 *	Increasing this number will increase latency but reduce potential for underruns.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Latency", meta = (ClampMin = "0", ClampMax = "1024"))
		int32 JitterLatencyFrames;

	bool GetDefaultCaptureDeviceInfo(Audio::FCaptureDeviceInfo& OutInfo);

	bool GetCaptureDeviceInfo(Audio::FCaptureDeviceInfo& OutInfo, int32 DeviceIndex);

	TArray<Audio::FCaptureDeviceInfo> GetCaptureDevicesAvailable();

	int32 GetActiveCaptureDevice(Audio::FCaptureDeviceInfo& OutInfo);

	bool SetCaptureDevice(int32 DeviceIndex);

	bool IsCaptureStreamHealthy() const;

	// Closes the stream and starts again, so Init re-picks the device and opens it for real.
	void RestartCapture();

	Audio::FConvaiAudioCaptureSynth* GetCaptureSynth();

private:
	void CloseCaptureStream();

	int32 SelectedDeviceIndex;

	// The device Init last resolved; the open stream is bound to it.
	Audio::FCaptureDeviceInfo ActiveDeviceInfo;

	Audio::FConvaiAudioCaptureSynth CaptureSynth;
	TArray<float> CaptureAudioData;
	int32 CapturedAudioDataSamples;

	bool bSuccessfullyInitialized;
	bool bIsCapturing;
	FThreadSafeBool bIsStreamOpen;
	int32 CaptureChannels;
	int32 FramesSinceStarting;
	int32 ReadSampleIndex;
	FThreadSafeBool bIsDestroying;
	FThreadSafeBool bIsNotReadyForForFinishDestroy;
};
