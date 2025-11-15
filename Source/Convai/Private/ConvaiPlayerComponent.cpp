// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiPlayerComponent.h"
#include "ConvaiAudioCaptureComponent.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiActionUtils.h"
#include "ConvaiUtils.h"
#include "ConvaiDefinitions.h"
#include "ConvaiSubsystem.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Net/UnrealNetwork.h"
#include "Misc/FileHelper.h"
#include "Http.h"
#include "ConvaiUtils.h"
#include "Containers/UnrealString.h"
#include "Kismet/GameplayStatics.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Engine/GameEngine.h"
#include "Sound/SoundWave.h"
#include "AudioDevice.h"
#include "AudioMixerDevice.h"
#include "UObject/ConstructorHelpers.h"
#include "ConvaiSubsystem.h"
#include "Engine/GameInstance.h"
#include "Async/Async.h"

DEFINE_LOG_CATEGORY(ConvaiPlayerLog);

static FAudioDevice* GetAudioDeviceFromWorldContext(const UObject* WorldContextObject)
{
	UWorld* ThisWorld = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!ThisWorld || !ThisWorld->bAllowAudioPlayback || ThisWorld->GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}

	return ThisWorld->GetAudioDevice().GetAudioDevice();
}

static Audio::FMixerDevice* GetAudioMixerDeviceFromWorldContext(const UObject* WorldContextObject)
{
	if (FAudioDevice* AudioDevice = GetAudioDeviceFromWorldContext(WorldContextObject))
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
		bool Found = AudioDevice != nullptr;
#else
		bool Found = AudioDevice != nullptr && AudioDevice->IsAudioMixerEnabled();
#endif

		if (!Found)
		{
			return nullptr;
		}
		else
		{
			return static_cast<Audio::FMixerDevice*>(AudioDevice);
		}
	}
	return nullptr;
}

UConvaiPlayerComponent::UConvaiPlayerComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	bAutoActivate = true;
	PlayerName = "Guest";

	IsInit = false;
	VoiceCaptureRingBuffer.Init(ConvaiConstants::VoiceCaptureRingBufferCapacity);
	VoiceCaptureBuffer.Empty(ConvaiConstants::VoiceCaptureBufferSize);
	UsePixelStreamingMicInput = true;
	bAutoInitializeSession = true;

	ConvaiAudioProcessing = nullptr; 

	const FString FoundSubmixPath = "/ConvAI/Submixes/AudioInput.AudioInput";
	static ConstructorHelpers::FObjectFinder<USoundSubmixBase> SoundSubmixFinder(*FoundSubmixPath);
	if (SoundSubmixFinder.Succeeded())
	{
		_FoundSubmix = SoundSubmixFinder.Object;
	}
}

void UConvaiPlayerComponent::OnComponentCreated()
{
	Super::OnComponentCreated();

	AudioCaptureComponent = NewObject<UConvaiAudioCaptureComponent>(this, UConvaiAudioCaptureComponent::StaticClass(), TEXT("ConvaiAudioCapture"));
	if (AudioCaptureComponent)
	{
		AudioCaptureComponent->RegisterComponent();
	}

	if (_FoundSubmix != nullptr) {
		AudioCaptureComponent->SoundSubmix = _FoundSubmix;
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("UConvaiPlayerComponent: Found submix \"AudioInput\""));
	}
	else
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("UConvaiPlayerComponent: Audio Submix was not found, please ensure an audio submix exists at this directory: \"/ConvAI/Submixes/AudioInput\" then restart Unreal Engine"));
	}
}

void UConvaiPlayerComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UConvaiPlayerComponent, PlayerName);
}

bool UConvaiPlayerComponent::Init()
{
	if (IsInit)
	{
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("AudioCaptureComponent is already init"));
		return true;
	}

	FString commandMicNoiseGateThreshold = "voice.MicNoiseGateThreshold  0.01";
	FString commandSilenceDetectionThreshold = "voice.SilenceDetectionThreshold 0.001";

	APlayerController* PController = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (PController)
	{
		PController->ConsoleCommand(*commandMicNoiseGateThreshold, true);
		PController->ConsoleCommand(*commandSilenceDetectionThreshold, true);
	}

	AudioCaptureComponent = Cast<UConvaiAudioCaptureComponent>(GetOwner()->GetComponentByClass(UConvaiAudioCaptureComponent::StaticClass()));
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("Init: AudioCaptureComponent is not valid"));
		return false;
	}

	IsInit = true;
	return true;
}

