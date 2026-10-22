// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiPlayerComponent.h"
#if __has_include("ISubmixBufferListener.h")
#include "ISubmixBufferListener.h"
#else
#include "AudioDevice.h"
#endif
#include "Utility/ConvaiEngineCompat.h"
#include "Utility/ConvaiPcmReblocker.h"
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
	// Mic health check cadence, indexed by consecutive restarts: every second while healthy, then
	// backing off for a device that stays dead, whose every reopen costs a long game-thread frame. It
	// never gives up, so a device plugged in later is still found within half a minute.
	constexpr float MicCheckBackoffSeconds[] = {1.f, 1.f, 2.f, 5.f, 10.f, 20.f, 30.f};

	// Not APawn::IsLocallyControlled: an AI controller counts as local wherever it runs.
	bool IsPossessedByLocalPlayer(const APawn& Pawn)
	{
		const APlayerController* Controller = Cast<APlayerController>(Pawn.GetController());
		return Controller && Controller->IsLocalController();
	}
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

	// One enumeration serves the check and the selection: each is a full device scan on the game thread.
	const TArray<Audio::FCaptureDeviceInfo> Devices = AudioCaptureComponent->GetCaptureDevicesAvailable();
	if (!Devices.IsValidIndex(DeviceIndex))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetCaptureDeviceByIndex: Invalid Device Index: %d - Max possible index: %d."), DeviceIndex, Devices.Num() - 1);
		return false;
	}

	return AudioCaptureComponent->SelectCaptureDevice(Devices[DeviceIndex], DeviceIndex);
}

bool UConvaiPlayerComponent::SetCaptureDeviceByName(FString DeviceName)
{
	if (!IsValid(AudioCaptureComponent))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetCaptureDeviceByName: AudioCaptureComponent is not valid"));
		return false;
	}

	// One enumeration serves the lookup, the selection and the error message.
	const TArray<Audio::FCaptureDeviceInfo> Devices = AudioCaptureComponent->GetCaptureDevicesAvailable();
	const int32 DeviceIndex = Devices.FindLastByPredicate([&DeviceName](const Audio::FCaptureDeviceInfo& Device)
	{
		return Device.DeviceName == DeviceName;
	});

	const auto AvailableDeviceNames = [&Devices]
	{
		TArray<FString> Names;
		for (const Audio::FCaptureDeviceInfo& Device : Devices)
		{
			Names.Add(Device.DeviceName);
		}
		return FString::Join(Names, TEXT(" - "));
	};

	if (DeviceIndex == INDEX_NONE)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetCaptureDeviceByName: Could not find Device name: %s - Available Device names are: [%s]."), *DeviceName, *AvailableDeviceNames());
		return false;
	}

	if (!AudioCaptureComponent->SelectCaptureDevice(Devices[DeviceIndex], DeviceIndex))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetCaptureDeviceByName: SetCaptureDeviceByIndex failed for index: %d and device name: %s - Available Device names are: [%s]."), DeviceIndex, *DeviceName, *AvailableDeviceNames());
		return false;
	}
	return true;
}

void UConvaiPlayerComponent::SetMicrophoneVolumeMultiplier(float InVolumeMultiplier, bool& Success)
{
	Success = false;
	MicVolumeMultiplier = InVolumeMultiplier;

	if (IConvaiAudioCaptureInterface* Adopted = GetAdoptedCapture(true))
	{
		Adopted->SetVolumeMultiplier(InVolumeMultiplier);
		Success = true;
	}

	// The built-in device keeps the gain as well, for when it takes the microphone back.
	if (IsValid(AudioCaptureComponent))
	{
		AudioCaptureComponent->SetVolumeMultiplier(InVolumeMultiplier);
		Success = true;
	}

	if (!Success)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("SetMicrophoneVolumeMultiplier: No valid audio capture component found"));
	}
}

void UConvaiPlayerComponent::GetMicrophoneVolumeMultiplier(float& OutVolumeMultiplier, bool& Success)
{
	Success = false;
	if (MicVolumeMultiplier.IsSet() && GetAdoptedCapture(false))
	{
		// What the adoptee plays at, so a save stores what the player hears.
		OutVolumeMultiplier = MicVolumeMultiplier.GetValue();
		Success = true;
		return;
	}
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

void UConvaiPlayerComponent::ApplySavedMicrophoneGain(const float Gain)
{
	// Once a gain was chosen, applying the saved one is a revert and goes everywhere.
	if (!MicVolumeMultiplier.IsSet() && FMath::IsNearlyEqual(Gain, FConvaiMicrophoneSettings().InputGain, 0.001f))
	{
		if (IsValid(AudioCaptureComponent))
		{
			AudioCaptureComponent->SetVolumeMultiplier(Gain);
		}
		return;
	}
	bool bApplied = false;
	SetMicrophoneVolumeMultiplier(Gain, bApplied);
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

	if (bAutoStartPending)
	{
		AutoStartSession();
	}
	else if (bAutoInitializeSession)
	{
		TakeMicrophoneOnPossession();
	}

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
	bEndingPlay = true;
	ResetSpeakingStateSilently();

	// Make sure any gaze-driven attention is released before the player goes away — otherwise
	// the chatbot would keep CurrentAttentionObject pointing at something whose "owner" is dead.
	ReleaseCurrentAttention();
	DestroyActiveHighlight();
	DestroyGazeCursorWidget();
	CurrentlyGazedActor.Reset();
	CurrentlyGazedPrimitive.Reset();

	// Before the stops below, which can drain a last chunk.
	DetachAudioProcessing();

	// Unregister from the ConvaiSubsystem and unbind connection state changes
	if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		ConvaiSubsystem->UnregisterPlayerComponent(this);
		ConvaiSubsystem->OnServerConnectionStateChangedEvent.RemoveDynamic(this, &UConvaiPlayerComponent::OnServerConnectionStateChanged);
	}
	
	if (IsRecording)
		EndRecording();

	if (IsStreaming)
		MuteStreamingAudio();

	// Shut down any active session
	if (IsValid(SessionProxyInstance))
	{
		StopSession();
	}

	// Stop audio capture to prevent the WASAPI thread from accessing freed resources
	StopAudioCaptureComponent();
	// After MuteStreamingAudio the tick no longer reaches UpdateVoiceCapture's
	// unregister branch, and a registered tap would call into a dead component
	// from the render thread.
	UnregisterMicTap();

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
	// A class that implements the interface only in Blueprint has no native side to call, so it
	// clears the adoptee like any other non-implementer.
	UActorComponent* Incoming = Cast<IConvaiAudioCaptureInterface>(InAudioCaptureComponent) ? InAudioCaptureComponent : nullptr;
	SwitchCaptureSource(Incoming);
	return Incoming != nullptr;
}

