// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiPlayerComponent.h"
#include "ConvaiAudioCaptureComponent.h"
#include "ConvaiMicrophoneSubsystem.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiActionUtils.h"
#include "ConvaiUtils.h"
#include "ConvaiDefinitions.h"
#include "ConvaiSubsystem.h"
#include "ConvaiAndroid.h"
#include "TimerManager.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Net/UnrealNetwork.h"
#include "Misc/FileHelper.h"
#include "Http.h"
#include "ConvaiUtils.h"
#include "Containers/UnrealString.h"
#include "Kismet/GameplayStatics.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Engine/GameEngine.h"
// FHitResult moved from Engine/EngineTypes.h into its own header in UE 5.1;
// older engines don't pull it in transitively here (same pattern as ConvaiSpatial.cpp).
#if __has_include("Engine/HitResult.h")
#include "Engine/HitResult.h"
#else
#include "Engine/EngineTypes.h"
#endif
#include "Sound/SoundWave.h"
#include "AudioDevice.h"
#include "AudioMixerDevice.h"
#include "UObject/ConstructorHelpers.h"
#include "ConvaiSubsystem.h"
#include "Engine/GameInstance.h"
#include "Async/Async.h"
#include "Interface/ConvaiAudioCaptureInterface.h"
#include "ConvaiObjectComponent.h"
#include "ConvaiChatbotComponent.h"
#include "Gaze/ConvaiGazeHighlightActor.h"
#include "Gaze/ConvaiGazeCursorWidget.h"
#include "Blueprint/UserWidget.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Components/PrimitiveComponent.h"
#include "CollisionQueryParams.h"

DEFINE_LOG_CATEGORY(ConvaiPlayerLog);

namespace
{
	// Poll cadence for mic capture health, and the restart cadence while it is unhealthy (Android
	// permission grant / transient device-busy / device died mid-session). 10 consecutive failures
	// is ~10s, which covers a slow permission-dialog tap.
	constexpr float MicOpenRetryInterval = 1.0f;
	constexpr int32 MicOpenMaxRetries = 10;
}

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
	PlayerName = "User";

	// Default the gaze highlight + cursor to the plugin's built-in classes so the feature
	// works out-of-the-box with no project assets required.
	GazeHighlightActorClass = AConvaiGazeHighlightActor::StaticClass();
	GazeCursorWidgetClass = UConvaiGazeCursorWidget::StaticClass();

	IsInit = false;
	VoiceCaptureRingBuffer.Init(ConvaiConstants::VoiceCaptureRingBufferCapacity);
	VoiceCaptureBuffer.Empty(ConvaiConstants::VoiceCaptureBufferSize);
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

	const FString CommandMicNoiseGateThreshold = "voice.MicNoiseGateThreshold  0.01";
	const FString CommandSilenceDetectionThreshold = "voice.SilenceDetectionThreshold 0.001";

	if (APlayerController* PController = UGameplayStatics::GetPlayerController(GetWorld(), 0))
	{
		PController->ConsoleCommand(*CommandMicNoiseGateThreshold, true);
		PController->ConsoleCommand(*CommandSilenceDetectionThreshold, true);
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

void UConvaiPlayerComponent::SetEndUserID(FString NewEndUserID)
{
	EndUserID = NewEndUserID;

	if (GetIsReplicated())
	{
		SetEndUserIDServer(EndUserID);
	}
}

void UConvaiPlayerComponent::SetEndUserIDServer_Implementation(const FString& NewEndUserID)
{
	EndUserID = NewEndUserID;
}

void UConvaiPlayerComponent::SetEndUserMetadata(FString NewEndUserMetadata)
{
	EndUserMetadata = NewEndUserMetadata;

	if (GetIsReplicated())
	{
		SetEndUserMetadataServer(EndUserMetadata);
	}
}

void UConvaiPlayerComponent::SetEndUserMetadataServer_Implementation(const FString& NewEndUserMetadata)
{
	EndUserMetadata = NewEndUserMetadata;
}

bool UConvaiPlayerComponent::GetDefaultCaptureDeviceInfo(FCaptureDeviceInfoBP& OutInfo)
{
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("GetDefaultCaptureDeviceInfo: AudioCaptureComponent is not valid"));
		return false;
	}

	Audio::FCaptureDeviceInfo OutDeviceInfo;
	if (!AudioCaptureComponent->GetDefaultCaptureDeviceInfo(OutDeviceInfo))
	{
		return false;
	}

	OutInfo.bSupportsHardwareAEC = OutDeviceInfo.bSupportsHardwareAEC;
	OutInfo.LongDeviceId = OutDeviceInfo.DeviceId;
	OutInfo.DeviceName = OutDeviceInfo.DeviceName;
	OutInfo.InputChannels = OutDeviceInfo.InputChannels;
	OutInfo.PreferredSampleRate = OutDeviceInfo.PreferredSampleRate;
	// The default device has no fixed position in the enumeration, and -1 is what
	// GetActiveCaptureDevice reports while the component is following the system default.
	OutInfo.DeviceIndex = -1;
	return true;
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

	if (ConvaiAudioCaptureComponent)
	{
		ConvaiAudioCaptureComponent->SetVolumeMultiplier(InVolumeMultiplier);
		Success = true;
		return;
	}

	if (IsValid(AudioCaptureComponent))
	{
		AudioCaptureComponent->SetVolumeMultiplier(InVolumeMultiplier);
		Success = true;
		return;
	}

	CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetMicrophoneVolumeMultiplier: No valid audio capture component found"));
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
		return;
	}
	OutVolumeMultiplier = InternalAudioComponent->VolumeMultiplier;
	Success = true;
}

bool UConvaiPlayerComponent::SaveMicrophoneSettings()
{
	UConvaiMicrophoneSubsystem* MicrophoneSubsystem = UConvaiMicrophoneSubsystem::Get(this);
	if (!MicrophoneSubsystem)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SaveMicrophoneSettings: ConvaiMicrophoneSubsystem is not available"));
		return false;
	}

	return MicrophoneSubsystem->SaveMicrophoneSettingsFromPlayerComponent(this);
}

bool UConvaiPlayerComponent::ApplySavedMicrophoneSettings()
{
	UConvaiMicrophoneSubsystem* MicrophoneSubsystem = UConvaiMicrophoneSubsystem::Get(this);
	if (!MicrophoneSubsystem)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("ApplySavedMicrophoneSettings: ConvaiMicrophoneSubsystem is not available"));
		return false;
	}

	return MicrophoneSubsystem->ApplySavedSettingsToPlayerComponent(this);
}

void UConvaiPlayerComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bEnableGazeAttention)
	{
		TickGazeAttention(DeltaTime);
	}
	else if (CurrentAttentionActor.IsValid() || CurrentlyGazedActor.IsValid() || ActiveHighlight.IsValid() || ActiveGazeCursor)
	{
		// Gaze tracking was on and got toggled off at runtime — tear down everything so the
		// highlight / cursor don't linger, and the chatbot's attention slot gets cleared.
		ReleaseCurrentAttention();
		DestroyActiveHighlight();
		DestroyGazeCursorWidget();
		CurrentlyGazedActor.Reset();
		CurrentlyGazedPrimitive.Reset();
		GazeAccumulator = 0.f;
		NoGazeAccumulator = 0.f;
	}

	if (!IsInit || !IsValid(AudioCaptureComponent))
	{
		return;
	}

	UpdateVoiceCapture(DeltaTime);
}

void UConvaiPlayerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ResetSpeakingStateSilently();

	// Make sure any gaze-driven attention is released before the player goes away — otherwise
	// the chatbot would keep CurrentAttentionObject pointing at something whose "owner" is dead.
	ReleaseCurrentAttention();
	DestroyActiveHighlight();
	DestroyGazeCursorWidget();
	CurrentlyGazedActor.Reset();
	CurrentlyGazedPrimitive.Reset();

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

	// Stop audio capture to prevent the WASAPI thread from accessing freed resources
	StopAudioCaptureComponent();

	Super::EndPlay(EndPlayReason);
}

void UConvaiPlayerComponent::RouteAdoptedCaptureComponent(UActorComponent* InAudioCaptureComponent)
{
	// Routing is assigned where the component is adopted, not where the default one
	// is constructed, so discovery and routing cannot come apart. They did: a
	// component supplied through IConvaiAudioCaptureInterface -- the documented
	// extension point -- was started with no SoundSubmix, so it rendered into the
	// Base Default submix and straight into the master mix, at -10.1 dB against the
	// -105.9 dB of the routed path. Audible through the game's speakers, and inside
	// Reference Audio, which is the signal the canceller subtracts (FINDINGS F19).
	//
	// StartVoiceChunkCapture records AudioCaptureComponent->SoundSubmix, so a
	// component that renders anywhere else is not captured at all. This submix is
	// not a preference, it is where the plugin listens.
	USynthComponent* Synth = Cast<USynthComponent>(InAudioCaptureComponent);
	if (!Synth)
	{
		// The interface says nothing about submixes, so a non-synth implementer is
		// legal and the plugin has no way to route it. Say so rather than leave the
		// silence that made this defect invisible.
		CONVAI_LOG(ConvaiPlayerLog, Warning,
			TEXT("Adopted capture component '%s' is not a USynthComponent, so its audio cannot be "
			     "routed to /ConvAI/Submixes/AudioInput. It will not be captured, and if it "
			     "renders at all it renders into the master mix."),
			*GetNameSafe(InAudioCaptureComponent));
		return;
	}

	if (_FoundSubmix == nullptr)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning,
			TEXT("Adopted capture component '%s' cannot be routed: the submix at "
			     "\"/ConvAI/Submixes/AudioInput\" was not found."),
			*GetNameSafe(InAudioCaptureComponent));
		return;
	}

	if (Synth->SoundSubmix == _FoundSubmix)
	{
		return;
	}

	if (Synth->SoundSubmix != nullptr)
	{
		// Deliberate on the third party's part, so it is not overwritten -- but the
		// plugin records AudioInput and nothing else, so their audio will not reach
		// the server.
		CONVAI_LOG(ConvaiPlayerLog, Warning,
			TEXT("Adopted capture component '%s' already renders into submix '%s', which is not "
			     "\"/ConvAI/Submixes/AudioInput\". Leaving it alone; its audio will not be "
			     "captured."),
			*GetNameSafe(InAudioCaptureComponent), *GetNameSafe(Synth->SoundSubmix));
		return;
	}

	Synth->SoundSubmix = _FoundSubmix;

	// Assigning is not routing. USynthComponent copies SoundSubmix into the sound it
	// plays, in Initialize and again in Start (SynthComponent.cpp:202, :480), and
	// Start returns immediately if the component is already active -- so a component
	// that was playing when it was adopted keeps the routing it started with, for
	// ever, while this property claims otherwise. That is the shape of a fix that
	// makes an assertion on the pointer pass and moves no audio: measured at
	// -9.9 dB into the master mix with the pointer correctly set, against -105.9 dB
	// once it is restarted.
	//
	// A third party's component is normally already rendering by the time discovery
	// reaches it, so this is the usual path and not the edge case.
	if (Synth->IsActive())
	{
		Synth->Stop();
		Synth->Start();
		CONVAI_LOG(ConvaiPlayerLog, Warning,
			TEXT("Adopted capture component '%s' was already playing, so it was restarted to "
			     "route it to \"/ConvAI/Submixes/AudioInput\". Until it is, its audio is not "
			     "captured and it is audible in the master mix."),
			*GetNameSafe(InAudioCaptureComponent));
		return;
	}

	CONVAI_LOG(ConvaiPlayerLog, Log,
		TEXT("Routed adopted capture component '%s' to \"/ConvAI/Submixes/AudioInput\""),
		*GetNameSafe(InAudioCaptureComponent));
}

IConvaiAudioCaptureInterface* UConvaiPlayerComponent::FindFirstAudioCaptureComponent()
{
	if (auto AudioCaptureComponents = GetOwner()->GetComponentsByInterface(UConvaiAudioCaptureInterface::StaticClass()); AudioCaptureComponents.Num() > 0)
	{
		SetAudioCaptureComponent(AudioCaptureComponents[0]);
	}
	return ConvaiAudioCaptureComponent.GetInterface();
}

bool UConvaiPlayerComponent::SetAudioCaptureComponent(UActorComponent* InAudioCaptureComponent)
{
	if (InAudioCaptureComponent && InAudioCaptureComponent->GetClass()->ImplementsInterface(UConvaiAudioCaptureInterface::StaticClass()))
	{
		ConvaiAudioCaptureComponent.SetObject(InAudioCaptureComponent);
		ConvaiAudioCaptureComponent.SetInterface(Cast<IConvaiAudioCaptureInterface>(InAudioCaptureComponent));
		
		if (ConvaiAudioCaptureComponent)
		{
			// The default component is created and registered in
			// OnComponentCreated, and a USynthComponent auto-activates on
			// registration -- so by the time BeginPlay adopts an alternative,
			// the host's capture device is already open and streaming. Both
			// components render into the submix StopVoiceChunkCapture records,
			// so leaving it running mixed the host's microphone into whatever
			// the alternative supplied and sent the sum to the server.
			if (IsValid(AudioCaptureComponent))
			{
				AudioCaptureComponent->Stop();
				CONVAI_LOG(ConvaiPlayerLog, Log,
					TEXT("Stopped default audio capture: an alternative was adopted"));
			}
			RouteAdoptedCaptureComponent(InAudioCaptureComponent);
			CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Set alternative audio capture component"));
			return true;
		}
	}
	else
	{
		ConvaiAudioCaptureComponent.SetObject(nullptr);
		ConvaiAudioCaptureComponent.SetInterface(nullptr);
	}
	return false;
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

void UConvaiPlayerComponent::StartVoiceChunkCapture(float ExpectedRecordingTime) const
{
	//GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Yellow, FString::Printf(TEXT("StartVoiceChunkCapture() in VoiceCaptureComp.cpp")));
	//CONVAI_LOG(LogTemp, Warning, TEXT("StartVoiceChunkCapture() in VoiceCaptureComp.cpp"));
	UAudioMixerBlueprintLibrary::StartRecordingOutput(this, ExpectedRecordingTime, Cast<USoundSubmix>(AudioCaptureComponent->SoundSubmix));
}

void UConvaiPlayerComponent::ReadRecordedBuffer(Audio::AlignedFloatBuffer& RecordedBuffer, float& OutNumChannels, float& OutSampleRate) const
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

void UConvaiPlayerComponent::EnsureMicrophonePermission()
{
#if PLATFORM_ANDROID
	if (!UConvaiAndroid::ConvaiAndroidHasMicrophonePermission())
	{
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Requesting RECORD_AUDIO permission"));
		UConvaiAndroid::ConvaiAndroidAskMicrophonePermission();
	}
#endif
}

void UConvaiPlayerComponent::StartAudioCaptureComponent()
{
	// Request the mic permission before opening the stream — on Android the open silently
	// yields a dead (all-silence) stream when RECORD_AUDIO isn't yet held.
	EnsureMicrophonePermission();

	if (ConvaiAudioCaptureComponent)
	{
		ConvaiAudioCaptureComponent->Start();
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Started alternative audio capture"));
		return;
	}

	AudioCaptureComponent->Start();
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Started default audio capture"));

	MicOpenRetryCount = 0;
	ScheduleMicOpenRetry();
}

void UConvaiPlayerComponent::ScheduleMicOpenRetry()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(MicOpenRetryTimer, this,
			&UConvaiPlayerComponent::CheckMicCaptureHealth, MicOpenRetryInterval, false);
	}
}