void UConvaiPlayerComponent::SetPlayerName(FString NewPlayerName)
{
	PlayerName = NewPlayerName;

	if (GetIsReplicated())
	{
		SetPlayerNameServer(PlayerName);
	}
}

void UConvaiPlayerComponent::SetPlayerNameServer_Implementation(const FString& NewPlayerName)
{
	PlayerName = NewPlayerName;
}

void UConvaiPlayerComponent::SetSpeakerID(FString NewSpeakerID)
{
	SpeakerID = NewSpeakerID;

	if (GetIsReplicated())
	{
		SetSpeakerIDServer(SpeakerID);
	}
}

void UConvaiPlayerComponent::SetSpeakerIDServer_Implementation(const FString& NewSpeakerID)
{
	SpeakerID = NewSpeakerID;
}

bool UConvaiPlayerComponent::GetDefaultCaptureDeviceInfo(FCaptureDeviceInfoBP& OutInfo)
{
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("GetDefaultCaptureDeviceInfo: AudioCaptureComponent is not valid"));
		return false;
	}

	return false;
}

bool UConvaiPlayerComponent::GetCaptureDeviceInfo(FCaptureDeviceInfoBP& OutInfo, int DeviceIndex)
{
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("GetCaptureDeviceInfo: AudioCaptureComponent is not valid"));
		return false;
	}
	Audio::FCaptureDeviceInfo OutDeviceInfo;
	if (AudioCaptureComponent->GetCaptureDeviceInfo(OutDeviceInfo, DeviceIndex))
	{
		OutInfo.bSupportsHardwareAEC = OutDeviceInfo.bSupportsHardwareAEC;
		OutInfo.LongDeviceId = OutDeviceInfo.DeviceId;
		OutInfo.DeviceName = OutDeviceInfo.DeviceName;
		OutInfo.InputChannels = OutDeviceInfo.InputChannels;
		OutInfo.PreferredSampleRate = OutDeviceInfo.PreferredSampleRate;
		OutInfo.DeviceIndex = DeviceIndex;
		return true;
	}

	return false;
}

TArray<FCaptureDeviceInfoBP> UConvaiPlayerComponent::GetAvailableCaptureDeviceDetails()
{
	TArray<FCaptureDeviceInfoBP> FCaptureDevicesInfoBP;
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("GetAvailableCaptureDeviceDetails: AudioCaptureComponent is not valid"));
		return FCaptureDevicesInfoBP;
	}

	int i = 0;
	for (auto DeviceInfo : AudioCaptureComponent->GetCaptureDevicesAvailable())
	{
		FCaptureDeviceInfoBP CaptureDeviceInfoBP;
		CaptureDeviceInfoBP.bSupportsHardwareAEC = DeviceInfo.bSupportsHardwareAEC;
		CaptureDeviceInfoBP.LongDeviceId = DeviceInfo.DeviceId;
		CaptureDeviceInfoBP.DeviceName = DeviceInfo.DeviceName;
		CaptureDeviceInfoBP.InputChannels = DeviceInfo.InputChannels;
		CaptureDeviceInfoBP.PreferredSampleRate = DeviceInfo.PreferredSampleRate;
		CaptureDeviceInfoBP.DeviceIndex = i++;
		FCaptureDevicesInfoBP.Add(CaptureDeviceInfoBP);
	}
	return FCaptureDevicesInfoBP;
}

TArray<FString> UConvaiPlayerComponent::GetAvailableCaptureDeviceNames()
{
	TArray<FString> AvailableDeviceNames;
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("GetAvailableCaptureDeviceNames: AudioCaptureComponent is not valid"));
		return AvailableDeviceNames;
	}

	for (auto CaptureDeviceInfo : GetAvailableCaptureDeviceDetails())
	{
		AvailableDeviceNames.Add(CaptureDeviceInfo.DeviceName);
	}

	return AvailableDeviceNames;
}

