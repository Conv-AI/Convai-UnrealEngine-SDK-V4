// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiAudioCaptureComponent.h"
#include "ConvaiDefinitions.h"
#include "Utility/Log/ConvaiLogger.h"

DEFINE_LOG_CATEGORY(ConvaiAudioLog);


namespace Audio
{
	FConvaiAudioCaptureSynth::FConvaiAudioCaptureSynth()
		: NumSamplesEnqueued(0)
		, bInitialized(false)
		, bIsCapturing(false)
		, bIsBeingDestroyed(MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(false))
	{
	}

	FConvaiAudioCaptureSynth::~FConvaiAudioCaptureSynth()
	{
		// Signal the callback lambda that this object is being destroyed.
		// The lambda holds a copy of this TSharedPtr, so it can safely check
		// the flag even after our members are freed.
		*bIsBeingDestroyed = true;

		{
			FScopeLock Lock(&CaptureCriticalSection);
			bIsCapturing = false;
		}

		// AbortStream was already called in BeginDestroy to signal the WASAPI
		// thread to stop. By now the thread has had time to exit. CloseStream
		// releases the COM resources safely.
		if (AudioCapture.IsStreamOpen())
		{
			AudioCapture.CloseStream();
		}
	}

	TArray<FCaptureDeviceInfo> FConvaiAudioCaptureSynth::GetCaptureDevicesAvailable()
	{
		TArray<FCaptureDeviceInfo> CaptureDevicesInfo;
		AudioCapture.GetCaptureDevicesAvailable(CaptureDevicesInfo);
		for (int i = CaptureDevicesInfo.Num()-1; i>=0; i--)
		{
			if (CaptureDevicesInfo[i].InputChannels <= 0)
				CaptureDevicesInfo.RemoveAt(i);
		}
		return CaptureDevicesInfo;
	}

	bool FConvaiAudioCaptureSynth::GetDefaultCaptureDeviceInfo(FCaptureDeviceInfo& OutInfo)
	{
		return AudioCapture.GetCaptureDeviceInfo(OutInfo);
	}

	bool FConvaiAudioCaptureSynth::GetCaptureDeviceInfo(FCaptureDeviceInfo& OutInfo, int32 DeviceIndex)
	{
		// DeviceIndex is a 0-based index into the input-capable devices. Resolve it from
		// the engine's input-only device list (which is bounded by the real device count)
		// rather than walking absolute device indices until the backend reports "past the
		// end". The RtAudio backend used on macOS returns success for EVERY index (errors
		// go to a callback, not an exception), so such a loop never terminates and hangs
		// the game thread on machines with no matching input device.
		TArray<FCaptureDeviceInfo> InputDevices = GetCaptureDevicesAvailable();
		if (DeviceIndex >= 0 && DeviceIndex < InputDevices.Num())
		{
			OutInfo = InputDevices[DeviceIndex];
			return true;
		}
		return false;
	}

	void FConvaiAudioCaptureSynth::OnAudioCaptured(const float* AudioData, int32 NumFrames, int32 NumChannels)
	{
		int32 NumSamples = NumChannels * NumFrames;

		FScopeLock Lock(&CaptureCriticalSection);

		if (bIsCapturing)
		{
			// Append the audio memory to the capture data buffer
			int32 Index = AudioCaptureData.AddUninitialized(NumSamples);
			float* AudioCaptureDataPtr = AudioCaptureData.GetData();
			FMemory::Memcpy(&AudioCaptureDataPtr[Index], AudioData, NumSamples * sizeof(float));
		}
	}