void UConvaiPlayerComponent::CheckMicCaptureHealth()
{
	if (!IsStreaming && !IsRecording)
		return;

	if (!IsValid(AudioCaptureComponent))
		return;

	if (AudioCaptureComponent->IsCaptureStreamHealthy())
	{
		MicOpenRetryCount = 0;
		ScheduleMicOpenRetry();
		return;
	}

	if (MicOpenRetryCount >= MicOpenMaxRetries)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("Microphone capture is not delivering audio after %d restart attempts - check that a microphone/input device is connected, that microphone permission is granted to this app, and that no other app holds the mic"), MicOpenRetryCount);
		return;
	}

	++MicOpenRetryCount;
	CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("Microphone capture not active (attempt %d/%d) - restarting capture"), MicOpenRetryCount, MicOpenMaxRetries);

	EnsureMicrophonePermission();
	AudioCaptureComponent->RestartCapture();

	ScheduleMicOpenRetry();
}

void UConvaiPlayerComponent::StopAudioCaptureComponent()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(MicOpenRetryTimer);
	}

	if (ConvaiAudioCaptureComponent)
	{
		ConvaiAudioCaptureComponent->Stop();
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Stopped alternative audio capture"));
	}

	if (IsValid(AudioCaptureComponent))
	{
		AudioCaptureComponent->Stop();
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Stopped default audio capture"));
	}
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
			VoiceCaptureBuffer.Append(reinterpret_cast<uint8*>(OutConverted.GetData()), OutConverted.Num() * sizeof(int16));
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

	// Recording while the conversation stream is live is fine: StopVoiceChunkCapture routes one
	// captured chunk into the recording buffer and the session through two independent branches.
	// Refusing here is what made the mic test a no-op whenever push-to-talk was off and the
	// stream was therefore always open.
	bMuteBeforeRecording = bMute;
	if (IsStreaming)
	{
		// The stream already owns the capture device and drives the chunk pump, so don't touch
		// either - just keep the recorded audio from reaching the character for the duration.
		bMute = true;
	}
	else
	{
		StartAudioCaptureComponent();    //Start the AudioCaptureComponent

		// reset audio buffers
		StartVoiceChunkCapture();
		StopVoiceChunkCapture();
	}

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

	// Only close the device if nothing else is using it. Checking IsStreaming now rather than
	// what it was at StartRecording also covers the stream being muted mid-recording, which
	// leaves us as the last user and so responsible for shutting the capture down.
	if (!IsStreaming)
	{
		StopAudioCaptureComponent();  //stop the AudioCaptureComponent
	}

	// Put bMute back exactly as the project had it, rather than assuming false.
	bMute = bMuteBeforeRecording;

	if (IsValid(OutSoundWave))
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("OutSoundWave->GetDuration(): %f seconds "), OutSoundWave->GetDuration());
	IsRecording = false;
	return OutSoundWave;
}

void UConvaiPlayerComponent::BeginPlay()
{
	// Before Super::BeginPlay(), which dispatches ReceiveBeginPlay - the Blueprint's Event
	// BeginPlay. BP_ConvaiPlayerComponent builds MicSettings_WB there, and the widget seeds its
	// device combo from GetActiveCaptureDevice during Event Construct. Restoring after Super would
	// leave SelectedDeviceIndex at -1 for that read, so the combo would show the system default
	// while the capture device was switched to the saved one a moment later.
	//
	// Safe this early: AudioCaptureComponent is created in OnComponentCreated, and with no stream
	// open yet SetCaptureDevice only records the index.
	if (!IsRunningDedicatedServer())
	{
		ApplySavedMicrophoneSettings();
	}

	Super::BeginPlay();

	EnsureMicrophonePermission();

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
	
	if (ConvaiAudioCaptureComponent == nullptr)
		FindFirstAudioCaptureComponent();
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

void UConvaiPlayerComponent::SetMute(const bool bNewMute)
{
	if (bMute == bNewMute)
	{
		return;
	}

	bMute = bNewMute;

	// Open mic: the soft-mute is local, and a game muting for a cutscene must not read as the
	// player finishing a sentence. Under push-to-talk the same flag is the button, so the
	// server has to hear about it.
	if (!IsPushToTalkEnabled() || !IsValid(SessionProxyInstance))
	{
		return;
	}

	if (bNewMute)
	{
		// Order matters: end the turn on audio the server already has, then stop listening.
		// This is what makes the button, rather than a pause for breath, end the player's turn.
		SessionProxyInstance->ForceUserStoppedSpeaking();
		SessionProxyInstance->ToggleSTT(true);
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Push To Talk released"));
	}
	else
	{
		SessionProxyInstance->ToggleSTT(false);
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Push To Talk pressed"));
	}
}

bool UConvaiPlayerComponent::IsPushToTalkEnabled() const
{
	static const FName PushToTalkPropertyName(TEXT("EnablePushToTalk"));
	
	if (const FBoolProperty* Switch = FindFProperty<FBoolProperty>(GetClass(), PushToTalkPropertyName))
	{
		return Switch->GetPropertyValue_InContainer(this);
	}
	else
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("IsPushToTalkEnabled: Could not find property %s, returning false"), *PushToTalkPropertyName.ToString());
	}
	return false;
}

float UConvaiPlayerComponent::ResolvePushToTalkStopSecs(const TArray<UConvaiPlayerComponent*>& PlayerComponents)
{
	for (const UConvaiPlayerComponent* Player : PlayerComponents)
	{
		if (IsValid(Player) && Player->IsPushToTalkEnabled())
		{
			return Player->PushToTalkStopSecs;
		}
	}
	return -1.0f;
}

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
		if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
		{
			ConvaiSubsystem->MarkExplicitDisconnect();
		}

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

bool UConvaiPlayerComponent::IsPlayerConnected() const
{
	return IsValid(SessionProxyInstance) && SessionProxyInstance->GetConnectionState() == EC_ConnectionState::Connected;
}