void UConvaiPlayerComponent::SwitchCaptureSource(UActorComponent* Incoming)
{
	// While capture runs the switch is live: the outgoing source stops, then the incoming one
	// starts, or the built-in device takes over. Idle, adoption only stops the built-in device.
	const bool bCapturing = IsStreaming || IsRecording;
	UObject* Outgoing = ConvaiAudioCaptureComponent.GetObject();
	if (Outgoing != Incoming)
	{
		IConvaiAudioCaptureInterface* OutgoingCapture = ConvaiAudioCaptureComponent.GetInterface();
		if (bCapturing && OutgoingCapture && IsValid(Outgoing))
		{
			OutgoingCapture->Stop();
			CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Stopped alternative audio capture '%s': it was replaced"), *GetNameSafe(Outgoing));
		}
		ConvaiAudioCaptureComponent.SetObject(Incoming);
		ConvaiAudioCaptureComponent.SetInterface(Cast<IConvaiAudioCaptureInterface>(Incoming));
	}

	IConvaiAudioCaptureInterface* Adopted = ConvaiAudioCaptureComponent.GetInterface();
	if (!Adopted)
	{
		if (bCapturing && Outgoing && IsValid(AudioCaptureComponent))
		{
			CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("The built-in microphone takes over from '%s'"), *GetNameSafe(Outgoing));
			StartDefaultCapture();
		}
		return;
	}

	// Both devices render into the submix the plugin captures, so the
	// built-in one left running mixed the host's microphone into whatever
	// the alternative supplied and sent the sum to the server. It is open
	// only once a capture has started - a synth component does not
	// auto-activate on registration - but adoption can come mid-stream.
	if (IsValid(AudioCaptureComponent))
	{
		AudioCaptureComponent->Stop();
		CONVAI_LOG(ConvaiPlayerLog, Log,
			TEXT("Stopped default audio capture: an alternative was adopted"));
	}
	RouteAdoptedCaptureComponent(Incoming);
	if (MicVolumeMultiplier.IsSet())
	{
		Adopted->SetVolumeMultiplier(MicVolumeMultiplier.GetValue());
	}
	if (bCapturing && Outgoing != Incoming)
	{
		Adopted->Start();
		MicOpenRetryCount = 0;
		ScheduleMicOpenRetry();
	}
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Set alternative audio capture component"));
}

/**
 * Collects the microphone submix on the render thread.
 *
 * Append only. The drain converts and sends in this component's game-thread
 * tick, where the recorder path runs too, so adopting the tap does not move
 * the recording buffer or a customer's IConvaiAudioProcessingInterface onto a
 * thread they were never written for.
 */
class FConvaiMicSubmixTap : public ISubmixBufferListener
{
public:
	explicit FConvaiMicSubmixTap(UConvaiPlayerComponent* InOwner) : Owner(InOwner) {}

	virtual void OnNewSubmixBuffer(const USoundSubmix*, float* AudioData, int32 NumSamples,
	                               int32 InNumChannels, const int32 InSampleRate, double) override
	{
		FScopeLock Lock(&OwnerLock);
		if (Owner)
		{
			Owner->AppendMicTapSamples(AudioData, NumSamples, InNumChannels, InSampleRate);
		}
	}

	CONVAI_SUBMIX_LISTENER_NAME("ConvaiMicCaptureTap")

	// Unregister does not drain a callback already in flight.
	void Detach()
	{
		FScopeLock Lock(&OwnerLock);
		Owner = nullptr;
	}

private:
	FCriticalSection OwnerLock;
	UConvaiPlayerComponent* Owner = nullptr;
};

namespace
{
	// How long a stalled consumer the tap survives. The drain is a step of this
	// component's game-thread tick, so a hitch, a paused world or one that stops
	// ticking stops it for as long as that lasts, while the render thread keeps
	// appending. Past this the oldest audio goes.
	constexpr double kMicTapMaxBacklogSeconds = 10.0;
	// What of a backlog reaches the character. Whatever is sent plays out at
	// real time while the microphone keeps feeding behind it, so it stays as
	// latency until something drains the gap. On an open mic nothing does.
	constexpr double kMicTapMaxOpenMicSendSeconds = 0.2;
	// A stall is a drain gap well past the usual one, not a slow frame: a game
	// or an unfocused editor drawing steadily at 3 fps finds 333 ms every drain
	// and keeps all of it. The usual gap is averaged over the drains before.
	constexpr double kMicTapStallFactor = 1.5;
	constexpr double kMicTapDrainGapSmoothing = 0.25;
	// A push-to-talk release drains it, so a hold keeps more of its turn. The
	// transport holds about 2 s and sheds the oldest past that, and a shed
	// frame fails the release's drain: so under it, not at it.
	constexpr double kMicTapMaxHoldSendSeconds = 1.5;
}

void UConvaiPlayerComponent::AppendMicTapSamples(const float* Samples, int32 NumSamples,
                                                 int32 NumChannels, int32 SampleRate)
{
	// The reblocker drops a block with no format, so it is not queued either.
	if (!Samples || NumSamples <= 0 || NumChannels <= 0 || SampleRate <= 0)
	{
		return;
	}

	FScopeLock Lock(&MicTapBufferLock);

	if (NumChannels != MicTapChannels || SampleRate != MicTapSampleRate)
	{
		// A format change mid-stream would splice two timelines; start clean.
		MicTapBuffer.Reset();
		MicTapChannels = NumChannels;
		MicTapSampleRate = SampleRate;
	}

	const int32 MaxFrames = static_cast<int32>(kMicTapMaxBacklogSeconds * SampleRate);
	if (MicTapBuffer.Num() + NumSamples > MaxFrames * NumChannels)
	{
		// Down to three quarters in one memmove: trimming one block per callback
		// moved the whole backlog on every render of a stall.
		const int32 Keep = FMath::Max(0, MaxFrames / 4 * 3 * NumChannels - NumSamples);
		MicTapBuffer.RemoveAt(0, MicTapBuffer.Num() - Keep, CONVAI_ALLOW_SHRINKING_NO);
#if WITH_TESTS
		++MicTapTrimsForTests;
#endif
		if (!bWarnedMicTapOverflow)
		{
			bWarnedMicTapOverflow = true;
			// Many frames means this component stopped ticking while the engine
			// kept framing; none means the game thread itself stalled.
			CONVAI_LOG(ConvaiPlayerLog, Warning,
				TEXT("Microphone tap buffer overflowed: %.0f s with no drain, over %llu engine frames. Audio is being dropped."),
				kMicTapMaxBacklogSeconds, GFrameCounter - MicTapDrainFrame);
		}
	}

	MicTapBuffer.Append(Samples, NumSamples);
}

