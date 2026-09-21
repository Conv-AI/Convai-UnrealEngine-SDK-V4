// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiMicrophoneSubsystem.h"

#include "ConvaiPlayerComponent.h"
#include "Utility/Log/ConvaiLogger.h"

#include "AudioCapture.h"
#include "AudioCaptureCore.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "GameFramework/SaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(ConvaiMicrophoneLog);

namespace
{
	/** One probe of the capture devices present right now. Cheap enough to do on demand. */
	TArray<Audio::FCaptureDeviceInfo> ProbeCaptureDevices()
	{
		TArray<Audio::FCaptureDeviceInfo> Devices;
		Audio::FAudioCapture Probe;
		Probe.GetCaptureDevicesAvailable(Devices);
		return Devices;
	}

	/** Locate the "Settings" struct inside either plugin's record, plus a pointer to its value. */
	bool FindSettingsStruct(const USaveGame* SaveObject, FStructProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!SaveObject)
		{
			return false;
		}

		OutProperty = FindFProperty<FStructProperty>(SaveObject->GetClass(), UConvaiMicrophoneSaveGame::SettingsPropertyName);
		if (!OutProperty || !OutProperty->Struct)
		{
			return false;
		}

		// const_cast so one lookup serves both the read and the write path; the read path only
		// ever hands the pointer to GetPropertyValue_InContainer.
		OutValuePtr = OutProperty->ContainerPtrToValuePtr<void>(const_cast<USaveGame*>(SaveObject));
		return OutValuePtr != nullptr;
	}
}

void UConvaiMicrophoneSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (!LoadFromDisk())
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Log,
			TEXT("No saved microphone settings - using the system default microphone."));
		return;
	}

	// Report the outcome at startup rather than at first use, so a missing mic is visible in the
	// log from the beginning. UConvaiPlayerComponent::BeginPlay does the actual applying.
	if (Settings.InputDeviceName.IsEmpty())
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Log,
			TEXT("Loaded saved microphone settings (gain %.2f, system default device)."), Settings.InputGain);
	}
	else if (IsSavedMicrophoneAvailable())
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Log,
			TEXT("Loaded saved microphone settings - '%s' is available (gain %.2f)."),
			*Settings.InputDeviceName, Settings.InputGain);
	}
	else
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Warning,
			TEXT("Saved microphone '%s' is not available - falling back to the system default. The preference is kept, so it will be used again once the device is back."),
			*Settings.InputDeviceName);
	}
}

UConvaiMicrophoneSubsystem* UConvaiMicrophoneSubsystem::Get(const UObject* WorldContextObject)
{
	if (!GEngine || !WorldContextObject)
	{
		return nullptr;
	}

	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
	if (!World)
	{
		return nullptr;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UConvaiMicrophoneSubsystem>() : nullptr;
}

void UConvaiMicrophoneSubsystem::SaveMicrophoneSettings(const FConvaiMicrophoneSettings& InSettings)
{
	Settings = InSettings;
	SaveToDisk();
	bLoadedFromDisk = true;

	OnMicrophoneSettingsChanged.Broadcast(Settings);
}

bool UConvaiMicrophoneSubsystem::SaveMicrophoneSettingsFromPlayerComponent(UConvaiPlayerComponent* PlayerComponent)
{
	if (!IsValid(PlayerComponent))
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Warning,
			TEXT("SaveMicrophoneSettingsFromPlayerComponent: PlayerComponent is not valid"));
		return false;
	}

	FConvaiMicrophoneSettings NewSettings = Settings;

	FCaptureDeviceInfoBP ActiveDevice;
	PlayerComponent->GetActiveCaptureDevice(ActiveDevice);
	if (ActiveDevice.DeviceName.IsEmpty())
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Warning,
			TEXT("SaveMicrophoneSettingsFromPlayerComponent: could not read the active capture device - keeping the previously saved device '%s'"),
			*NewSettings.InputDeviceName);
	}
	else
	{
		NewSettings.InputDeviceName = ActiveDevice.DeviceName;
	}

	float Gain = NewSettings.InputGain;
	bool bGainRead = false;
	PlayerComponent->GetMicrophoneVolumeMultiplier(Gain, bGainRead);
	if (bGainRead)
	{
		NewSettings.InputGain = Gain;
	}

	SaveMicrophoneSettings(NewSettings);

	CONVAI_LOG(ConvaiMicrophoneLog, Log,
		TEXT("Saved microphone settings - '%s', gain %.2f"), *Settings.InputDeviceName, Settings.InputGain);
	return true;
}