void UConvaiPlayerComponent::SendText(UConvaiConversationComponent* ChatbotComponent, const FString Text) const
{
	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->SendTextMessage(Text);

		if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
		{
			ConvaiSubsystem->MarkPlayerActivity();
		}
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

	const bool bPushToTalk = IsPushToTalkEnabled();

	// Push-to-talk opens the transport, not the ear. The capture device runs exactly as it does
	// for an open mic — the mic test, the levels and the AEC reference all depend on it — but
	// nothing reaches the character until StartPushToTalk.
	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->ToggleSTT(bPushToTalk);
	}

	if (bPushToTalk)
	{
		bMute = true;
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("Push To Talk is on: the character hears nothing until Mute is set to false. Set it false while your talk key is held and true on release, or turn off Enable Push To Talk."));
	}

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

	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->ToggleSTT(true);
	}

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Stopped Streaming Audio"));
}

void UConvaiPlayerComponent::OnConnectedToServer()
{
	
}

void UConvaiPlayerComponent::OnDisconnectedFromServer()
{
	// May arrive on the WebRTC transport thread; bIsSpeaking is game-thread-only.
	TWeakObjectPtr<UConvaiPlayerComponent> WeakSelf(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelf]
		{
			if (WeakSelf.IsValid())
			{
				WeakSelf->ResetSpeakingStateSilently();
			}
		});
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

void UConvaiPlayerComponent::BroadcastConnectionStateChanged(const FString& AttendeeId, EC_ConnectionState State)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiPlayerComponent> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, AttendeeId, State]()
		{
			if (UConvaiPlayerComponent* Component = WeakThis.Get())
			{
				Component->OnAttendeeConnectionStateChangedEvent.Broadcast(Component, AttendeeId, State);
			}
		});
		return;
	}

	// Already on game thread - broadcast directly
	OnAttendeeConnectionStateChangedEvent.Broadcast(this, AttendeeId, State);
}

void UConvaiPlayerComponent::OnAttendeeConnected(FString AttendeeId)
{
	BroadcastConnectionStateChanged(AttendeeId, EC_ConnectionState::Connected);
}

void UConvaiPlayerComponent::OnAttendeeDisconnected(FString AttendeeId)
{
	BroadcastConnectionStateChanged(AttendeeId, EC_ConnectionState::Disconnected);
}

// IConvaiConnectionInterface implementation
void UConvaiPlayerComponent::OnTranscriptionReceived(FString Transcription, bool IsTranscriptionReady, bool IsFinal)
{
	FString TrimmedTranscription = Transcription;
	TrimmedTranscription.TrimStartAndEndInline();
	const bool bHasText = !TrimmedTranscription.IsEmpty();
	const bool bHasContent = bHasText || IsFinal;
	if (!bHasContent)
		return;
		
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiPlayerComponent> WeakSelf(this);
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, Transcription, IsTranscriptionReady, IsFinal]
			{
				if (WeakSelf.IsValid())
				{
					WeakSelf->OnTranscriptionReceived(Transcription, IsTranscriptionReady, IsFinal);
				}
			});
		return;
	}

	// A non-empty partial is reliable evidence that an utterance is in progress,
	// even when the backend omitted user-started-speaking. It also cancels a
	// premature stop that split one continuously growing transcript.
	if (bHasText && !IsFinal)
	{
		OnStartedTalking();
	}

	// Handle transcription received from the server
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Transcription received: %s"), *Transcription);

	// Broadcast to any listeners
	OnTranscriptionReceivedDelegate.Broadcast(this, nullptr, Transcription, IsTranscriptionReady, IsFinal);

	// A real, non-empty final transcript is an authoritative utterance boundary.
	// Finish after broadcasting it so Blueprint observes Started -> transcript(s)
	// -> final transcript -> Finished. The empty final sentinel injected by
	// OnUserStoppedSpeaking is deliberately left to the short stop grace.
	if (bHasText && IsFinal)
	{
		FinishSpeakingNow();
	}
}

void UConvaiPlayerComponent::OnStartedTalking()
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiPlayerComponent> WeakSelf(this);
		AsyncTask(ENamedThreads::GameThread, [WeakSelf]
			{
				if (WeakSelf.IsValid())
				{
					WeakSelf->OnStartedTalking();
				}
			});
		return;
	}

	CancelPendingSpeakingStop();
	if (bIsSpeaking)
	{
		return;
	}

	// Handle started talking event
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Started talking"));

	bIsSpeaking = true;

	if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		ConvaiSubsystem->MarkPlayerActivity();
	}

	// Broadcast to any listeners
	OnStartedTalkingDelegate.Broadcast();
}

void UConvaiPlayerComponent::OnFinishedTalking()
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiPlayerComponent> WeakSelf(this);
		AsyncTask(ENamedThreads::GameThread, [WeakSelf]
			{
				if (WeakSelf.IsValid())
				{
					WeakSelf->OnFinishedTalking();
				}
			});
		return;
	}

	ScheduleSpeakingStop();
}

void UConvaiPlayerComponent::CancelPendingSpeakingStop()
{
	check(IsInGameThread());

	if (SpeakingStopTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(SpeakingStopTickerHandle);
		SpeakingStopTickerHandle.Reset();
	}
	bSpeakingStopPending = false;
}

void UConvaiPlayerComponent::ScheduleSpeakingStop()
{
	check(IsInGameThread());

	if (!bIsSpeaking || bSpeakingStopPending)
	{
		return;
	}

	bSpeakingStopPending = true;
	// Speech packets arrive in real time, independently of world pause and time
	// dilation. The core ticker follows that clock while still invoking us on
	// the game thread.
	SpeakingStopTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(
			this,
			&UConvaiPlayerComponent::HandleSpeakingStopGraceElapsed),
		SpeakingStopGraceSeconds);
}

bool UConvaiPlayerComponent::HandleSpeakingStopGraceElapsed(float /*DeltaTime*/)
{
	check(IsInGameThread());

	// The ticker is one-shot. Drop our weak handle before the transition so
	// FinishSpeakingNow does not try to remove the callback while it is running.
	SpeakingStopTickerHandle.Reset();
	FinishSpeakingNow();
	return false;
}

void UConvaiPlayerComponent::FinishSpeakingNow()
{
	check(IsInGameThread());

	CancelPendingSpeakingStop();
	if (!bIsSpeaking)
	{
		return;
	}

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Finished talking"));
	bIsSpeaking = false;

	if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		ConvaiSubsystem->MarkPlayerActivity();
	}

	OnFinishedTalkingDelegate.Broadcast();
}

