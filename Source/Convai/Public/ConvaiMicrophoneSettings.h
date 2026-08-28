// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "ConvaiMicrophoneSettings.generated.h"

/**
 * The player's persisted microphone choice, owned by the SDK.
 *
 * Deliberately distinct in name from the Convai Helper Library's FConvaiAudioInputSettings so
 * both plugins can be installed together, but the FIELD names here match the Helper Library's
 * one-for-one on purpose: UConvaiMicrophoneSubsystem reads and writes the shared save record by
 * reflection, looking properties up by name, so the two stay interchangeable without either
 * plugin including the other's headers.
 *
 * Only the fields the SDK actually manages live here. The Helper Library's record carries extra
 * AEC/VAD/noise-suppression flags; those are preserved untouched when the SDK rewrites the file
 * (see UConvaiMicrophoneSubsystem::SaveToDisk).
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiMicrophoneSettings
{
	GENERATED_BODY()

	/** Empty = system default. Otherwise a DeviceName from UConvaiPlayerComponent::GetAvailableCaptureDeviceNames. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai|Microphone")
	FString InputDeviceName;

	/** Multiplier applied to mic input gain. 1.0 = unchanged. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai|Microphone",
		meta = (ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0"))
	float InputGain = 1.f;

	bool operator==(const FConvaiMicrophoneSettings& O) const
	{
		return InputDeviceName == O.InputDeviceName
			&& FMath::IsNearlyEqual(InputGain, O.InputGain, 0.001f);
	}
	bool operator!=(const FConvaiMicrophoneSettings& O) const { return !(*this == O); }
};

/**
 * Fallback container for the shared save record, used when the Convai Helper Library is not
 * installed. When it IS installed, UConvaiMicrophoneSubsystem writes an instance of the Helper
 * Library's own SaveGame class instead, so that plugin can still read the file natively --
 * UGameplayStatics stamps the writer's class path into the header and the Helper Library casts
 * the loaded object to its own type, so a file written under this class would be invisible to it.
 *
 * The property is named "Settings" and its struct's fields mirror the Helper Library's for the
 * same reason: one reflection path handles either class.
 */
UCLASS()
class CONVAI_API UConvaiMicrophoneSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FConvaiMicrophoneSettings Settings;

	/** Shared with the Helper Library - same slot, same user index, one file. */
	static const FString SlotName;
	static const int32   UserIndex;

	/** Class path of the Helper Library's SaveGame class, resolved at runtime when present. */
	static const TCHAR* HelperLibrarySaveGameClassPath;

	/** Property names the reflection bridge looks for. Identical in both plugins' records. */
	static const TCHAR* SettingsPropertyName;
	static const TCHAR* DeviceNamePropertyName;
	static const TCHAR* GainPropertyName;
};