void UConvaiPlayerComponent::GetMicTapFormat(int32& OutChannels, int32& OutSampleRate, int64& OutChunksRouted) const
{
	// Last seen, not currently seen: the tap is unregistered before a scenario
	// reads this, and a zero here would read as "the tap never ran".
	FScopeLock Lock(&MicTapBufferLock);
	OutChannels = MicTapChannels;
	OutSampleRate = MicTapSampleRate;
	OutChunksRouted = MicTapChunksRouted;
}

void UConvaiPlayerComponent::DrainMicTap()
{
	ensure(IsInGameThread());
	int32 Channels = 0;
	int32 SampleRate = 0;
	{
		FScopeLock Lock(&MicTapBufferLock);
		// Before the early return: an idle drain still proves the consumer ran.
		MicTapDrainFrame = GFrameCounter;
		if (MicTapBuffer.Num() == 0)
		{
			return;
		}
		// Swapped, not moved: moving took the allocation, and the render thread
		// allocated a new one under the lock after every drain.
		Swap(MicTapBuffer, MicTapSpare);
		Channels = MicTapChannels;
		SampleRate = MicTapSampleRate;
	}

	if (!MicReblocker.IsValid())
	{
		MicReblocker = MakeUnique<FConvaiPcmReblocker>(
			ConvaiConstants::VoiceCaptureSampleRate,
			FMath::Max(1, static_cast<int32>(ConvaiConstants::VoiceCaptureSampleRate) / 100));
	}

	// After a stall only the newest audio goes on to the character; a recording keeps all of it.
	const bool bSending = IsStreaming && !bMute;
	const double SendSeconds = bSending && !IsPushToTalkEnabled()
		? FMath::Max(kMicTapMaxOpenMicSendSeconds, kMicTapStallFactor * MicTapUsualDrainSeconds.Get(0.0))
		: kMicTapMaxHoldSendSeconds;
	// After the bound, so a stall cannot raise its own; capped, so the open-mic bound stays under the hold's.
	const double DrainGap = FMath::Min(static_cast<double>(MicTapSpare.Num()) / (Channels * SampleRate),
		kMicTapMaxHoldSendSeconds / kMicTapStallFactor);
	MicTapUsualDrainSeconds = MicTapUsualDrainSeconds.IsSet()
		? FMath::Lerp(MicTapUsualDrainSeconds.GetValue(), DrainGap, kMicTapDrainGapSmoothing)
		: DrainGap;
	const int32 Stale = FMath::Max(0, MicTapSpare.Num() - static_cast<int32>(SendSeconds * SampleRate) * Channels);
	if (Stale > 0)
	{
		if (bSending)
		{
			CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Microphone backlog of %.1f s after a stall: only the last %.1f s is sent"),
				static_cast<double>(MicTapSpare.Num()) / (Channels * SampleRate), SendSeconds);
		}
		if (IsRecording)
		{
			MicReblocker->PushInterleavedFloat(MicTapSpare.GetData(), Stale, Channels, SampleRate);
			MicReblocker->Drain([this](const int16* Chunk, int32 Num)
			{
				++MicTapChunksRouted;
				VoiceCaptureBuffer.Append(reinterpret_cast<const uint8*>(Chunk), Num * sizeof(int16));
			});
		}
	}

	MicReblocker->PushInterleavedFloat(MicTapSpare.GetData() + Stale, MicTapSpare.Num() - Stale, Channels, SampleRate);
	MicReblocker->Drain([this](const int16* Chunk, int32 Num)
	{
		++MicTapChunksRouted;
		RouteCapturedMicChunk(Chunk, Num);
	});

	// A stall's allocation is let go rather than kept for every drain after it.
	if (MicTapSpare.Max() > Channels * SampleRate)
	{
		MicTapSpare.Empty();
	}
	else
	{
		MicTapSpare.Reset();
	}
}

bool UConvaiPlayerComponent::EnsureMicTapRegistered()
{
	ensure(IsInGameThread());
	if (MicTap.IsValid())
	{
		return true;
	}

	// The submix, not the capture component. Every capture component the plugin
	// accepts - its own and an adopted one - is routed to this submix by
	// RouteAdoptedCaptureComponent, so binding here covers both; binding to a
	// component's own SoundSubmix would silently capture nothing the moment a
	// third-party component was adopted.
	USoundSubmix* MicSubmix = Cast<USoundSubmix>(_FoundSubmix);
	Audio::FMixerDevice* MixerDevice = GetAudioMixerDeviceFromWorldContext(this);
	if (!MicSubmix || !MixerDevice)
	{
		// Falling back to the master submix here would send every game sound to
		// the server as the player's voice.
		CONVAI_LOG(ConvaiPlayerLog, Warning,
			TEXT("Microphone tap unavailable (%s); falling back to the output recorder."),
			MicSubmix ? TEXT("no mixer device") : TEXT("no AudioInput submix"));
		bUseMicListenerTap = false;
		return false;
	}

	{
		FScopeLock Lock(&MicTapBufferLock);
		MicTapBuffer.Reset();
	}
	MicReblocker.Reset();
	MicTapUsualDrainSeconds.Reset();

	MicTap = MakeShared<FConvaiMicSubmixTap, ESPMode::ThreadSafe>(this);
	ConvaiRegisterSubmixListener(MixerDevice, MicTap.ToSharedRef(), MicSubmix);
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Microphone capture tap registered on the AudioInput submix"));
	return true;
}

void UConvaiPlayerComponent::UnregisterMicTap(bool bDrainCapture)
{
	ensure(IsInGameThread());
	if (!MicTap.IsValid())
	{
		return;
	}

	if (Audio::FMixerDevice* MixerDevice = GetAudioMixerDeviceFromWorldContext(this))
	{
		if (USoundSubmix* MicSubmix = Cast<USoundSubmix>(_FoundSubmix))
		{
			ConvaiUnregisterSubmixListener(MixerDevice, MicTap.ToSharedRef(), MicSubmix);
		}
	}
	MicTap->Detach();
	MicTap.Reset();

	if (bDrainCapture)
	{
		DrainMicTap();
		if (MicReblocker.IsValid())
			MicReblocker->FlushPadded([this](const int16* Chunk, int32 Num) { RouteCapturedMicChunk(Chunk, Num); });
	}
	else
	{
		FScopeLock Lock(&MicTapBufferLock);
		MicTapBuffer.Reset();
	}
}