void UConvaiPlayerComponent::ResetSpeakingStateSilently()
{
	check(IsInGameThread());
	CancelPendingSpeakingStop();
	bIsSpeaking = false;
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
	if (IsInGameThread())
	{
		ResetSpeakingStateSilently();
	}

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

void UConvaiPlayerComponent::SafeProcessAudioData(const int16* AudioData, int32 NumSamples, int32 SampleRate) const
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
	// bMute has to be honoured here as well as in StopVoiceChunkCapture: when audio processing is
	// active this is the path that reaches the session, so without it a muted mic - or a mic test
	// taken while streaming - would still be heard by the character.
	if (IsStreaming && !bMute) {
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

// ─────────────────────────────────────────────────────────────────────────────
// Gaze attention
// ─────────────────────────────────────────────────────────────────────────────

void UConvaiPlayerComponent::TickGazeAttention(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Resolve a view point. GetPlayerViewPoint returns the active POV camera, which is
	// the right thing for first-person, third-person, and VR (HMD pose). Falling back to
	// player-0 keeps the simple split-screen / single-player path working without forcing
	// the project to wire up a controller reference on this component.
	APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
	if (!PC)
	{
		return;
	}

	FVector ViewLoc;
	FRotator ViewRot;
	PC->GetPlayerViewPoint(ViewLoc, ViewRot);
	const FVector TraceEnd = ViewLoc + ViewRot.Vector() * GazeMaxDistance;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ConvaiGazeTrace), /*bTraceComplex=*/ false);
	// Ignore both the component's owner AND the controller's pawn — when the component lives
	// on the controller (rather than the pawn itself), Owner alone wouldn't filter out the
	// player's collision capsule, and a fat sphere trace would self-hit the pawn instantly.
	if (AActor* Owner = GetOwner())
	{
		Params.AddIgnoredActor(Owner);
	}
	if (APawn* OwningPawn = PC->GetPawn())
	{
		Params.AddIgnoredActor(OwningPawn);
	}

	FHitResult Hit;
	World->LineTraceSingleByChannel(Hit, ViewLoc, TraceEnd, GazeTraceChannel.GetValue(), Params);

	AActor* HitActor = Hit.GetActor();
	UPrimitiveComponent* HitPrim = Hit.GetComponent();

	// Track whether the primary line trace landed on something physical that *isn't* a Convai
	// actor (a wall, terrain, an enemy collision capsule, etc.). When that happens, the
	// player's eye-line is physically obstructed — falling back to dot-product would let us
	// engage Convai objects through the wall, which is almost never what the designer wants.
	bool bPrimaryHitBlockedByNonConvai = false;
	if (HitActor && Hit.bBlockingHit && !HitActor->FindComponentByClass<UConvaiObjectComponent>())
	{
		bPrimaryHitBlockedByNonConvai = true;
	}

	// Engagement check + match cache. Gathering once here lets the transition block below
	// reuse the result instead of re-gathering for the same (Actor, Primitive) pair. The
	// piggyback rule (component-scoped peers suppress whole-actor matches on non-scope
	// primitives) means a hit on a non-resolved primitive returns an empty set — we treat
	// that as "no engagement" and null the hit, same path as gazing at sky / non-Convai.
	TArray<UConvaiObjectComponent*> HitMatches;
	bool bHitAnyComponentScoped = false;
	if (HitActor)
	{
		GatherMatchingObjects(HitActor, HitPrim, HitMatches, bHitAnyComponentScoped);
		if (HitMatches.IsEmpty())
		{
			HitActor = nullptr;
			HitPrim = nullptr;
		}
	}

	// Dot-product fallback: when the strict line trace didn't engage anything, pick the
	// best-aligned Convai object inside a soft "gaze cone." Skip rules are deliberately cheap
	// so the loop stays microsecond-scale even with hundreds of candidates: bail on
	// out-of-range (squared distance), behind-the-camera (dot < 0), and out-of-cone
	// (dot ≤ cos(tolerance)) before any further work.
	//
	// Suppressed when the primary trace hit a real obstruction (wall / non-Convai geometry) —
	// otherwise we'd engage Convai objects through a physical occluder.
	if (!HitActor && GazeAngleTolerance > 0.0f && !bPrimaryHitBlockedByNonConvai)
	{
		FallbackEngageViaDotProduct(ViewLoc, ViewRot.Vector(), HitActor, HitPrim);
		if (HitActor)
		{
			// Re-validate via the same GatherMatchingObjects rules the transition block uses.
			// Fallback picks the best-aligned Convai object component and synthesizes a hit
			// from its owner + resolved component, but an actor with a mix of whole-actor and
			// component-scoped ObjComps could still produce an empty match set under the
			// piggyback rule (e.g. the synthesized HitPrim is null because the chosen ObjComp
			// is whole-actor, but the actor has unrelated component-scoped peers that suppress
			// the whole-actor match on a null primitive). If empty, revert to "no hit" so we
			// don't highlight or promote on a synthesized pair the pipeline rejects.
			GatherMatchingObjects(HitActor, HitPrim, HitMatches, bHitAnyComponentScoped);
			if (HitMatches.IsEmpty())
			{
				HitActor = nullptr;
				HitPrim = nullptr;
			}
		}
	}

	// Lazy-create cursor widget on the first tick where gaze tracking is on; lifecycle is
	// tied to bShowGazeCursor + EndPlay so we don't pay UMG cost when the feature is off.
	if (bShowGazeCursor)
	{
		EnsureGazeCursorWidget();
	}
	else if (ActiveGazeCursor)
	{
		// Cursor was on and got toggled off — remove the widget from the viewport so it
		// doesn't linger as a frozen dot on screen.
		DestroyGazeCursorWidget();
	}

	// Detect the destroyed-target transition: if we WERE tracking something and the weak ptr
	// is now stale, force the transition path even though HitActor may also be null (the
	// `HitActor != Current.Get()` test alone wouldn't catch nullptr == nullptr).
	const bool bGazeTargetGone =
		(!CurrentlyGazedActor.IsExplicitlyNull() && !CurrentlyGazedActor.IsValid()) ||
		(!CurrentlyGazedPrimitive.IsExplicitlyNull() && !CurrentlyGazedPrimitive.IsValid());

	if (HitActor != CurrentlyGazedActor.Get() || HitPrim != CurrentlyGazedPrimitive.Get() || bGazeTargetGone)
	{
		// 1) Gaze target changed — notify matching ObjComps on the OLD pair that gaze ended,
		//    then swap, then notify matching on the NEW pair that gaze began.
		if (AActor* PrevActor = CurrentlyGazedActor.Get())
		{
			TArray<UConvaiObjectComponent*> PrevMatches;
			bool bUnusedScoped = false;
			GatherMatchingObjects(PrevActor, CurrentlyGazedPrimitive.Get(), PrevMatches, bUnusedScoped);
			// [GAZE-DEBUG] Verbose; bump back to Log when debugging gaze transitions.
			CONVAI_LOG(ConvaiPlayerLog, Verbose,
				TEXT("[Gaze] Stopped gazing at actor=%s prim=%s (matches=%d)"),
				*PrevActor->GetName(),
				CurrentlyGazedPrimitive.IsValid() ? *CurrentlyGazedPrimitive->GetName() : TEXT("none"),
				PrevMatches.Num());
			for (UConvaiObjectComponent* Obj : PrevMatches)
			{
				if (!IsValid(Obj))
				{
					continue;
				}
				Obj->NotifyGazeEnd(this);
				OnGazeEnd.Broadcast(this, Obj);
			}
		}

		DestroyActiveHighlight();
		CurrentlyGazedActor = HitActor;
		CurrentlyGazedPrimitive = HitPrim;
		GazeAccumulator = 0.f;

		if (HitActor)
		{
			// Reuse the matches we already gathered for the engagement pre-check — saves a
			// duplicate scan of HitActor's components on every gaze-target transition.

			// [GAZE-DEBUG] Verbose; bump back to Log when debugging gaze transitions.
			CONVAI_LOG(ConvaiPlayerLog, Verbose,
				TEXT("[Gaze] Began gazing at actor=%s prim=%s (matches=%d, anyComponentScoped=%d)"),
				*HitActor->GetName(),
				HitPrim ? *HitPrim->GetName() : TEXT("none"),
				HitMatches.Num(), bHitAnyComponentScoped ? 1 : 0);

			// If any matching ObjComp is component-scoped, the highlight follows the
			// specific hit primitive — that's the user-requested rule. Otherwise (all
			// matches are whole-actor scope) the highlight covers the whole actor.
			USceneComponent* HighlightScope = bHitAnyComponentScoped ? Cast<USceneComponent>(HitPrim) : nullptr;

			// Collective highlight: for whole-actor scope, expand each hit Convai object into
			// its merged set and highlight every distinct owning actor, so gazing at one crate
			// of a pile lights them all. A hit object can opt out with "Highlight Only This
			// Object When Gazed" (purely cosmetic — attention + merging are unaffected).
			// Component-scoped hits stay single-actor (the silhouette tracks one sub-mesh).
			TArray<AActor*> HighlightActors;
			bool bSoloHighlight = false;
			for (UConvaiObjectComponent* Obj : HitMatches)
			{
				if (IsValid(Obj) && Obj->bHighlightOnlyThisWhenGazed) { bSoloHighlight = true; break; }
			}
			if (HighlightScope || bSoloHighlight)
			{
				HighlightActors.Add(HitActor);
			}
			else if (UConvaiSubsystem* HlSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
			{
				for (UConvaiObjectComponent* Obj : HitMatches)
				{
					TArray<UConvaiObjectComponent*> Members;
					HlSubsystem->GetObjectGroupMembers(Obj, Members);
					for (UConvaiObjectComponent* Member : Members)
					{
						if (IsValid(Member) && IsValid(Member->GetOwner()))
						{
							HighlightActors.AddUnique(Member->GetOwner());
						}
					}
				}
			}
			if (HighlightActors.Num() == 0)
			{
				HighlightActors.Add(HitActor);
			}
			SpawnHighlightFor(HighlightActors, HighlightScope);

			for (UConvaiObjectComponent* Obj : HitMatches)
			{
				if (!IsValid(Obj))
				{
					continue;
				}
				Obj->NotifyGazeBegin(this);
				OnGazeBegin.Broadcast(this, Obj);
			}
		}

		if (ActiveGazeCursor)
		{
			// bAlwaysShowGazeCursor forces the active state even when gaze is on nothing —
			// for projects that want a persistent reticle that doesn't fade with engagement.
			ActiveGazeCursor->SetGazeActive(bAlwaysShowGazeCursor || HitActor != nullptr);
		}
	}

	// 2) Escalation — sustained gaze on the same (Actor, Primitive) promotes it to attention.
	if (const bool bSameAsCurrentlyGazed = (HitActor && HitActor == CurrentlyGazedActor.Get() && HitPrim == CurrentlyGazedPrimitive.Get()))
	{
		GazeAccumulator += DeltaTime;
		if (const bool bAlreadyAttention = (HitActor == CurrentAttentionActor.Get() && HitPrim == CurrentAttentionPrimitive.Get()); GazeAccumulator >= GazeAttentionDelay && !bAlreadyAttention)
		{
			// Promote with HitPrim directly (not a scope-derived value). The "is this already
			// the attention pair?" check above compares HitPrim to CurrentAttentionPrimitive,
			// so storing the hit primitive lets the comparison succeed on subsequent ticks
			// regardless of whether the highlight scope was narrow-component or whole-actor.
			// The highlight scope decision was already made in step 1; PromoteToAttention only
			// needs the (Actor, Primitive) pair for its matching-object fan-out.
			PromoteToAttention(HitActor, HitPrim);
		}
	}

	// 3) Loss tracking — count down to release when the gaze has moved off the current
	//    attention pair. The piggyback rule above nulls HitActor when the player gazes at a
	//    non-resolved primitive of an actor with component-scoped ObjComps (e.g. the Cube on
	//    a "Cone only" actor), so this timer ONLY accumulates when the player is genuinely
	//    looking at "nothing engaged" — never at a primitive that happens to live on the
	//    attended actor but doesn't match its scope.
	const bool bSameAsCurrentAttention = (HitActor == CurrentAttentionActor.Get() && HitPrim == CurrentAttentionPrimitive.Get());
	if (CurrentAttentionActor.IsValid() && !bSameAsCurrentAttention)
	{
		NoGazeAccumulator += DeltaTime;
		if (NoGazeAccumulator >= GazeAttentionLossDelay)
		{
			ReleaseCurrentAttention();
		}
	}
	else
	{
		// Either gaze is back on the current attention pair, or nothing is attended — reset.
		NoGazeAccumulator = 0.f;
	}
}

void UConvaiPlayerComponent::GatherMatchingObjects(AActor* Actor, UPrimitiveComponent* HitComponent,
	TArray<UConvaiObjectComponent*>& OutMatches, bool& bOutAnyComponentScoped) const
{
	OutMatches.Reset();
	bOutAnyComponentScoped = false;
	if (!Actor)
	{
		return;
	}

	TArray<UConvaiObjectComponent*> AllObjs;
	Actor->GetComponents<UConvaiObjectComponent>(AllObjs);

	// Partition by scope type. Defining a component-scoped ObjComp on an actor is treated as
	// a declaration that only those specific sub-components are gaze-interesting; whole-actor
	// ObjComps on the same actor become piggyback-only (they fire alongside a component-scoped
	// match but don't engage on hits that don't match any component scope).
	//
	// IMPORTANT: an ObjComp with a ComponentName filter that *failed to resolve* (substring
	// didn't match) doesn't count as "component-scoped" — including it in ComponentObjs
	// would mean its mere presence on the actor suppresses whole-actor peer matches (via
	// the "ComponentObjs non-empty + no match = silent" branch below). Treat unresolved-
	// filter ObjComps as ignored entirely; the warning about the unresolved name already
	// fires inside FConvaiObjectEntry::ResolveComponent.
	TArray<UConvaiObjectComponent*> WholeActorObjs;
	TArray<UConvaiObjectComponent*> ComponentObjs;
	for (UConvaiObjectComponent* Obj : AllObjs)
	{
		if (!IsValid(Obj) || !Obj->bGazeable)
		{
			// bGazeable=false makes the object invisible to gaze entirely — no highlight,
			// no events, no attention promotion. Use case: scenery / props that should still
			// appear in the chatbot's environment for narrative reference but shouldn't be
			// interactable via player gaze.
			continue;
		}
		if (Obj->ObjectEntry.HasComponentFilters())
		{
			if (Obj->GetResolvedComponent() != nullptr)
			{
				ComponentObjs.Add(Obj);
			}
			// else: filter set but unresolved → ignored (already logged at resolve time)
		}
		else
		{
			WholeActorObjs.Add(Obj);
		}
	}

	// Compute the set of component-scoped ObjComps that actually match this hit. Resolved is
	// guaranteed non-null for every entry in ComponentObjs (we partitioned that way above).
	TArray<UConvaiObjectComponent*> ComponentMatches;
	for (UConvaiObjectComponent* Obj : ComponentObjs)
	{
		USceneComponent* Resolved = Obj->GetResolvedComponent();
		if (Resolved == HitComponent ||
			(HitComponent && HitComponent->IsAttachedTo(Resolved)))
		{
			ComponentMatches.Add(Obj);
		}
	}

	if (ComponentObjs.Num() == 0)
	{
		// Pure whole-actor case — every whole-actor ObjComp matches any hit on this actor.
		OutMatches = WholeActorObjs;
		return;
	}

	if (ComponentMatches.Num() == 0)
	{
		// Actor has component-scoped ObjComps but the hit didn't match any — silent.
		// Whole-actor peers don't engage on non-matching primitives by design.
		return;
	}

	// Mixed case: at least one component-scoped match. Return component matches + the
	// whole-actor peers piggybacking. Component matches go first so the highlight scope
	// pick below (any-component-scoped → HitPrim) is consistent.
	OutMatches.Reserve(ComponentMatches.Num() + WholeActorObjs.Num());
	OutMatches.Append(ComponentMatches);
	OutMatches.Append(WholeActorObjs);
	bOutAnyComponentScoped = true;
}

void UConvaiPlayerComponent::FallbackEngageViaDotProduct(const FVector& ViewLoc, const FVector& ViewDir,
	AActor*& OutHitActor, UPrimitiveComponent*& OutHitPrim) const
{
	UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this);
	if (!Subsystem)
	{
		return;
	}

	// Candidate set is just the subsystem-wide pool — every ObjComp (designer-placed or
	// chatbot-auto-spawned) registers there now, so no per-chatbot union is needed.
	TSet<UConvaiObjectComponent*> Candidates;
	for (UConvaiObjectComponent* O : Subsystem->GetAllObjectComponents())
	{
		if (IsValid(O))
		{
			Candidates.Add(O);
		}
	}

	// Pre-compute the skip thresholds. cos(tolerance) gives us a single floating-point
	// comparison per object instead of trig in the loop. Squared max distance saves a sqrt
	// on every candidate that's clearly out of range.
	const float ToleranceCos = FMath::Cos(FMath::DegreesToRadians(GazeAngleTolerance));
	const float MaxDistSq = GazeMaxDistance * GazeMaxDistance;
	const AActor* OwnerActor = GetOwner();
	const APawn* OwnerPawn = nullptr;
	if (UWorld* W = GetWorld())
	{
		if (APlayerController* PC = UGameplayStatics::GetPlayerController(W, 0))
		{
			OwnerPawn = PC->GetPawn();
		}
	}

	UConvaiObjectComponent* Best = nullptr;
	float BestDot = ToleranceCos; // must strictly beat this to win

	for (UConvaiObjectComponent* Obj : Candidates)
	{
		if (!Obj->bGazeable)
		{
			// Opt-out — the user marked this object as non-gazeable.
			continue;
		}
		AActor* ObjOwner = Obj->GetOwner();
		if (!ObjOwner || ObjOwner == OwnerActor || ObjOwner == OwnerPawn)
		{
			// Filter out the player's own actor / pawn so the fallback can't self-engage.
			continue;
		}

		// Use the resolved component's world location when there's a component filter, else
		// the actor's origin — keeps "looking at the Handle" pick correctly toward the Handle
		// even when whole-actor would pull the math toward the actor pivot.
		USceneComponent* Resolved = Obj->GetResolvedComponent();
		const FVector ObjLoc = Resolved ? Resolved->GetComponentLocation() : ObjOwner->GetActorLocation();

		const FVector ToObj = ObjLoc - ViewLoc;
		const float DistSq = ToObj.SquaredLength();
		if (DistSq > MaxDistSq || DistSq <= KINDA_SMALL_NUMBER)
		{
			continue; // out of gaze range, or coincident with the camera
		}

		// Cheap hemisphere check before the sqrt — anything behind the camera is dot < 0.
		const float ToObjDotView = FVector::DotProduct(ToObj, ViewDir);
		if (ToObjDotView <= 0.0f)
		{
			continue;
		}

		// Normalize and compute the alignment dot. Now we pay the sqrt, but only for
		// candidates that passed both the range and hemisphere fast-fails.
		const float InvDist = FMath::InvSqrt(DistSq);
		const float Dot = ToObjDotView * InvDist;
		if (Dot <= BestDot)
		{
			continue; // outside the cone, or worse than a previous candidate
		}

		// TODO(off-axis LOS): the caller suppresses fallback when the *center* line trace hits
		// a non-Convai occluder, but a candidate sitting off-axis can still be behind a
		// different wall while the center ray is clear. Add a per-candidate visibility trace
		// here (ViewLoc → ObjLoc on GazeTraceChannel, ignore OwnerActor/OwnerPawn) and skip the
		// candidate on a blocking non-Convai hit. Doing it after the cone test keeps the trace
		// cost bounded to actually-plausible candidates.
		BestDot = Dot;
		Best = Obj;
	}

	if (!Best)
	{
		return;
	}

	// Synthesize the hit: HitActor = winner's owning actor, HitPrim = its resolved component
	// when that's a UPrimitiveComponent (gives component-scoped matching downstream) or null
	// for whole-actor scope (matches every whole-actor ObjComp on this actor naturally).
	OutHitActor = Best->GetOwner();
	OutHitPrim = Cast<UPrimitiveComponent>(Best->GetResolvedComponent());
}