	bool FConvaiAudioCaptureSynth::OpenDefaultStream()
	{
		bool bSuccess = true;
		if (!AudioCapture.IsStreamOpen())
		{
			// Capture the shared destroyed flag by value so the lambda can safely
			// check it even after FConvaiAudioCaptureSynth is freed
			TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> DestroyedFlag = bIsBeingDestroyed;
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
			Audio::FOnAudioCaptureFunction OnCapture = [this, DestroyedFlag](const void* AudioData, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverFlow)
			{
				if (*DestroyedFlag) return;
				OnAudioCaptured(static_cast<const float*>(AudioData), NumFrames, NumChannels);
			};
#else
			FOnCaptureFunction OnCapture = [this, DestroyedFlag](const float* AudioData, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverFlow)
			{
				if (*DestroyedFlag) return;
				// RtAudio backend (UE <= 5.2) can deliver degenerate buffers on overflow / device hiccups.
				if (!AudioData) return;
				if (NumFrames <= 0 || NumChannels <= 0 || NumChannels > 8) return;
				if (static_cast<int64>(NumFrames) * NumChannels > 2 * 2 * 48000) return;
				OnAudioCaptured(AudioData, NumFrames, NumChannels);
			};
#endif

			// Prepare the audio buffer memory for 2 seconds of stereo audio at 48k SR to reduce chance for allocation in callbacks
			AudioCaptureData.Reserve(2 * 2 * 48000);

			FAudioCaptureDeviceParams Params = FAudioCaptureDeviceParams();

			// Start the stream here to avoid hitching the audio render thread.
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
			if (AudioCapture.OpenAudioCaptureStream(Params, MoveTemp(OnCapture), 1024))
#else
			if (AudioCapture.OpenCaptureStream(Params, MoveTemp(OnCapture), 1024))
#endif
			{
				AudioCapture.StartStream();
			}
			else
			{
				bSuccess = false;
			}
		}
		return bSuccess;
	}

	bool FConvaiAudioCaptureSynth::OpenStream(int32 DeviceIndex)
	{
		bool bSuccess = true;
		if (!AudioCapture.IsStreamOpen())
		{
			TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> DestroyedFlag = bIsBeingDestroyed;
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
			Audio::FOnAudioCaptureFunction OnCapture = [this, DestroyedFlag](const void* AudioData, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverFlow)
			{
				if (*DestroyedFlag) return;
				OnAudioCaptured(static_cast<const float*>(AudioData), NumFrames, NumChannels);
			};
#else
			FOnCaptureFunction OnCapture = [this, DestroyedFlag](const float* AudioData, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverFlow)
			{
				if (*DestroyedFlag) return;
				// RtAudio backend (UE <= 5.2) can deliver degenerate buffers on overflow / device hiccups.
				if (!AudioData) return;
				if (NumFrames <= 0 || NumChannels <= 0 || NumChannels > 8) return;
				if (static_cast<int64>(NumFrames) * NumChannels > 2 * 2 * 48000) return;
				OnAudioCaptured(AudioData, NumFrames, NumChannels);
			};
#endif

			// Prepare the audio buffer memory for 2 seconds of stereo audio at 48k SR to reduce chance for allocation in callbacks
			AudioCaptureData.Reserve(2 * 2 * 48000);

			FAudioCaptureDeviceParams Params = FAudioCaptureDeviceParams();
			Params.DeviceIndex = DeviceIndex;
			// Start the stream here to avoid hitching the audio render thread. 
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
			if (AudioCapture.OpenAudioCaptureStream(Params, MoveTemp(OnCapture), 1024))
#else
			if (AudioCapture.OpenCaptureStream(Params, MoveTemp(OnCapture), 1024))
#endif
			{
				AudioCapture.StartStream();
			}
			else
			{
				bSuccess = false;
			}
		}
		return bSuccess;
	}

	void FConvaiAudioCaptureSynth::CloseStream()
	{
		if (AudioCapture.IsStreamOpen())
		{
			AudioCapture.CloseStream();
		}
	}

	bool FConvaiAudioCaptureSynth::StartCapturing()
	{
		FScopeLock Lock(&CaptureCriticalSection);

		AudioCaptureData.Reset();

		if (!AudioCapture.IsStreamOpen())
		{
			return false;
		}

		bIsCapturing = true;
		return true;
	}

	void FConvaiAudioCaptureSynth::StopCapturing()
	{
		FScopeLock Lock(&CaptureCriticalSection);
		bIsCapturing = false;
	}

	void FConvaiAudioCaptureSynth::AbortCapturing()
	{
		AudioCapture.AbortStream();
		AudioCapture.CloseStream();
	}

	bool FConvaiAudioCaptureSynth::IsStreamOpen() const
	{
		return AudioCapture.IsStreamOpen();
	}

	bool FConvaiAudioCaptureSynth::IsCapturing() const
	{
		return bIsCapturing;
	}

	int32 FConvaiAudioCaptureSynth::GetNumSamplesEnqueued()
	{
		FScopeLock Lock(&CaptureCriticalSection);
		return AudioCaptureData.Num();
	}