void UConvaiPlayerComponent::UpdateVoiceCapture(float DeltaTime)
{
	if (IsRecording || IsStreaming)
	{
		if (bUseMicListenerTap && EnsureMicTapRegistered())
		{
			// Every tick, not on the countdown below: it counts DeltaTime, which time
			// dilation scales, while the render thread fills the tap on the wall clock.
			// Slow motion delayed the mic stream, and a frozen pawn stopped it.
			DrainMicTap();
			return;
		}

		RemainingTimeUntilNextUpdate -= DeltaTime;
		if (RemainingTimeUntilNextUpdate <= 0)
		{
			float ExpectedRecordingTime = DeltaTime > TIME_BETWEEN_VOICE_UPDATES_SECS ? DeltaTime : TIME_BETWEEN_VOICE_UPDATES_SECS;

			StopVoiceChunkCapture();
			StartVoiceChunkCapture(ExpectedRecordingTime);
			RemainingTimeUntilNextUpdate = TIME_BETWEEN_VOICE_UPDATES_SECS;
		}
	}
	else
	{
		UnregisterMicTap();
		RemainingTimeUntilNextUpdate = 0;
	}
}

void UConvaiPlayerComponent::StartVoiceChunkCapture(float ExpectedRecordingTime) const
{
	// The two capture paths are exclusive. StartRecording, UnmuteStreamingAudio
	// and the mic-health restart all arm the recorder directly, so guarding only
	// the tick would leave it armed and every StopVoiceChunkCapture would route
	// a second copy of the same audio - which reads to the canceller as a mic
	// stream running at twice the far end's rate.
	if (bUseMicListenerTap)
	{
		return;
	}

	//GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Yellow, FString::Printf(TEXT("StartVoiceChunkCapture() in VoiceCaptureComp.cpp")));
	//CONVAI_LOG(LogTemp, Warning, TEXT("StartVoiceChunkCapture() in VoiceCaptureComp.cpp"));
	UAudioMixerBlueprintLibrary::StartRecordingOutput(this, ExpectedRecordingTime, Cast<USoundSubmix>(AudioCaptureComponent->SoundSubmix));
}

void UConvaiPlayerComponent::ReadRecordedBuffer(Audio::AlignedFloatBuffer& RecordedBuffer, float& OutNumChannels, float& OutSampleRate) const
{
#if WITH_TESTS
	if (RecordedAudioForTest)
	{
		RecordedAudioForTest(RecordedBuffer, OutNumChannels, OutSampleRate);
		return;
	}
#endif
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

	// Latched per capture so a run cannot change tap halfway through.
	bUseMicListenerTap = !UConvaiUtils::GetMicCaptureTap().Equals(TEXT("Recorder"), ESearchCase::IgnoreCase);
	{
		// Under the lock the render thread writes them under, and seeded from
		// now: a zero drain frame would report the whole process uptime as the
		// gap on an overflow that happened before the first drain.
		FScopeLock Lock(&MicTapBufferLock);
		bWarnedMicTapOverflow = false;
		MicTapDrainFrame = GFrameCounter;
	}

	if (IConvaiAudioCaptureInterface* Adopted = GetAdoptedCapture(false))
	{
		Adopted->Start();
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Started alternative audio capture"));
		// The health check watches an adopted source too: one destroyed mid-capture gives the
		// microphone back to the built-in device.
		MicOpenRetryCount = 0;
		ScheduleMicOpenRetry();
		return;
	}

	StartDefaultCapture();
}

IConvaiAudioCaptureInterface* UConvaiPlayerComponent::GetAdoptedCapture(bool bResumeDefault)
{
	UObject* Adopted = ConvaiAudioCaptureComponent.GetObject();
	if (!Adopted || IsValid(Adopted))
	{
		return ConvaiAudioCaptureComponent.GetInterface();
	}

	// Destroyed, but the handle stays set until garbage collection: without this the built-in
	// device was never restarted, and a plain Unmute was refused because streaming never stopped.
	if (bResumeDefault)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("Adopted capture component '%s' was destroyed"), *GetNameSafe(Adopted));
		SwitchCaptureSource(nullptr);
	}
	else
	{
		ConvaiAudioCaptureComponent = nullptr;
	}
	return nullptr;
}

void UConvaiPlayerComponent::StartDefaultCapture()
{
#if WITH_TESTS
	if (StartDefaultCaptureForTest)
	{
		StartDefaultCaptureForTest();
	}
	else
#endif
	{
		AudioCaptureComponent->Start();
	}
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Started default audio capture"));

	bRestartMicAfterPermission = !HasMicrophonePermission();
	MicOpenRetryCount = 0;
	ScheduleMicOpenRetry();
}

void UConvaiPlayerComponent::ScheduleMicOpenRetry()
{
	MicCheckDelay = MicCheckBackoffSeconds[FMath::Clamp(MicOpenRetryCount, 0, static_cast<int32>(UE_ARRAY_COUNT(MicCheckBackoffSeconds)) - 1)];
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(MicOpenRetryTimer, this,
			&UConvaiPlayerComponent::CheckMicCaptureHealth, MicCheckDelay, false);
	}
}

void UConvaiPlayerComponent::CheckMicCaptureHealth()
{
	if (!IsStreaming && !IsRecording)
		return;

	// An adopted component is the microphone now, and the default was stopped
	// on purpose. Restarting it reopened the host's device on the game thread
	// (a 250-300 ms frame) and mixed the host's voice into the adopted audio.
	// Only the adoptee's liveness is watched.
	const bool bHadAdoptee = ConvaiAudioCaptureComponent.GetObject() != nullptr;
	if (GetAdoptedCapture(true))
	{
		ScheduleMicOpenRetry();
		return;
	}
	if (bHadAdoptee)
	{
		// Dropped just now; the built-in device was started with a check of its own.
		return;
	}

	if (!IsValid(AudioCaptureComponent))
		return;

	// Without RECORD_AUDIO a restart cannot help, so wait for the grant without spending retries.
	// A stream opened before the grant can deliver silence on time, so it is restarted once after.
	if (!HasMicrophonePermission())
	{
		bRestartMicAfterPermission = true;
		MicOpenRetryCount = 0;
		ScheduleMicOpenRetry();
		return;
	}
	if (bRestartMicAfterPermission)
	{
		bRestartMicAfterPermission = false;
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Microphone permission granted - restarting capture"));
		RestartMicCapture();
		MicOpenRetryCount = 0;
		ScheduleMicOpenRetry();
		return;
	}

	if (IsMicCaptureHealthy())
	{
		MicOpenRetryCount = 0;
		ScheduleMicOpenRetry();
		return;
	}

	++MicOpenRetryCount;
	RestartMicCapture();
	ScheduleMicOpenRetry();
	// A machine with no microphone at all retries for as long as capture runs: warn once per run of failures.
	if (MicOpenRetryCount == 1)
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("Microphone capture not active (attempt %d) - restarted capture, next check in %.0f s. Check that a microphone/input device is connected, that microphone permission is granted to this app, and that no other app holds the mic"), MicOpenRetryCount, MicCheckDelay);
	}
	else
	{
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Microphone capture still not active (attempt %d) - restarted capture, next check in %.0f s"), MicOpenRetryCount, MicCheckDelay);
	}
}