void UConvaiPlayerComponent::SpawnHighlightFor(const TArray<AActor*>& TargetActors, USceneComponent* TargetComponent)
{
	AActor* PrimaryActor = nullptr;
	for (AActor* A : TargetActors)
	{
		if (IsValid(A)) { PrimaryActor = A; break; }
	}
	if (!PrimaryActor)
	{
		return;
	}

	UWorld* World = GetWorld();
	TSubclassOf<AConvaiGazeHighlightActor> Class = GazeHighlightActorClass;
	if (!Class)
	{
		Class = AConvaiGazeHighlightActor::StaticClass();
	}
	if (!World || !Class)
	{
		return;
	}

	FActorSpawnParameters Spawn;
	Spawn.Owner = GetOwner();
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AConvaiGazeHighlightActor* Highlight = World->SpawnActor<AConvaiGazeHighlightActor>(
		Class, PrimaryActor->GetActorLocation(), FRotator::ZeroRotator, Spawn);
	if (!Highlight)
	{
		return;
	}
	Highlight->HighlightColor = GazeHighlightColor;
	Highlight->EmissiveIntensity = GazeHighlightEmissiveIntensity;
	// Only override the highlight actor's own default if the player component is pointing at
	// something specific — leaves the actor free to use whatever it defaulted to in its ctor
	// (the plugin Fresnel material) when GazeOverlayMaterial is unset.
	if (!GazeOverlayMaterial.IsNull())
	{
		Highlight->OverlayMaterial = GazeOverlayMaterial;
	}
	// TargetComponent is non-null only when at least one matching UConvaiObjectComponent is
	// component-scoped — the highlight then stamps only that sub-component instead of the
	// whole actor's meshes (door's "Handle" silhouette, not the whole door). Multiple actors
	// are passed for a merged set so they light up collectively.
	Highlight->SetTargets(TargetActors, TargetComponent);
	ActiveHighlight = Highlight;
}