void UConvaiPlayerComponent::GetActiveCaptureDevice(FCaptureDeviceInfoBP& OutInfo)
{
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("GetActiveCaptureDevice: AudioCaptureComponent is not valid"));
		return;
	}
	Audio::FCaptureDeviceInfo OutDeviceInfo;
	int SelectedDeviceIndex = AudioCaptureComponent->GetActiveCaptureDevice(OutDeviceInfo);
	OutInfo.bSupportsHardwareAEC = OutDeviceInfo.bSupportsHardwareAEC;
	OutInfo.LongDeviceId = OutDeviceInfo.DeviceId;
	OutInfo.DeviceName = OutDeviceInfo.DeviceName;
	OutInfo.InputChannels = OutDeviceInfo.InputChannels;
	OutInfo.PreferredSampleRate = OutDeviceInfo.PreferredSampleRate;
	OutInfo.DeviceIndex = SelectedDeviceIndex;
}

bool UConvaiPlayerComponent::SetCaptureDeviceByIndex(int DeviceIndex)
{
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetCaptureDeviceByIndex: AudioCaptureComponent is not valid"));
		return false;
	}

	if (DeviceIndex >= GetAvailableCaptureDeviceDetails().Num())
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetCaptureDeviceByIndex: Invalid Device Index: %d - Max possible index: %d."), DeviceIndex, GetAvailableCaptureDeviceDetails().Num() - 1);
		return false;
	}

	return AudioCaptureComponent->SetCaptureDevice(DeviceIndex);
}

bool UConvaiPlayerComponent::SetCaptureDeviceByName(FString DeviceName)
{
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetCaptureDeviceByName: AudioCaptureComponent is not valid"));
		return false;
	}

	bool bDeviceFound = false;
	int DeviceIndex = -1;
	for (auto CaptureDeviceInfo : GetAvailableCaptureDeviceDetails())
	{
		if (CaptureDeviceInfo.DeviceName == DeviceName)
		{
			bDeviceFound = true;
			DeviceIndex = CaptureDeviceInfo.DeviceIndex;
		}
	}

	TArray<FString> AvailableDeviceNames;

	if (!bDeviceFound)
	{
		AvailableDeviceNames = GetAvailableCaptureDeviceNames();
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetCaptureDeviceByName: Could not find Device name: %s - Available Device names are: [%s]."), *DeviceName, *FString::Join(AvailableDeviceNames, *FString(" - ")));
		return false;
	}

	if (!SetCaptureDeviceByIndex(DeviceIndex))
	{
		AvailableDeviceNames = GetAvailableCaptureDeviceNames();
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetCaptureDeviceByName: SetCaptureDeviceByIndex failed for index: %d and device name: %s - Available Device names are: [%s]."), DeviceIndex, *DeviceName, *FString::Join(AvailableDeviceNames, *FString(" - ")));
		return false;
	}
	return true;
}

void UConvaiPlayerComponent::SetMicrophoneVolumeMultiplier(float InVolumeMultiplier, bool& Success)
{
	Success = false;
	if (IsValid(AudioCaptureComponent))
	{
		AudioCaptureComponent->SetVolumeMultiplier(InVolumeMultiplier);
		Success = true;
	}

	if (PixelStreamingAudioComponent.IsValid())
	{
		AudioCaptureComponent->SetVolumeMultiplier(InVolumeMultiplier * 2); // multiply by 2 because Pixel Streaming mic is usually very low
		Success = true;
	}

	if (!IsValid(AudioCaptureComponent) && !PixelStreamingAudioComponent.IsValid())
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetMicrophoneVolumeMultiplier: AudioCaptureComponent and PixelStreamingAudioComponent are not valid"));
	}
}

void UConvaiPlayerComponent::GetMicrophoneVolumeMultiplier(float& OutVolumeMultiplier, bool& Success)
{
	Success = false;
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetMicrophoneVolumeMultiplier: AudioCaptureComponent is not valid"));
		return;
	}
	auto InternalAudioComponent = AudioCaptureComponent->GetAudioComponent();
	if (!InternalAudioComponent)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("GetMicrophoneVolumeMultiplier: InternalAudioComponent is not valid"));
	}
	OutVolumeMultiplier = InternalAudioComponent->VolumeMultiplier;
	Success = true;
}

void UConvaiPlayerComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!IsInit || !IsValid(AudioCaptureComponent))
	{
		return;
	}

	UpdateVoiceCapture(DeltaTime);
}

void UConvaiPlayerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Clear the audio processing component reference first to prevent new audio thread calls
	ConvaiAudioProcessing = nullptr;

	// Unregister from the ConvaiSubsystem and unbind connection state changes
	if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		ConvaiSubsystem->UnregisterPlayerComponent(this);
		ConvaiSubsystem->OnServerConnectionStateChangedEvent.RemoveDynamic(this, &UConvaiPlayerComponent::OnServerConnectionStateChanged);
	}
	
	if (IsRecording)
		FinishRecording();

	if (IsStreaming)
		MuteStreamingAudio();

	// Shut down any active session
	if (IsValid(SessionProxyInstance))
	{
		StopSession();
	}

	Super::EndPlay(EndPlayReason);
}

bool UConvaiPlayerComponent::IsPixelStreamingEnabledAndAllowed()
{
	return UsePixelStreamingMicInput && PixelStreamingAudioComponent.IsValid();
}

void UConvaiPlayerComponent::UpdateVoiceCapture(float DeltaTime)
{
	if (IsRecording || IsStreaming) {
		RemainingTimeUntilNextUpdate -= DeltaTime;
		if (RemainingTimeUntilNextUpdate <= 0)
		{
			float ExpectedRecordingTime = DeltaTime > TIME_BETWEEN_VOICE_UPDATES_SECS ? DeltaTime : TIME_BETWEEN_VOICE_UPDATES_SECS;

			TWeakObjectPtr<UConvaiPlayerComponent> WeakThis(this);
			FAudioThread::RunCommandOnAudioThread([WeakThis, ExpectedRecordingTime]()
				{
					if (!WeakThis.IsValid())
						return;

					WeakThis->StopVoiceChunkCapture();
                    WeakThis->StartVoiceChunkCapture(ExpectedRecordingTime);
				});
			RemainingTimeUntilNextUpdate = TIME_BETWEEN_VOICE_UPDATES_SECS;
		}
	}
	else
	{
		RemainingTimeUntilNextUpdate = 0;
	}
}

void UConvaiPlayerComponent::StartVoiceChunkCapture(float ExpectedRecordingTime)
{
	//GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Yellow, FString::Printf(TEXT("StartVoiceChunkCapture() in VoiceCaptureComp.cpp")));
	//CONVAI_LOG(LogTemp, Warning, TEXT("StartVoiceChunkCapture() in VoiceCaptureComp.cpp"));
	UAudioMixerBlueprintLibrary::StartRecordingOutput(this, ExpectedRecordingTime, Cast<USoundSubmix>(AudioCaptureComponent->SoundSubmix));
}

void UConvaiPlayerComponent::ReadRecordedBuffer(Audio::AlignedFloatBuffer& RecordedBuffer, float& OutNumChannels, float& OutSampleRate)
{
	if (Audio::FMixerDevice* MixerDevice = GetAudioMixerDeviceFromWorldContext(this))
	{
		// call the thing here.
		RecordedBuffer = MixerDevice->StopRecording(Cast<USoundSubmix>(AudioCaptureComponent->SoundSubmix), OutNumChannels, OutSampleRate);

		if (RecordedBuffer.Num() == 0)
		{
			//CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("ReadRecordedBuffer: No audio data. Did you call Start Recording Output?"));
		}
	}
	else
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("ReadRecordedBuffer: Could not get MixerDevice"));
	}
}

void UConvaiPlayerComponent::StartAudioCaptureComponent()
{
	if (IsPixelStreamingEnabledAndAllowed())
	{
		PixelStreamingAudioComponent->Start();
	}
	else
	{
		AudioCaptureComponent->Start();
	}
}