bool UConvaiPlayerComponent::IsMicCaptureHealthy() const
{
#if WITH_TESTS
	if (MicCaptureHealthyForTest)
	{
		return MicCaptureHealthyForTest();
	}
#endif
	return AudioCaptureComponent->IsCaptureStreamHealthy();
}

bool UConvaiPlayerComponent::HasMicrophonePermission() const
{
#if WITH_TESTS
	if (MicPermissionForTest)
	{
		return MicPermissionForTest();
	}
#endif
	return UConvaiAndroid::ConvaiAndroidHasMicrophonePermission();
}

void UConvaiPlayerComponent::RestartMicCapture()
{
#if WITH_TESTS
	if (RestartMicCaptureForTest)
	{
		RestartMicCaptureForTest();
		return;
	}
#endif
	AudioCaptureComponent->RestartCapture();
}

void UConvaiPlayerComponent::StopAudioCaptureComponent()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(MicOpenRetryTimer);
	}

	if (IConvaiAudioCaptureInterface* Adopted = GetAdoptedCapture(false))
	{
		Adopted->Stop();
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Stopped alternative audio capture"));
	}

	if (IsValid(AudioCaptureComponent))
	{
		AudioCaptureComponent->Stop();
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Stopped default audio capture"));
	}

	// Nothing renders into the submix any more, so anything still queued is
	// audio from before the stop. Sending it late is worse than losing it.
	UnregisterMicTap();
	MicReblocker.Reset();
}

void UConvaiPlayerComponent::StopVoiceChunkCapture()
{
	if (bUseMicListenerTap)
	{
		return;
	}

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

	RouteCapturedMicChunk(OutConverted.GetData(), OutConverted.Num());
}

void UConvaiPlayerComponent::RouteCapturedMicChunk(const int16* Samples, int32 NumSamples)
{
	if (!Samples || NumSamples <= 0)
	{
		return;
	}

	if (IsRecording)
	{
		VoiceCaptureBuffer.Append(reinterpret_cast<const uint8*>(Samples), NumSamples * sizeof(int16));
	}

	// Write to the shared buffer for direct access
	if (IsStreaming)
	{
		if (bMute) return;

		if (SupportsAudioProcessing())
		{
			SafeProcessAudioData(const_cast<int16*>(Samples), NumSamples, ConvaiConstants::VoiceCaptureSampleRate);
		}
		// Send audio to the session proxy if we have one
		else if (IsValid(SessionProxyInstance))
		{
			// Calculate the number of frames (samples per channel)
			const size_t NumFrames = NumSamples;
			SessionProxyInstance->SendAudio((const int16_t*)Samples, NumFrames);
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

	// The mic test forces bMute on, so the key-up would change nothing and end no turn.
	if (IsStreaming && !bMute && IsPushToTalkEnabled())
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("StartRecording: refused while the push-to-talk key is held; release it first"));
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
	if (IsStreaming)
	{
		// The stream already owns the capture device and drives the chunk pump, so don't touch
		// either - just keep the recorded audio from reaching the character for the duration.
		MuteBeforeRecording = bMute;
		bMute = true;
		StopAwaitingServerStop();
		bWarnedPressDuringMicTest = false;
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
	if (bUseMicListenerTap)
	{
		// The tap's tail since the last drain; StopVoiceChunkCapture collects one only on the
		// recorder. Before bMute is restored, so a stream it forced quiet stays quiet.
		DrainMicTap();
		if (MicReblocker.IsValid())
			MicReblocker->FlushPadded([this](const int16* Chunk, int32 Num) { RouteCapturedMicChunk(Chunk, Num); });
	}
	EndRecording();

#if !UE_BUILD_SHIPPING
	// For debugging, and what the capture_device_format scenario measures. A
	// shipped game does not keep the player's voice on disk.
	FString FileName = FPaths::ProjectSavedDir() / TEXT("AudioDebug/recorded_audio.wav");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FileName), true);
	TArray<uint8> WavFileData;
	UConvaiUtils::PCMDataToWav(VoiceCaptureBuffer, WavFileData, 1, ConvaiConstants::VoiceCaptureSampleRate);
	UConvaiUtils::SaveByteArrayAsFile(FileName, WavFileData);

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Saved recorded audio to %s - %d bytes"),
		*FileName, VoiceCaptureBuffer.Num());
#endif

	USoundWave* OutSoundWave = UConvaiUtils::PCMDataToSoundWav(VoiceCaptureBuffer, 1, ConvaiConstants::VoiceCaptureSampleRate);
	if (IsValid(OutSoundWave))
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("OutSoundWave->GetDuration(): %f seconds "), OutSoundWave->GetDuration());
	return OutSoundWave;
}

void UConvaiPlayerComponent::EndRecording()
{
	StopVoiceChunkCapture();

	// Only close the device if nothing else is using it. Checking IsStreaming now rather than
	// what it was at StartRecording also covers the stream being muted mid-recording, which
	// leaves us as the last user and so responsible for shutting the capture down.
	if (!IsStreaming)
	{
		StopAudioCaptureComponent();
	}

	// Put bMute back exactly as the project had it, rather than assuming false.
	if (MuteBeforeRecording.IsSet())
	{
		bMute = MuteBeforeRecording.GetValue();
		MuteBeforeRecording.Reset();
	}
	IsRecording = false;
}

void UConvaiPlayerComponent::BeginPlay()
{
	// A level streamed out and back in begins play again after its End Play.
	bEndingPlay = false;

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

	// A player's session is bookkeeping - it opens no connection of its own -
	// so there is nothing to wait for. Starting it only from the Connected
	// broadcast meant a player that began play after the character had already
	// connected never got one, and its microphone and text were silently dead
	// for the rest of the level.
	//
	// After the capture discovery above, never before it: starting the session
	// opens the microphone, and before discovery that is always the host's own
	// device even when the actor carries a component meant to replace it.
	AutoStartSession();
}