void UConvaiPlayerComponent::DestroyActiveHighlight()
{
	if (AConvaiGazeHighlightActor* Highlight = ActiveHighlight.Get())
	{
		Highlight->Destroy();
	}
	ActiveHighlight.Reset();
}

void UConvaiPlayerComponent::PromoteToAttention(AActor* NewActor, UPrimitiveComponent* NewPrimitive)
{
	if (!NewActor)
	{
		return;
	}

	// If we already had an attention target, tell it (and through it, the chatbots) that
	// it's no longer the focus, before swapping in the new one. Order matters: the chatbot
	// may key off seeing a clear-then-set pair, and we want the old target's OnDestroyed
	// binding gone before we wire up a new one.
	ReleaseCurrentAttention();

	CurrentAttentionActor = NewActor;
	CurrentAttentionPrimitive = NewPrimitive;
	NoGazeAccumulator = 0.f;

	// Dynamic-multicast bind so we automatically clear if the target actor is destroyed
	// out from under us mid-conversation.
	NewActor->OnDestroyed.AddDynamic(this, &UConvaiPlayerComponent::HandleAttentionTargetDestroyed);

	// Fan out to every matching UConvaiObjectComponent on the new (Actor, Primitive) pair —
	// each one drives its own chatbot fan-out via the subsystem pool.
	TArray<UConvaiObjectComponent*> Matches;
	bool bUnusedScoped = false;
	GatherMatchingObjects(NewActor, CurrentAttentionPrimitive.Get(), Matches, bUnusedScoped);
	// [GAZE-DEBUG] Verbose; bump back to Log when debugging gaze transitions.
	CONVAI_LOG(ConvaiPlayerLog, Verbose,
		TEXT("[Gaze] Promoting attention -> actor=%s prim=%s (matchingObjComps=%d)"),
		*NewActor->GetName(),
		NewPrimitive ? *NewPrimitive->GetName() : TEXT("<none>"),
		Matches.Num());

	// Snapshot the ObjectEntries — HandleAttentionTargetDestroyed needs them to fan-out a
	// clear to chatbots after the underlying UConvaiObjectComponents are torn down with the
	// target actor. Cleared in ReleaseCurrentAttention.
	CurrentAttentionEntries.Reset();
	CurrentAttentionEntries.Reserve(Matches.Num());

	for (UConvaiObjectComponent* Obj : Matches)
	{
		if (!IsValid(Obj))
		{
			continue;
		}
		CurrentAttentionEntries.Add(Obj->ObjectEntry);
		Obj->NotifyGazeAttentionBegin(this, GazeAttentionText, GazeShouldRespond, GazeDelivery);
		OnAttentionGained.Broadcast(this, Obj);
	}
}