	FAudioCapture* FConvaiAudioCaptureSynth::GetAudioCapture()
	{
		return &AudioCapture;
	}

	void FConvaiAudioCaptureSynth::MarkBeingDestroyed()
	{
		*bIsBeingDestroyed = true;
	}

	bool FConvaiAudioCaptureSynth::GetAudioData(TArray<float>& OutAudioData)
	{
		FScopeLock Lock(&CaptureCriticalSection);

		int32 CaptureDataSamples = AudioCaptureData.Num();
		if (CaptureDataSamples > 0)
		{
			// Append the capture audio to the output buffer
			int32 OutIndex = OutAudioData.AddUninitialized(CaptureDataSamples);
			float* OutDataPtr = OutAudioData.GetData();
			FMemory::Memcpy(&OutDataPtr[OutIndex], AudioCaptureData.GetData(), CaptureDataSamples * sizeof(float));

			// Reset the capture data buffer since we copied the audio out
			AudioCaptureData.Reset();
			return true;
		}
		return false;
	}
};

UConvaiAudioCaptureComponent::UConvaiAudioCaptureComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bSuccessfullyInitialized = false;
	bIsCapturing = false;
	CapturedAudioDataSamples = 0;
	ReadSampleIndex = 0;
	bIsDestroying = false;
	bIsNotReadyForForFinishDestroy = false;
	bIsStreamOpen = false;
	CaptureAudioData.Reserve(2 * 48000 * 5);
	SelectedDeviceIndex = -1;
}

bool UConvaiAudioCaptureComponent::Init(int32& SampleRate)
{

	Audio::FCaptureDeviceInfo DeviceInfo;
	bool FoundDevice = false;
	if (SelectedDeviceIndex == -1)
	{
		FoundDevice = CaptureSynth.GetDefaultCaptureDeviceInfo(DeviceInfo);

		// The engine's default-capture query resolves to the host's default *input*
		// device, but on some hosts it comes back as an output-only device with zero
		// input channels (e.g. macOS when no default input is configured), which can
		// never capture. In that case fall back to the first real input device.
		if (!FoundDevice || DeviceInfo.InputChannels <= 0)
		{
			Audio::FCaptureDeviceInfo InputDeviceInfo;
			if (CaptureSynth.GetCaptureDeviceInfo(InputDeviceInfo, 0) && InputDeviceInfo.InputChannels > 0)
			{
				DeviceInfo = InputDeviceInfo;
				SelectedDeviceIndex = 0; // input-device index; the engine maps it to the real device on OpenStream
				FoundDevice = true;
			}
		}
	}
	else
	{
		FoundDevice = CaptureSynth.GetCaptureDeviceInfo(DeviceInfo, SelectedDeviceIndex);
		if (!FoundDevice)
		{
			FoundDevice = CaptureSynth.GetDefaultCaptureDeviceInfo(DeviceInfo);
		}
	}

	CONVAI_LOG(ConvaiAudioLog, Log, TEXT("Using %s as Audio capture device with NumChannels:%d and SampleRate:%d"), *DeviceInfo.DeviceName, DeviceInfo.InputChannels, DeviceInfo.PreferredSampleRate);

	if (FoundDevice)
	{
		SampleRate = DeviceInfo.PreferredSampleRate;
		NumChannels = DeviceInfo.InputChannels;

		// Only support mono and stereo mic inputs for now...
		if (NumChannels == 1 || NumChannels == 2)
		{
			// This may fail if capture synths aren't supported on a given platform or if something went wrong with the capture device
			bIsStreamOpen = CaptureSynth.OpenStream(SelectedDeviceIndex);
			if (!bIsStreamOpen)
			{
				CONVAI_LOG(ConvaiAudioLog, Warning, TEXT("OpenStream returned false."));
			}
			return true;
		}
		else if (NumChannels <= 0)
		{
			// No usable input device. Common on desktops/Mac minis with no built-in
			// or attached microphone (the selected device is output-only). This is a
			// hardware/config issue, not a permission one — say so plainly.
			CONVAI_LOG(ConvaiAudioLog, Error, TEXT("No audio input (microphone) device found - '%s' reports %d input channels. Connect a microphone/input device; Convai cannot capture audio without one."), *DeviceInfo.DeviceName, NumChannels);
			return true;
		}
		else
		{
			CONVAI_LOG(ConvaiAudioLog, Warning, TEXT("Audio capture components only support mono and stereo mic input - Audio might be mangeled."));
			return true;
		}

	}
	return false;
}