void UConvaiPlayerComponent::AutoStartSession()
{
	bAutoStartPending = false;
	if (!bAutoInitializeSession || IsValid(SessionProxyInstance))
	{
		return;
	}
	// Decided at possession, not at Begin Play: a pawn spawned during play
	// begins play before anything possesses it, the local player's own too.
	if (const APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		bLocallyPossessed = IsPossessedByLocalPlayer(*Pawn);
		if (!bLocallyPossessed)
		{
			bAutoStartPending = true;
			return;
		}
	}
	else if (const APlayerController* Controller = Cast<APlayerController>(GetOwner()); Controller && !Controller->IsLocalController())
	{
		return;
	}
	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Auto Initialize Session: starting session"));
	StartSession();
}

void UConvaiPlayerComponent::TakeMicrophoneOnPossession()
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const bool bWasPossessed = bLocallyPossessed;
	bLocallyPossessed = Pawn && IsPossessedByLocalPlayer(*Pawn);
	if (!bLocallyPossessed || bWasPossessed || !IsValid(SessionProxyInstance))
	{
		return;
	}
	// Back on this pawn while the pawn they left holds the microphone: the
	// player's presses now drive this component.
	const UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this);
	if (Subsystem && !Subsystem->OwnsMicrophone(SessionProxyInstance) && SessionProxyInstance->Connect())
	{
		// Streams again if it was streaming when displaced, as on a hand-back.
		OnConnectedToServer();
	}
}

bool UConvaiPlayerComponent::ConsumeStreamingBuffer(TArray<uint8>& Buffer)
{
	// Kept for source compatibility only: captured audio goes straight to the session, so there is
	// never anything here to return.
	CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("ConsumeStreamingBuffer is deprecated. Use GetSharedAudioBuffer()->ConsumeAll() instead."));
	return false;
}

// Implementation of session-related functions

void UConvaiPlayerComponent::SetMute(const bool bNewMute)
{
	if (bMute == bNewMute)
	{
		return;
	}

	if (!bNewMute && IsPushToTalkEnabled())
	{
		// The mic test holds the button up and puts it back when it ends: a press now would
		// stream the test to the character, and the put-back would swallow its release.
		if (MuteBeforeRecording.IsSet())
		{
			if (!bWarnedPressDuringMicTest)
			{
				bWarnedPressDuringMicTest = true;
				CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("Push To Talk press ignored: a mic test is running; finish it first"));
			}
			return;
		}
		UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this);
		if (Subsystem && !Subsystem->CanStartPushToTalkCapture())
		{
			if (Subsystem->TakePttLockWarning())
			{
				CONVAI_LOG(ConvaiPlayerLog, Warning,
					TEXT("Push To Talk press ignored: a failed release locked push-to-talk until the session reconnects"));
			}
			return;
		}
	}
	if (bNewMute && IsPushToTalkEnabled() && IsStreaming)
	{
		if (bUseMicListenerTap) UnregisterMicTap(true);
		else StopVoiceChunkCapture();
	}
	bMute = bNewMute;
	if (bNewMute && IsStreaming && bUseMicListenerTap) EnsureMicTapRegistered();
	if (bNewMute) StopAwaitingServerStop();

	// A press opens a fresh hold whatever the server made of the last one. Cleared above the
	// push-to-talk gate because a bracket can close while the button is up, and because a press
	// before the session exists would otherwise leave the latch stale into the next release.
	if (!bNewMute)
	{
		bServerEndedTurnThisHold = false;
	}

	// A press is the player wanting to talk: a Dormant character wakes for it, with
	// no energy threshold to clear. An open-mic unmute - a cutscene ending - is not.
	if (!bNewMute && IsPushToTalkEnabled())
	{
		if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
		{
			ConvaiSubsystem->WakeReconnect();
		}
	}

	// Open mic: the soft-mute is local, and a game muting for a cutscene must not read as the
	// player finishing a sentence. Under push-to-talk the same flag is the button, so the
	// server has to hear about it.
	if (!IsPushToTalkEnabled() || !IsValid(SessionProxyInstance))
	{
		return;
	}

	if (bNewMute)
	{
		// The subsystem drains native audio before muting STT and committing this release.
		if (ShouldEndTurnOnRelease())
		{
			SessionProxyInstance->ForceUserStoppedSpeaking();
		}
		else
		{
			CONVAI_LOG(ConvaiPlayerLog, Log,
				TEXT("Skipped force-user-stopped-speaking: the server already ended this turn"));
			SessionProxyInstance->ToggleSTT(true);
		}
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
	// Found once per object: its class does not change, and a Blueprint recompile reinstances it.
	if (!bPushToTalkSwitchResolved)
	{
		bPushToTalkSwitchResolved = true;
		static const FName PushToTalkPropertyName(TEXT("EnablePushToTalk"));
		PushToTalkSwitch = FindFProperty<FBoolProperty>(GetClass(), PushToTalkPropertyName);
		if (!PushToTalkSwitch)
		{
			CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("IsPushToTalkEnabled: Could not find property %s, returning false"), *PushToTalkPropertyName.ToString());
		}
	}
	return PushToTalkSwitch && PushToTalkSwitch->GetPropertyValue_InContainer(this);
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
	bStreamWhenHandedBack = false;
	if (IsValid(SessionProxyInstance))
	{
		// A player leaving is not a teardown of the character's transport, and
		// marking it explicit made a character's later drop read as one the
		// game asked for — which is exactly the drop a reconnect must not skip.

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
		// Typing is the player coming back: a Dormant session reopens first,
		// so the text is held for it and delivered once it is ready.
		if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
		{
			ConvaiSubsystem->WakeReconnect();
		}

		// Activity is marked when the text actually reaches the character: text
		// held for a session that is still coming up is not delivered yet.
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

	// Its audio would be refused, and its device would render into the submix the owner records.
	const UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this);
	if (Subsystem && !Subsystem->OwnsMicrophone(SessionProxyInstance))
	{
		CONVAI_LOG(ConvaiPlayerLog, Warning, TEXT("UnmuteStreamingAudio: refused, another player component's session holds the microphone"));
		return false;
	}

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Started Streaming Audio"));

	StartAudioCaptureComponent();    // Start the AudioCaptureComponent

	// Reset audio buffers
	StartVoiceChunkCapture();
	StopVoiceChunkCapture();

	IsStreaming = true;

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

	if (IsPushToTalkEnabled() && !bMute) SetMute(true);
	StopVoiceChunkCapture();
	// A running mic test keeps the device; Finish Recording stops it.
	if (!IsRecording)
	{
		StopAudioCaptureComponent();
	}
	IsStreaming = false;
	StopAwaitingServerStop();

	if (IsValid(SessionProxyInstance))
	{
		const UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this);
		if (!Subsystem || Subsystem->CanStartPushToTalkCapture())
			SessionProxyInstance->ToggleSTT(true);
	}

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Stopped Streaming Audio"));
}