void UConvaiMicrophoneSubsystem::ResetMicrophoneSettingsToDefaults()
{
	Settings = FConvaiMicrophoneSettings{};
	bLoadedFromDisk = false;

	if (UGameplayStatics::DoesSaveGameExist(UConvaiMicrophoneSaveGame::SlotName, UConvaiMicrophoneSaveGame::UserIndex))
	{
		UGameplayStatics::DeleteGameInSlot(UConvaiMicrophoneSaveGame::SlotName, UConvaiMicrophoneSaveGame::UserIndex);
	}

	CONVAI_LOG(ConvaiMicrophoneLog, Log, TEXT("Cleared the saved microphone settings."));
	OnMicrophoneSettingsChanged.Broadcast(Settings);
}

TArray<FString> UConvaiMicrophoneSubsystem::GetAvailableMicrophoneNames() const
{
	TArray<FString> Names;
	for (const Audio::FCaptureDeviceInfo& Device : ProbeCaptureDevices())
	{
		Names.Add(Device.DeviceName);
	}
	return Names;
}

bool UConvaiMicrophoneSubsystem::IsSavedMicrophoneAvailable() const
{
	if (Settings.InputDeviceName.IsEmpty())
	{
		return false;
	}

	for (const Audio::FCaptureDeviceInfo& Device : ProbeCaptureDevices())
	{
		if (Device.DeviceName == Settings.InputDeviceName)
		{
			return true;
		}
	}
	return false;
}

bool UConvaiMicrophoneSubsystem::ApplySavedSettingsToPlayerComponent(UConvaiPlayerComponent* PlayerComponent) const
{
	if (!IsValid(PlayerComponent))
	{
		return false;
	}

	// Gain is device-independent, so it applies whether or not the saved mic is still around.
	bool bGainApplied = false;
	PlayerComponent->SetMicrophoneVolumeMultiplier(Settings.InputGain, bGainApplied);

	if (!IsSavedMicrophoneAvailable())
	{
		// Either nothing is saved, or the device is gone - both mean "leave it on the system
		// default". Initialize already logged the missing-device case.
		return false;
	}

	if (!PlayerComponent->SetCaptureDeviceByName(Settings.InputDeviceName))
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Warning,
			TEXT("Could not select the saved microphone '%s' - staying on the system default."),
			*Settings.InputDeviceName);
		return false;
	}

	CONVAI_LOG(ConvaiMicrophoneLog, Log,
		TEXT("Applied saved microphone '%s' (gain %.2f)."), *Settings.InputDeviceName, Settings.InputGain);
	return true;
}

UClass* UConvaiMicrophoneSubsystem::ResolveSaveGameClass()
{
	// TryFindTypeSlowSafe rather than LoadObject: the Helper Library's class is native, so if the
	// plugin is installed the class is already registered, and if it isn't there is nothing to load.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	if (UClass* HelperClass = UClass::TryFindTypeSlowSafe<UClass>(UConvaiMicrophoneSaveGame::HelperLibrarySaveGameClassPath))
#else
	if (UClass* HelperClass = FindObject<UClass>(ANY_PACKAGE, UConvaiMicrophoneSaveGame::HelperLibrarySaveGameClassPath))
#endif
	{
		if (HelperClass->IsChildOf(USaveGame::StaticClass()))
		{
			return HelperClass;
		}
	}
	return UConvaiMicrophoneSaveGame::StaticClass();
}

bool UConvaiMicrophoneSubsystem::ReadSharedRecord(const USaveGame* SaveObject, FConvaiMicrophoneSettings& OutSettings)
{
	FStructProperty* SettingsProperty = nullptr;
	void* SettingsValue = nullptr;
	if (!FindSettingsStruct(SaveObject, SettingsProperty, SettingsValue))
	{
		return false;
	}

	bool bReadAnything = false;

	if (const FStrProperty* DeviceNameProperty =
			FindFProperty<FStrProperty>(SettingsProperty->Struct, UConvaiMicrophoneSaveGame::DeviceNamePropertyName))
	{
		OutSettings.InputDeviceName = DeviceNameProperty->GetPropertyValue_InContainer(SettingsValue);
		bReadAnything = true;
	}

	if (const FFloatProperty* GainProperty =
			FindFProperty<FFloatProperty>(SettingsProperty->Struct, UConvaiMicrophoneSaveGame::GainPropertyName))
	{
		OutSettings.InputGain = GainProperty->GetPropertyValue_InContainer(SettingsValue);
		bReadAnything = true;
	}

	return bReadAnything;
}