void UConvaiPlayerComponent::StopAudioCaptureComponent()
{
	if (IsPixelStreamingEnabledAndAllowed())
	{
		PixelStreamingAudioComponent->Stop();
	}
	AudioCaptureComponent->Stop();
}

void UConvaiPlayerComponent::StopVoiceChunkCapture()
{
	float NumChannels;
	float SampleRate;
	Audio::AlignedFloatBuffer RecordedBuffer = Audio::AlignedFloatBuffer();

	ReadRecordedBuffer(RecordedBuffer, NumChannels, SampleRate);

	if (RecordedBuffer.Num() == 0)
		return;

	Audio::TSampleBuffer<int16> Int16Buffer = Audio::TSampleBuffer<int16>(RecordedBuffer, NumChannels, SampleRate);
	TArray<int16> OutConverted;

	if (NumChannels > 1 || SampleRate != static_cast<int32>(ConvaiConstants::VoiceCaptureSampleRate))
	{
		UConvaiUtils::ResampleAudio(SampleRate, ConvaiConstants::VoiceCaptureSampleRate, NumChannels, true, static_cast<TArray<int16>>(Int16Buffer.GetArrayView()), Int16Buffer.GetNumSamples(), OutConverted);
	}
	else
	{
		OutConverted = static_cast<TArray<int16>>(Int16Buffer.GetArrayView());
	}

	if (IsRecording)
	{
		/*if (SupportsAudioProcessing())
		{
			ConvaiAudioProcessing->ProcessAudioData(OutConverted.GetData(), OutConverted.Num(), ConvaiConstants::VoiceCaptureSampleRate);
		}
		else {*/
			VoiceCaptureBuffer.Append((uint8*)OutConverted.GetData(), OutConverted.Num() * sizeof(int16));
		//}
	}

	// Write to the shared buffer for direct access
	if (IsStreaming)
	{
		if (bMute) return;
		
		if (SupportsAudioProcessing())
		{
			SafeProcessAudioData(OutConverted.GetData(), OutConverted.Num(), ConvaiConstants::VoiceCaptureSampleRate);
		}
		// Send audio to the session proxy if we have one
		else if (IsValid(SessionProxyInstance))
		{
			// Calculate the number of frames (samples per channel)
			const size_t NumFrames = OutConverted.Num();
			SessionProxyInstance->SendAudio((const int16_t*)OutConverted.GetData(), NumFrames);
		}
	}
}

void UConvaiPlayerComponent::StartRecording()
{
	if (IsRecording)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("StartRecording: already recording!"));
		return;
	}

	if (IsStreaming)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("StartRecording: already talking!"));
		return;
	}

	if (!IsInit)
	{
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("StartRecording: Initializing..."));
		if (!Init())
		{
			CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("StartRecording: Could not initialize"));
			return;
		}
	}

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Started Recording "));
	StartAudioCaptureComponent();    //Start the AudioCaptureComponent

	// reset audio buffers
	StartVoiceChunkCapture();
	StopVoiceChunkCapture();
	VoiceCaptureBuffer.Empty(ConvaiConstants::VoiceCaptureBufferSize);

	IsRecording = true;
}

USoundWave* UConvaiPlayerComponent::FinishRecording()
{
	if (!IsRecording)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("FinishRecording: did not start recording"));
		return nullptr;
	}

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Stopped Recording "));
	StopVoiceChunkCapture();

	// Save the recorded audio to disk for debugging
	FString FileName = FPaths::ProjectSavedDir() / TEXT("AudioDebug/recorded_audio.wav");

	// Ensure directory exists
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FileName), true);

	// Create WAV file data
	TArray<uint8> WavFileData;
	UConvaiUtils::PCMDataToWav(VoiceCaptureBuffer, WavFileData, 1, ConvaiConstants::VoiceCaptureSampleRate);

	// Save to disk
	UConvaiUtils::SaveByteArrayAsFile(FileName, WavFileData);

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Saved recorded audio to %s - %d bytes"),
		*FileName, VoiceCaptureBuffer.Num());

	USoundWave* OutSoundWave = UConvaiUtils::PCMDataToSoundWav(VoiceCaptureBuffer, 1, ConvaiConstants::VoiceCaptureSampleRate);
	StopAudioCaptureComponent();  //stop the AudioCaptureComponent
	if (IsValid(OutSoundWave))
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("OutSoundWave->GetDuration(): %f seconds "), OutSoundWave->GetDuration());
	IsRecording = false;
	return OutSoundWave;
}