void UConvaiPlayerComponent::OnConnectedToServer()
{
	// Besides the owner's connects, this is the subsystem handing the
	// microphone back to a component another one displaced.
	const UWorld* World = GetWorld();
	if (bStreamWhenHandedBack && !IsStreaming && !bEndingPlay && !(World && World->bIsTearingDown))
	{
		UnmuteStreamingAudio();
	}
	bStreamWhenHandedBack = false;
}

void UConvaiPlayerComponent::OnDisconnectedFromServer()
{
	// May arrive on the WebRTC transport thread; bIsSpeaking is game-thread-only.
	TWeakObjectPtr<UConvaiPlayerComponent> WeakSelf(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelf]
		{
			if (UConvaiPlayerComponent* Self = WeakSelf.Get())
			{
				const bool bWasSpeaking = Self->bIsSpeaking;
				Self->ResetSpeakingStateSilently();
				// A push-to-talk hold does not survive the session it was held
				// in: its release would commit a turn the new session never heard.
				if (Self->IsPushToTalkEnabled() && !Self->bMute)
				{
					Self->bMute = true;
					CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Push-to-talk hold ended by the drop"));
				}

				// Another component's session took the microphone. Decided on
				// ownership, not on this callback: a plain drop calls it too, and
				// keeps capturing, because the Dormant voice wake listens to that audio.
				const UConvaiSubsystem* Subsystem = Self->IsStreaming && IsValid(Self->SessionProxyInstance)
					? UConvaiUtils::GetConvaiSubsystem(Self) : nullptr;
				if (Subsystem && !Subsystem->OwnsMicrophone(Self->SessionProxyInstance))
				{
					CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Another player component holds the microphone: capture stops until it is handed back"));
					Self->MuteStreamingAudio();
					Self->bStreamWhenHandedBack = true;
				}

				// Last, so a handler sees the drop already applied. Neither Player
				// Activity nor the server taking the turn, so neither is marked.
				if (bWasSpeaking)
				{
					CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Finished talking: the session ended"));
					Self->OnFinishedTalkingDelegate.Broadcast();
				}
			}
		});
}

void UConvaiPlayerComponent::ResyncStt()
{
	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->ToggleSTT(ShouldMuteStt(IsStreaming, IsPushToTalkEnabled(), bMute));
	}
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
			AutoStartSession();
		}
		else
		{
			// A reconnected session: its speech-to-text state is the server's
			// default, not this player's. Ready sends it again, for the bot.
			ResyncStt();
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
		OpenSpeakingBracket(/*bServerStarted*/ false);
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

	OpenSpeakingBracket(/*bServerStarted*/ true);
}

void UConvaiPlayerComponent::OpenSpeakingBracket(const bool bServerStarted)
{
	CancelPendingSpeakingStop();
	bServerSpeakingOpen |= bServerStarted;
	if (!bServerSpeakingOpen)
	{
		ArmSpeakingWatchdog();
	}
	// Before the duplicate-start early-out: a partial arriving after the server took the turn is
	// what makes the guard survive a backend that omits user-started-speaking.
	bServerEndedTurnThisHold = false;
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

void UConvaiPlayerComponent::ArmSpeakingWatchdog()
{
	SpeakingWatchdogHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(
			this,
			&UConvaiPlayerComponent::HandleSpeakingWatchdogElapsed),
		SpeakingEvidenceTimeoutSeconds);
}

void UConvaiPlayerComponent::StopAwaitingServerStop()
{
	// Not closed here: muting is not the player finishing a sentence, and the stop may still come.
	if (bServerSpeakingOpen)
	{
		bServerSpeakingOpen = false;
		ArmSpeakingWatchdog();
	}
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

	// From here only a continuing partial can hold the bracket open, so the watchdog watches it again.
	bServerSpeakingOpen = false;
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
	// Every caller either closes the bracket or has fresh evidence and re-arms it.
	if (SpeakingWatchdogHandle.IsValid())
	{
		FTSTicker::RemoveTicker(SpeakingWatchdogHandle);
		SpeakingWatchdogHandle.Reset();
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

bool UConvaiPlayerComponent::HandleSpeakingWatchdogElapsed(float /*DeltaTime*/)
{
	check(IsInGameThread());

	// One-shot, like the stop grace.
	SpeakingWatchdogHandle.Reset();
	if (bIsSpeaking)
	{
		// Not FinishSpeakingNow: the server has not taken this turn, so a push-to-talk
		// release must still end it, and a timeout is no Player Activity.
		CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Finished talking: no speech for %.0f s"), SpeakingEvidenceTimeoutSeconds);
		CancelPendingSpeakingStop();
		bIsSpeaking = false;
		OnFinishedTalkingDelegate.Broadcast();
	}
	return false;
}

void UConvaiPlayerComponent::FinishSpeakingNow()
{
	check(IsInGameThread());

	CancelPendingSpeakingStop();
	bServerSpeakingOpen = false;
	if (!bIsSpeaking)
	{
		return;
	}

	CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("Finished talking"));
	bIsSpeaking = false;
	// The server took this turn, so a forced stop on release has nothing left to commit.
	bServerEndedTurnThisHold = true;

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
	bServerSpeakingOpen = false;
	bServerEndedTurnThisHold = false;
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
	// A class default object or a Blueprint's component template never had a
	// session, and has no world or subsystem to look one up in.
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		Super::BeginDestroy();
		return;
	}

	bEndingPlay = true;
	if (IsInGameThread())
	{
		ResetSpeakingStateSilently();
	}

	DetachAudioProcessing();

	if (IsRecording)
	{
		EndRecording();
	}

	if (IsStreaming)
	{
		MuteStreamingAudio();
	}

	if (IsValid(SessionProxyInstance))
	{
		StopSession();
	}

	VoiceCaptureBuffer.Empty();

	if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		ConvaiSubsystem->UnregisterPlayerComponent(this);
	}

	Super::BeginDestroy();
}