bool UConvaiMicrophoneSubsystem::WriteSharedRecord(USaveGame* SaveObject, const FConvaiMicrophoneSettings& InSettings)
{
	FStructProperty* SettingsProperty = nullptr;
	void* SettingsValue = nullptr;
	if (!FindSettingsStruct(SaveObject, SettingsProperty, SettingsValue))
	{
		return false;
	}

	// Only these two fields are touched. Anything else in the record - the Helper Library's
	// AEC/VAD/noise-suppression flags, for instance - is left exactly as loaded.
	bool bWroteAnything = false;

	if (const FStrProperty* DeviceNameProperty =
			FindFProperty<FStrProperty>(SettingsProperty->Struct, UConvaiMicrophoneSaveGame::DeviceNamePropertyName))
	{
		DeviceNameProperty->SetPropertyValue_InContainer(SettingsValue, InSettings.InputDeviceName);
		bWroteAnything = true;
	}

	if (const FFloatProperty* GainProperty =
			FindFProperty<FFloatProperty>(SettingsProperty->Struct, UConvaiMicrophoneSaveGame::GainPropertyName))
	{
		GainProperty->SetPropertyValue_InContainer(SettingsValue, InSettings.InputGain);
		bWroteAnything = true;
	}

	return bWroteAnything;
}

bool UConvaiMicrophoneSubsystem::LoadFromDisk()
{
	if (!UGameplayStatics::DoesSaveGameExist(UConvaiMicrophoneSaveGame::SlotName, UConvaiMicrophoneSaveGame::UserIndex))
	{
		return false;
	}

	// LoadGameFromSlot resolves whichever class wrote the file, so this returns the Helper
	// Library's object when that plugin wrote it and ours when we did. ReadSharedRecord handles
	// either by property name.
	USaveGame* Loaded = UGameplayStatics::LoadGameFromSlot(UConvaiMicrophoneSaveGame::SlotName, UConvaiMicrophoneSaveGame::UserIndex);
	if (!Loaded)
	{
		// Most likely the file was written by the Helper Library and that plugin is no longer
		// installed: UGameplayStatics resolves the header's class path with a plain
		// UClass::TryFindTypeSlow, which cannot find a class from a removed module.
		CONVAI_LOG(ConvaiMicrophoneLog, Warning,
			TEXT("Save slot '%s' exists but its SaveGame class could not be resolved - using default microphone settings. The next save will rewrite it."),
			*UConvaiMicrophoneSaveGame::SlotName);
		return false;
	}

	FConvaiMicrophoneSettings LoadedSettings;
	if (!ReadSharedRecord(Loaded, LoadedSettings))
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Warning,
			TEXT("Save slot '%s' holds a '%s' record with no recognisable microphone fields - using defaults."),
			*UConvaiMicrophoneSaveGame::SlotName, *Loaded->GetClass()->GetName());
		return false;
	}

	Settings = LoadedSettings;
	bLoadedFromDisk = true;
	return true;
}

void UConvaiMicrophoneSubsystem::SaveToDisk() const
{
	UClass* TargetClass = ResolveSaveGameClass();

	// Start from what is already on disk when it is the same class, so the other plugin's fields
	// survive our write. Otherwise begin from a fresh instance.
	USaveGame* Record = UGameplayStatics::LoadGameFromSlot(UConvaiMicrophoneSaveGame::SlotName, UConvaiMicrophoneSaveGame::UserIndex);
	if (!Record || Record->GetClass() != TargetClass)
	{
		Record = NewObject<USaveGame>(GetTransientPackage(), TargetClass);
	}

	if (!Record)
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Warning, TEXT("Could not create a '%s' microphone save object."), *TargetClass->GetName());
		return;
	}

	if (!WriteSharedRecord(Record, Settings))
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Warning,
			TEXT("'%s' has no recognisable microphone fields - nothing was saved."), *TargetClass->GetName());
		return;
	}

	if (!UGameplayStatics::SaveGameToSlot(Record, UConvaiMicrophoneSaveGame::SlotName, UConvaiMicrophoneSaveGame::UserIndex))
	{
		CONVAI_LOG(ConvaiMicrophoneLog, Warning,
			TEXT("Could not write the microphone settings to slot '%s'."), *UConvaiMicrophoneSaveGame::SlotName);
	}
}