void UConvaiPlayerComponent::ReleaseCurrentAttention()
{
	AActor* Attention = CurrentAttentionActor.Get();
	if (!Attention)
	{
		CurrentAttentionActor.Reset();
		CurrentAttentionPrimitive.Reset();
		return;
	}

	Attention->OnDestroyed.RemoveDynamic(this, &UConvaiPlayerComponent::HandleAttentionTargetDestroyed);

	// Notify each ObjComp that was in scope for the OLD attention pair — fan out clears to
	// the chatbots so the gaze-attention slot is released wherever it was set.
	TArray<UConvaiObjectComponent*> Matches;
	bool bUnusedScoped = false;
	GatherMatchingObjects(Attention, CurrentAttentionPrimitive.Get(), Matches, bUnusedScoped);
	// [GAZE-DEBUG] Verbose; bump back to Log when debugging gaze transitions.
	CONVAI_LOG(ConvaiPlayerLog, Verbose,
		TEXT("[Gaze] Releasing attention <- actor=%s prim=%s (matchingObjComps=%d)"),
		*Attention->GetName(),
		CurrentAttentionPrimitive.IsValid() ? *CurrentAttentionPrimitive->GetName() : TEXT("<whole-actor>"),
		Matches.Num());
	for (UConvaiObjectComponent* Obj : Matches)
	{
		if (!IsValid(Obj))
		{
			continue;
		}
		Obj->NotifyGazeAttentionEnd(this);
		OnAttentionLost.Broadcast(this, Obj);
	}

	CurrentAttentionActor.Reset();
	CurrentAttentionPrimitive.Reset();
	CurrentAttentionEntries.Reset();
	NoGazeAccumulator = 0.f;
}

void UConvaiPlayerComponent::HandleAttentionTargetDestroyed(AActor* DestroyedActor)
{
	// [GAZE-DEBUG] Verbose; bump back to Log when debugging the destroy-clear fan-out.
	CONVAI_LOG(ConvaiPlayerLog, Verbose,
		TEXT("[Gaze] Attention target destroyed: %s — clearing %d cached entries from %d chatbots"),
		DestroyedActor ? *DestroyedActor->GetName() : TEXT("<unknown>"),
		CurrentAttentionEntries.Num(),
		UConvaiUtils::GetConvaiSubsystem(this) ? UConvaiUtils::GetConvaiSubsystem(this)->GetAllChatbotComponents().Num() : 0);

	// The UConvaiObjectComponents on the destroyed actor are gone too, so we can't call
	// their NotifyGazeAttentionEnd. Instead, fan out TryClearObjectInAttentionFromGaze to
	// every chatbot in the subsystem using the entry snapshot captured at PromoteToAttention
	// — each chatbot's TryClear matches by Name and silently no-ops on chatbots that don't
	// currently hold gaze attention on this entry.
	if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		for (UConvaiChatbotComponent* Chatbot : Subsystem->GetAllChatbotComponents())
		{
			if (!IsValid(Chatbot))
			{
				continue;
			}
			for (const FConvaiObjectEntry& Entry : CurrentAttentionEntries)
			{
				Chatbot->TryClearObjectInAttentionFromGaze(Entry);
			}
		}
	}

	CurrentAttentionActor.Reset();
	CurrentAttentionPrimitive.Reset();
	CurrentAttentionEntries.Reset();
	NoGazeAccumulator = 0.f;

	// Fire the loss event with a null ObjectComponent so listeners can react to the destroy
	// case (they have to be null-checking the param anyway since the component is gone).
	OnAttentionLost.Broadcast(this, nullptr);

	// Also clear the "currently gazed" pair + highlight if they pointed at the same actor —
	// they almost always do.
	if (!CurrentlyGazedActor.IsValid())
	{
		DestroyActiveHighlight();
		CurrentlyGazedActor.Reset();
		CurrentlyGazedPrimitive.Reset();
		GazeAccumulator = 0.f;
		if (ActiveGazeCursor)
		{
			ActiveGazeCursor->SetGazeActive(bAlwaysShowGazeCursor);
		}
	}
}

void UConvaiPlayerComponent::EnsureGazeCursorWidget()
{
	if (ActiveGazeCursor)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Only spawn the cursor for the local player — server / non-local pawns don't have a
	// viewport, and AddToViewport on a non-local widget is wasted work / a warning.
	APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
	if (!PC || !PC->IsLocalController())
	{
		return;
	}

	TSubclassOf<UConvaiGazeCursorWidget> Class = GazeCursorWidgetClass;
	if (!Class)
	{
		Class = UConvaiGazeCursorWidget::StaticClass();
	}

	ActiveGazeCursor = CreateWidget<UConvaiGazeCursorWidget>(PC, Class);
	if (!ActiveGazeCursor)
	{
		return;
	}
	ActiveGazeCursor->ActiveColor = GazeCursorActiveColor;
	ActiveGazeCursor->IdleColor = GazeCursorIdleColor;
	ActiveGazeCursor->DotSize = GazeCursorDotSize;
	ActiveGazeCursor->FadeInTime = GazeCursorFadeInTime;
	ActiveGazeCursor->FadeOutTime = GazeCursorFadeOutTime;
	ActiveGazeCursor->AddToViewport();
	ActiveGazeCursor->SetGazeActive(bAlwaysShowGazeCursor || CurrentlyGazedActor.IsValid());
}

void UConvaiPlayerComponent::DestroyGazeCursorWidget()
{
	if (ActiveGazeCursor)
	{
		ActiveGazeCursor->RemoveFromParent();
		ActiveGazeCursor = nullptr;
	}
}

