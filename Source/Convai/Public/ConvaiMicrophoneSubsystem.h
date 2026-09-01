// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ConvaiMicrophoneSettings.h"
#include "ConvaiMicrophoneSubsystem.generated.h"

class UConvaiPlayerComponent;
class USaveGame;

DECLARE_LOG_CATEGORY_EXTERN(ConvaiMicrophoneLog, Log, All);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnConvaiMicrophoneSettingsChanged, const FConvaiMicrophoneSettings&, NewSettings);

/**
 * Owns the microphone choice that survives across sessions, so the SDK no longer resets to the
 * system default every launch (UConvaiAudioCaptureComponent::SelectedDeviceIndex starts at -1).
 *
 * Self-sufficient: the SDK needs nothing from the Convai Helper Library. But when that plugin IS
 * installed, both read and write one shared record so a microphone picked in either UI is the one
 * used next launch. That interop is a reflection bridge, not a code dependency -- see
 * ResolveSaveGameClass and the Read/Write helpers -- because the Helper Library casts the loaded
 * object to its own SaveGame type, and UGameplayStatics stamps the writer's class into the header.
 *
 * Loading is deliberately forgiving about the saved device being gone: the preference stays on
 * disk and is simply not applied this run, so unplugging a USB mic for an afternoon doesn't lose
 * the choice.
 */
UCLASS(meta = (DisplayName = "Convai Microphone Subsystem"))
class CONVAI_API UConvaiMicrophoneSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	//~ End USubsystem interface

	UFUNCTION(BlueprintPure, Category = "Convai|Microphone", meta = (WorldContext = "WorldContextObject"))
	static UConvaiMicrophoneSubsystem* Get(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Convai|Microphone")
	FConvaiMicrophoneSettings GetMicrophoneSettings() const { return Settings; }

	/** True when a saved record was found on disk this session, as opposed to falling back to defaults. */
	UFUNCTION(BlueprintPure, Category = "Convai|Microphone")
	bool HasSavedMicrophoneSettings() const { return bLoadedFromDisk; }

	/** Replace the settings and write them to the shared record. Broadcasts OnMicrophoneSettingsChanged. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	void SaveMicrophoneSettings(const FConvaiMicrophoneSettings& InSettings);

	/**
	 * Snapshot PlayerComponent's live capture device and gain, then persist them.
	 * This is what the "Save Changes" button in MicSettings_WB ends up calling.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	bool SaveMicrophoneSettingsFromPlayerComponent(UConvaiPlayerComponent* PlayerComponent);

	/** Forget the saved choice and delete the shared record. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	void ResetMicrophoneSettingsToDefaults();

	/** Device names present right now, same naming as UConvaiPlayerComponent::GetAvailableCaptureDeviceNames. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	TArray<FString> GetAvailableMicrophoneNames() const;

	/**
	 * True when a device is saved AND it is plugged in right now. False with nothing saved is the
	 * normal "follow the system default" case, not an error.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	bool IsSavedMicrophoneAvailable() const;

	/**
	 * Apply the saved gain, plus the saved device if it is still available, to PlayerComponent.
	 * Returns true only when a saved device was resolved and selected.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")
	bool ApplySavedSettingsToPlayerComponent(UConvaiPlayerComponent* PlayerComponent) const;

	UPROPERTY(BlueprintAssignable, Category = "Convai|Microphone")
	FOnConvaiMicrophoneSettingsChanged OnMicrophoneSettingsChanged;

private:
	bool LoadFromDisk();
	void SaveToDisk() const;

	/**
	 * The Helper Library's SaveGame class when that plugin is installed, otherwise
	 * UConvaiMicrophoneSaveGame. Writing the Helper Library's class is the only way it can read
	 * what we wrote, since it hard-casts the loaded object to its own type.
	 */
	static UClass* ResolveSaveGameClass();

	/** Pull the device name and gain out of either plugin's record, by property name. */
	static bool ReadSharedRecord(const USaveGame* SaveObject, FConvaiMicrophoneSettings& OutSettings);

	/** Write the device name and gain into either plugin's record, leaving its other fields alone. */
	static bool WriteSharedRecord(USaveGame* SaveObject, const FConvaiMicrophoneSettings& InSettings);

	FConvaiMicrophoneSettings Settings;
	bool bLoadedFromDisk = false;
};