void UConvaiAudioCaptureComponent::BeginDestroy()
{
	bIsDestroying = true;
	bIsNotReadyForForFinishDestroy = false;

	// Signal the capture callback as early as possible. RtAudio's worker thread
	// (UE <= 5.2) keeps firing callbacks until the stream is actually closed,
	// which doesn't happen until ~FConvaiAudioCaptureSynth runs. Flipping the
	// shared flag here closes the window where a callback could enter
	// OnAudioCaptured on a partially-destroyed object.
	CaptureSynth.MarkBeingDestroyed();

	if (CaptureSynth.IsCapturing())
	{
		CaptureSynth.StopCapturing();
	}

	// Signal the WASAPI thread to stop, but do NOT call CloseStream here.
	// AbortStream just sets the stop flag — it doesn't release COM objects.
	// CloseStream will run later in ~FConvaiAudioCaptureSynth, by which time
	// the thread has had time to exit the blocking GetBuffer call.
	if (CaptureSynth.IsStreamOpen())
	{
		CaptureSynth.GetAudioCapture()->AbortStream();
	}
	bIsStreamOpen = false;

	Stop();

	Super::BeginDestroy();
}

bool UConvaiAudioCaptureComponent::IsReadyForFinishDestroy()
{
	return !bIsNotReadyForForFinishDestroy;
}

void UConvaiAudioCaptureComponent::FinishDestroy()
{
	// Stream closure is handled by ~FConvaiAudioCaptureSynth -> ~FAudioCapture
	// which properly joins the WASAPI thread before releasing COM resources

	Super::FinishDestroy();
	bSuccessfullyInitialized = false;
	bIsCapturing = false;
	bIsDestroying = false;
	bIsStreamOpen = false;
}

bool UConvaiAudioCaptureComponent::GetDefaultCaptureDeviceInfo(Audio::FCaptureDeviceInfo& OutInfo)
{
	return CaptureSynth.GetDefaultCaptureDeviceInfo(OutInfo);
}

bool UConvaiAudioCaptureComponent::GetCaptureDeviceInfo(Audio::FCaptureDeviceInfo& OutInfo, int32 DeviceIndex)
{
	return CaptureSynth.GetCaptureDeviceInfo(OutInfo, DeviceIndex);
}

TArray<Audio::FCaptureDeviceInfo> UConvaiAudioCaptureComponent::GetCaptureDevicesAvailable()
{
	return CaptureSynth.GetCaptureDevicesAvailable();
}

int32 UConvaiAudioCaptureComponent::GetActiveCaptureDevice(Audio::FCaptureDeviceInfo& OutInfo)
{
	if (SelectedDeviceIndex == -1)
		GetDefaultCaptureDeviceInfo(OutInfo);
	else
		GetCaptureDeviceInfo(OutInfo, SelectedDeviceIndex);
	return SelectedDeviceIndex;
}

bool UConvaiAudioCaptureComponent::SetCaptureDevice(int32 DeviceIndex)
{
	Audio::FCaptureDeviceInfo OutInfo;
	if (!GetCaptureDeviceInfo(OutInfo, DeviceIndex))
	{
		return false;
	}

	const bool bWasActive = bIsStreamOpen;

	// Quiesce the audio render thread first so OnGenerateAudio cannot enter
	// CaptureSynth while we tear the stream down on the game thread.
	if (bWasActive)
	{
		Stop();
	}

	if (CaptureSynth.IsCapturing())
	{
		CaptureSynth.StopCapturing();
	}

	if (CaptureSynth.IsStreamOpen())
	{
		// AbortStream signals the WASAPI worker before CloseStream waits on it.
		CaptureSynth.GetAudioCapture()->AbortStream();
		CaptureSynth.CloseStream();
	}
	bIsStreamOpen = false;

	SelectedDeviceIndex = DeviceIndex;

	if (bWasActive)
	{
		bIsStreamOpen = CaptureSynth.OpenStream(SelectedDeviceIndex);
		if (bIsStreamOpen)
		{
			Start();
		}
		else
		{
			CONVAI_LOG(ConvaiAudioLog, Warning, TEXT("SetCaptureDevice: OpenStream failed for device index %d"), SelectedDeviceIndex);
			return false;
		}
	}

	return true;
}