IConvaiAudioProcessingInterface* UConvaiPlayerComponent::FindFirstAudioProcessingComponent()
{
#if WITH_TESTS
	++AudioProcessingLookupsForTests;
#endif
	if (const AActor* Owner = GetOwner())
	{
		const TArray<UActorComponent*> AudioProcessingComponents = Owner->GetComponentsByInterface(UConvaiAudioProcessingInterface::StaticClass());
		if (AudioProcessingComponents.Num())
		{
			SetAudioProcessingComponent(AudioProcessingComponents[0]);
		}
	}
	return ConvaiAudioProcessing.GetInterface();
}

bool UConvaiPlayerComponent::SetAudioProcessingComponent(UActorComponent* AudioProcessingComponent)
{
	const bool bIsProcessor = AudioProcessingComponent
		&& AudioProcessingComponent->GetClass()->ImplementsInterface(UConvaiAudioProcessingInterface::StaticClass());
	if (ConvaiAudioProcessing.GetObject() != (bIsProcessor ? AudioProcessingComponent : nullptr))
	{
		DetachAudioProcessing();
	}
	if (!bIsProcessor)
	{
		return false;
	}

	ConvaiAudioProcessing.SetObject(AudioProcessingComponent);
	ConvaiAudioProcessing.SetInterface(Cast<IConvaiAudioProcessingInterface>(AudioProcessingComponent));
	if (IConvaiAudioProcessingInterface* Interface = ConvaiAudioProcessing.GetInterface())
	{
		Interface->SetProcessedAudioReceiver(this);
	}
	return true;
}

void UConvaiPlayerComponent::DetachAudioProcessing()
{
	// The processor keeps a raw pointer back to this component, and one that
	// outlived it called into freed memory.
	if (IConvaiAudioProcessingInterface* Processor = ConvaiAudioProcessing.GetInterface();
		Processor && IsValid(ConvaiAudioProcessing.GetObject()))
	{
		Processor->SetProcessedAudioReceiver(nullptr);
	}
	ConvaiAudioProcessing = nullptr;
}

bool UConvaiPlayerComponent::SupportsAudioProcessing()
{
	if (!bEnableAudioProcessingParse) return false;

	// A processor added after Begin Play is still found, but a player without
	// one no longer walks its owner's components for every 10 ms chunk.
	if (ConvaiAudioProcessing == nullptr && !bEndingPlay)
	{
		const double Now = FPlatformTime::Seconds();
		if (Now >= NextAudioProcessingLookupSeconds)
		{
			NextAudioProcessingLookupSeconds = Now + 1.0;
			FindFirstAudioProcessingComponent();
		}
	}
	return ConvaiAudioProcessing != nullptr;
}

void UConvaiPlayerComponent::SafeProcessAudioData(const int16* AudioData, int32 NumSamples, int32 SampleRate) const
{
	if (!ConvaiAudioProcessing)
	{
		return;
	}

	ConvaiAudioProcessing->ProcessAudioData(AudioData, NumSamples, SampleRate);
}

void UConvaiPlayerComponent::OnProcessedAudioDataReceived(const int16* ProcessedAudioData, int32 NumSamples, int32 SampleRate)
{
	// A processor may answer on any thread. What follows reads game-thread
	// state, and the send behind it reads a client the game thread can let go
	// of, so the chunk is copied and handled there; one that lands after a
	// push-to-talk release is then dropped by the mute.
	if (!IsInGameThread())
	{
		if (!ProcessedAudioData || NumSamples <= 0)
		{
			return;
		}
		TWeakObjectPtr<UConvaiPlayerComponent> WeakSelf(this);
		AsyncTask(ENamedThreads::GameThread,
			[WeakSelf, Samples = TArray<int16>(ProcessedAudioData, NumSamples), SampleRate]()
			{
				if (UConvaiPlayerComponent* Self = WeakSelf.Get())
				{
					Self->OnProcessedAudioDataReceived(Samples.GetData(), Samples.Num(), SampleRate);
				}
			});
		return;
	}

	if (IsRecording) {
		VoiceCaptureBuffer.Append((uint8*)ProcessedAudioData, NumSamples * sizeof(int16));
	}
	// bMute has to be honoured here as well as in StopVoiceChunkCapture: when audio processing is
	// active this is the path that reaches the session, so without it a muted mic - or a mic test
	// taken while streaming - would still be heard by the character.
	if (IsStreaming && !bMute) {
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
	FGazeMatches HitMatches;
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
			FGazeMatches PrevMatches;
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

		ClearActiveHighlight();
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
	FGazeMatches& OutMatches, bool& bOutAnyComponentScoped) const
{
	OutMatches.Reset();
	bOutAnyComponentScoped = false;
	if (!Actor)
	{
		return;
	}

	FGazeMatches AllObjs;
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
	FGazeMatches WholeActorObjs;
	FGazeMatches ComponentObjs;
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
	FGazeMatches ComponentMatches;
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

	// The subsystem-wide pool: every ObjComp, designer-placed or chatbot-auto-spawned, registers
	// there. A maximum needs no de-duplication, so the pool is walked as it comes.
	for (UConvaiObjectComponent* Obj : Subsystem->GetAllObjectComponents())
	{
		if (!IsValid(Obj) || !Obj->bGazeable)
		{
			// Gone, or opted out as non-gazeable.
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

	// The default class is retargeted rather than spawned per target: sweeping the view across a
	// pile of objects spawned and destroyed one a frame. A subclass is the extension point and may
	// do its visuals per target in Begin Play and End Play, so it still gets one actor per target.
	AConvaiGazeHighlightActor* Highlight = ActiveHighlight.Get();
	if (Highlight && (Class != AConvaiGazeHighlightActor::StaticClass() || Highlight->GetClass() != Class))
	{
		DestroyActiveHighlight();
		Highlight = nullptr;
	}
	if (Highlight)
	{
		Highlight->SetActorLocation(PrimaryActor->GetActorLocation());
	}
	else
	{
		FActorSpawnParameters Spawn;
		Spawn.Owner = GetOwner();
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Highlight = World->SpawnActor<AConvaiGazeHighlightActor>(
			Class, PrimaryActor->GetActorLocation(), FRotator::ZeroRotator, Spawn);
		if (!Highlight)
		{
			return;
		}
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

void UConvaiPlayerComponent::ClearActiveHighlight()
{
	AConvaiGazeHighlightActor* Highlight = ActiveHighlight.Get();
	if (Highlight && Highlight->GetClass() == AConvaiGazeHighlightActor::StaticClass())
	{
		// Clearing its targets leaves it attached to the last one, to move with it and into its level.
		Highlight->SetTargets({});
		Highlight->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		return;
	}
	DestroyActiveHighlight();
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
	FGazeMatches Matches;
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
	FGazeMatches Matches;
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
		ClearActiveHighlight();
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