void UConvaiPlayerComponent::BeginPlay()
{
	Super::BeginPlay();
	
	if (IsValid(AudioCaptureComponent))
	{
		AudioCaptureComponent->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
	}
	else
	{
		CONVAI_LOG(ConvaiPlayerLog, Error, TEXT("Could not attach AudioCaptureComponent"));
	}

	if (!IsInit)
	{
		if (!Init())
		{
			CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("Could not initialize Audio Decoder"));
			return;
		}
	}
	
	// Register with the ConvaiSubsystem and bind to connection state changes
	if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		ConvaiSubsystem->RegisterPlayerComponent(this);
		ConvaiSubsystem->OnServerConnectionStateChangedEvent.AddDynamic(this, &UConvaiPlayerComponent::OnServerConnectionStateChanged);
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Registered with ConvaiSubsystem and bound to server connection state changes"));
	}
	else
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("BeginPlay: ConvaiSubsystem is not valid"));
	}

	FString EnableAudioProcessingStr = UCommandLineUtils::GetCommandLineFlagValueAsString(TEXT("EnableAudioProcessing"), TEXT(""));
	if (!EnableAudioProcessingStr.IsEmpty())
	{
		bEnableAudioProcessingParse = EnableAudioProcessingStr.ToBool();
		UE_LOG(LogTemp, Log, TEXT("EnableAudioProcessing overridden from command line: %s"), bEnableAudioProcessingParse ? TEXT("true") : TEXT("false"));
		
	}

	if (ConvaiAudioProcessing == nullptr)
		FindFirstAudioProcessingComponent();
}

bool UConvaiPlayerComponent::ConsumeStreamingBuffer(TArray<uint8>& Buffer)
{
	// This method is kept for backward compatibility but should not be used
	// Use GetSharedAudioBuffer()->ConsumeAll() instead
	CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("ConsumeStreamingBuffer is deprecated. Use GetSharedAudioBuffer()->ConsumeAll() instead."));

	// For backward compatibility, we'll still provide the implementation
	int Datalength = VoiceCaptureRingBuffer.RingDataUsage();
	if (Datalength <= 0)
		return false;

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 5
	Buffer.SetNumUninitialized(Datalength, EAllowShrinking::No);
#else
	Buffer.SetNumUninitialized(Datalength, false);
#endif
	VoiceCaptureRingBuffer.Dequeue(Buffer.GetData(), Datalength);

	return true;
}

// Implementation of session-related functions

bool UConvaiPlayerComponent::StartSession()
{
	// If we already have a session, shut it down first
	if (IsValid(SessionProxyInstance))
	{
		StopSession();
	}

	// Create a new session proxy
	SessionProxyInstance = NewObject<UConvaiConnectionSessionProxy>(this);
	if (!IsValid(SessionProxyInstance))
	{
		CONVAI_LOG(ConvaiPlayerLog, Error, TEXT("Failed to create session proxy"));
		return false;
	}

	// Initialize the session proxy
	if (!SessionProxyInstance->Initialize(this, true))
	{
		CONVAI_LOG(ConvaiPlayerLog, Error, TEXT("Failed to initialize session proxy"));
		SessionProxyInstance = nullptr;
		return false;
	}

	// Connect the session
	if (!SessionProxyInstance->Connect())
	{
		CONVAI_LOG(ConvaiPlayerLog, Error, TEXT("Failed to connect session"));
		SessionProxyInstance = nullptr;
		return false;
	}

	UnmuteStreamingAudio();

	return true;
}

void UConvaiPlayerComponent::StopSession()
{
	if (IsValid(SessionProxyInstance))
	{
		// Stop streaming if we're currently streaming
		if (IsStreaming)
		{
			MuteStreamingAudio();
		}

		// Disconnect the session
		SessionProxyInstance->Disconnect();
		SessionProxyInstance = nullptr;
	}
}