bool UConvaiAudioCaptureComponent::IsCaptureStreamHealthy() const
{
	return bIsStreamOpen && CaptureSynth.IsStreamOpen() && CaptureSynth.IsCapturing();
}

Audio::FConvaiAudioCaptureSynth* UConvaiAudioCaptureComponent::GetCaptureSynth()
{
	return &CaptureSynth;
}

void UConvaiAudioCaptureComponent::OnBeginGenerate()
{
	CapturedAudioDataSamples = 0;
	ReadSampleIndex = 0;
	CaptureAudioData.Reset();

	if (!bIsStreamOpen)
	{
		bIsStreamOpen = CaptureSynth.OpenDefaultStream();
	}

	if (bIsStreamOpen)
	{
		if (!CaptureSynth.StartCapturing())
		{
			bIsStreamOpen = false;
			return;
		}

		bIsNotReadyForForFinishDestroy = true;
		FramesSinceStarting = 0;
		ReadSampleIndex = 0;
	}

}

void UConvaiAudioCaptureComponent::OnEndGenerate()
{
	if (bIsStreamOpen)
	{
		if (!CaptureSynth.IsStreamOpen())
		{
			CONVAI_LOG(ConvaiAudioLog, Verbose, TEXT("OnEndGenerate: stream already closed by BeginDestroy"));
			bIsStreamOpen = false;
			bIsNotReadyForForFinishDestroy = false;
			return;
		}
		// Only stop capturing (flag), do NOT close/abort the stream from the audio render thread.
		// Closing the stream here can race with the WASAPI capture thread.
		// The stream will be properly stopped in BeginDestroy or by ~FConvaiAudioCaptureSynth.
		CaptureSynth.StopCapturing();
		bIsStreamOpen = false;

		bIsNotReadyForForFinishDestroy = false;
	}
}

int32 UConvaiAudioCaptureComponent::OnGenerateAudio(float* OutAudio, int32 NumSamples)
{
	// Don't do anything if the stream isn't open
	if (!bIsStreamOpen || !CaptureSynth.IsStreamOpen() || !CaptureSynth.IsCapturing())
	{
		// Just return NumSamples, which uses zero'd buffer
		return NumSamples;
	}

	int32 OutputSamplesGenerated = 0;

	if (CapturedAudioDataSamples > 0 || CaptureSynth.GetNumSamplesEnqueued() > 1024)
	{
		// Check if we need to read more audio data from capture synth
		if (ReadSampleIndex + NumSamples > CaptureAudioData.Num())
		{
			// but before we do, copy off the remainder of the capture audio data buffer if there's data in it
			int32 SamplesLeft = FMath::Max(0, CaptureAudioData.Num() - ReadSampleIndex);
			if (SamplesLeft > 0)
			{
				float* CaptureDataPtr = CaptureAudioData.GetData();
				if (CaptureDataPtr)
				{
					FMemory::Memcpy(OutAudio, &CaptureDataPtr[ReadSampleIndex], SamplesLeft * sizeof(float));
					// Track samples generated
					OutputSamplesGenerated += SamplesLeft;
				}
			}

			// Get another block of audio from the capture synth
			CaptureAudioData.Reset();
			CaptureSynth.GetAudioData(CaptureAudioData);

			// Reset the read sample index since we got a new buffer of audio data
			ReadSampleIndex = 0;
		}

		// note it's possible we didn't get any more audio in our last attempt to get it
		if (CaptureAudioData.Num() > 0)
		{
			// Compute samples to copy
			int32 NumSamplesToCopy = FMath::Min(NumSamples - OutputSamplesGenerated, CaptureAudioData.Num() - ReadSampleIndex);

			float* CaptureDataPtr = CaptureAudioData.GetData();
			if (CaptureDataPtr)
			{
				FMemory::Memcpy(&OutAudio[OutputSamplesGenerated], &CaptureDataPtr[ReadSampleIndex], NumSamplesToCopy * sizeof(float));
				ReadSampleIndex += NumSamplesToCopy;
				OutputSamplesGenerated += NumSamplesToCopy;
			}
		}

		CapturedAudioDataSamples += OutputSamplesGenerated;
	}
	else
	{
		// Say we generated the full samples, this will result in silence
		OutputSamplesGenerated = NumSamples;
	}

	return OutputSamplesGenerated;
}

