// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiMicrophoneSettings.h"

// The slot the Convai Helper Library's ConvaiSettings module already uses. Sharing the slot name
// is what makes the two plugins share one file; the slot is just a string, so nothing collides.
const FString UConvaiMicrophoneSaveGame::SlotName  = TEXT("ConvaiAudioInputSettings");
const int32   UConvaiMicrophoneSaveGame::UserIndex = 0;

const TCHAR* UConvaiMicrophoneSaveGame::HelperLibrarySaveGameClassPath =
	TEXT("/Script/ConvaiSettings.ConvaiAudioInputSettingsSaveGame");

const TCHAR* UConvaiMicrophoneSaveGame::SettingsPropertyName   = TEXT("Settings");
const TCHAR* UConvaiMicrophoneSaveGame::DeviceNamePropertyName = TEXT("InputDeviceName");
const TCHAR* UConvaiMicrophoneSaveGame::GainPropertyName       = TEXT("InputGain");