void UConvaiPlayerComponent::SendText(UConvaiConversationComponent* ChatbotComponent, const FString Text) const
{
	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->SendTextMessage(Text);
	}
}

bool UConvaiPlayerComponent::UnmuteStreamingAudio()
{
	if (IsStreaming)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("UnmuteStreamingAudio: already streaming!"));
		return false;
	}

	if (IsRecording)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("UnmuteStreamingAudio: already recording!"));
		return false;
	}

	if (!IsInit)
	{
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("UnmuteStreamingAudio: Initializing..."));
		if (!Init())
		{
			CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("UnmuteStreamingAudio: Could not initialize"));
			return false;
		}
	}

	// Make sure we have a valid session
	if (!IsValid(SessionProxyInstance))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("UnmuteStreamingAudio: No valid session"));
		return false;
	}

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Started Streaming Audio"));

	StartAudioCaptureComponent();    // Start the AudioCaptureComponent

	// Reset audio buffers
	StartVoiceChunkCapture();
	StopVoiceChunkCapture();

	IsStreaming = true;
	VoiceCaptureRingBuffer.Empty();

	return true;
}

void UConvaiPlayerComponent::MuteStreamingAudio()
{
	if (!IsStreaming)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("MuteStreamingAudio: not streaming"));
		return;
	}

	StopVoiceChunkCapture();
	StopAudioCaptureComponent();  // Stop the AudioCaptureComponent
	IsStreaming = false;

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Stopped Streaming Audio"));
}

void UConvaiPlayerComponent::OnConnectedToServer()
{
	
}

void UConvaiPlayerComponent::OnDisconnectedFromServer()
{
	
}

void UConvaiPlayerComponent::OnServerConnectionStateChanged(EC_ConnectionState ConnectionState)
{
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Server connection state changed: %d"), static_cast<int32>(ConnectionState));
	
	// Auto-initialize session when connected and auto-init is enabled
	if (ConnectionState == EC_ConnectionState::Connected && bAutoInitializeSession)
	{
		// Only start if we don't already have an active session
		if (!IsValid(SessionProxyInstance))
		{
			CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Server connected and auto-initialize enabled - starting session"));
			StartSession();
		}
		else
		{
			CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Server connected but session already active"));
		}
	}
}

void UConvaiPlayerComponent::OnAttendeeConnected(FString AttendeeId)
{
	OnAttendeeConnectionStateChangedEvent.Broadcast(this, AttendeeId, EC_ConnectionState::Connected);
}

void UConvaiPlayerComponent::OnAttendeeDisconnected(FString AttendeeId)
{
	OnAttendeeConnectionStateChangedEvent.Broadcast(this, AttendeeId, EC_ConnectionState::Disconnected);
}

// IConvaiConnectionInterface implementation
void UConvaiPlayerComponent::OnTranscriptionReceived(FString Transcription, bool IsTranscriptionReady, bool IsFinal)
{
	const bool bHasContent = !Transcription.IsEmpty() || IsFinal;
	if (!bHasContent)
		return;
		
	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this, Transcription, IsTranscriptionReady, IsFinal]
			{
				OnTranscriptionReceived(Transcription, IsTranscriptionReady, IsFinal);
			});
		return;
	}

	// Handle transcription received from the server
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Transcription received: %s"), *Transcription);

	// Broadcast to any listeners
	OnTranscriptionReceivedDelegate.Broadcast(this, nullptr, Transcription, IsTranscriptionReady, IsFinal);
}

void UConvaiPlayerComponent::OnStartedTalking()
{
	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this]
			{
				OnStartedTalking();
			});
		return;
	}


	// Handle started talking event
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Started talking"));

	// Broadcast to any listeners
	OnStartedTalkingDelegate.Broadcast();
}

void UConvaiPlayerComponent::OnFinishedTalking()
{
	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this]
			{
				OnFinishedTalking();
			});
		return;
	}

	// Handle finished talking event
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Finished talking"));

	// Broadcast to any listeners
	OnFinishedTalkingDelegate.Broadcast();
}

void UConvaiPlayerComponent::OnAudioDataReceived(const int16_t* AudioData, size_t NumFrames, uint32_t SampleRate, uint32_t BitsPerSample, uint32_t NumChannels)
{
	// Handle audio data received from the server
	// This would typically be audio from other players
}

void UConvaiPlayerComponent::OnFailure(FString Message)
{
	// Handle failure
	CONVAI_LOG(ConvaiPlayerLog, Error, TEXT("Connection failure: %s"), *Message);
}

void UConvaiPlayerComponent::BeginDestroy()
{
	// Clear the audio processing component reference immediately to prevent crashes
	ConvaiAudioProcessing = nullptr;

	if (IsRecording)
	{
		FinishRecording();
	}

	if (IsStreaming)
	{
		MuteStreamingAudio();
	}

	if (IsValid(SessionProxyInstance))
	{
		StopSession();
	}

	VoiceCaptureRingBuffer.Empty();
	VoiceCaptureBuffer.Empty();

	if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		ConvaiSubsystem->UnregisterPlayerComponent(this);
		ConvaiSubsystem->OnServerConnectionStateChangedEvent.RemoveDynamic(this, &UConvaiPlayerComponent::OnServerConnectionStateChanged);
	}

	Super::BeginDestroy();
}


IConvaiAudioProcessingInterface* UConvaiPlayerComponent::FindFirstAudioProcessingComponent()
{
	// Find the Audio Processing component using interface
	auto AudioProcessingComponents = (GetOwner()->GetComponentsByInterface(UConvaiAudioProcessingInterface::StaticClass()));
	if (AudioProcessingComponents.Num())
	{
		SetAudioProcessingComponent(AudioProcessingComponents[0]);
	}
	return ConvaiAudioProcessing.GetInterface();
}

bool UConvaiPlayerComponent::SetAudioProcessingComponent(UActorComponent* AudioProcessingComponent)
{
	// Find the Audio Processing component
	if (AudioProcessingComponent && AudioProcessingComponent->GetClass()->ImplementsInterface(UConvaiAudioProcessingInterface::StaticClass()))
	{
		TScriptInterface<IConvaiAudioProcessingInterface> Tmp;
		Tmp.SetObject(AudioProcessingComponent);
		Tmp.SetInterface(Cast<IConvaiAudioProcessingInterface>(AudioProcessingComponent));
		ConvaiAudioProcessing = Tmp;

		if (IConvaiAudioProcessingInterface* Interface = ConvaiAudioProcessing.GetInterface())
		{
			Interface->SetProcessedAudioReceiver(this);
		}
		return true;
	}
	else
	{
		ConvaiAudioProcessing = nullptr;
		return false;
	}
}

bool UConvaiPlayerComponent::SupportsAudioProcessing()
{
	if (!bEnableAudioProcessingParse) return false;

	if (ConvaiAudioProcessing == nullptr)
	{
		FindFirstAudioProcessingComponent();
	}
	return ConvaiAudioProcessing != nullptr;
}

void UConvaiPlayerComponent::SafeProcessAudioData(const int16* AudioData, int32 NumSamples, int32 SampleRate)
{
	// Thread-safe check for audio processing component validity
	if (!ConvaiAudioProcessing)
	{
		return;
	}

	ConvaiAudioProcessing->ProcessAudioData(AudioData, NumSamples, SampleRate);
}

void UConvaiPlayerComponent::OnProcessedAudioDataReceived(const int16* ProcessedAudioData, int32 NumSamples, int32 SampleRate)
{

	if (IsRecording) {
		VoiceCaptureBuffer.Append((uint8*)ProcessedAudioData, NumSamples * sizeof(int16));
	}
	if (IsStreaming) {
		//VoiceCaptureRingBuffer.Enqueue((uint8*)ProcessedAudioData, NumSamples * sizeof(int16));

		if (IsValid(SessionProxyInstance))
		{
			SessionProxyInstance->SendAudio((const int16_t*)ProcessedAudioData, NumSamples);
		}
	}
}

bool UConvaiPlayerComponent::UpdateVadBP(bool EnableVAD)
{
	return ConvaiAudioProcessing && ConvaiAudioProcessing->UpdateVAD(EnableVAD);
}
