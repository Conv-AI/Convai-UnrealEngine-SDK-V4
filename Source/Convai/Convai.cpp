// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Convai.h"
#include "Developer/Settings/Public/ISettingsModule.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "Utility/Log/ConvaiLogger.h"
#include "HAL/FileManager.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(Convai, Convai);

#define LOCTEXT_NAMESPACE "Convai"

DEFINE_LOG_CATEGORY(LogConvai);

void Convai::StartupModule()
{
	ConvaiSettings = NewObject<UConvaiSettings>(GetTransientPackage(), "ConvaiSettings", RF_Standalone);
	ConvaiSettings->AddToRoot();

	// Register settings
	if (ISettingsModule *SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->RegisterSettings("Project", "Plugins", "Convai",
										 LOCTEXT("RuntimeSettingsName", "Convai"),
										 LOCTEXT("RuntimeSettingsDescription", "Configure Convai settings"),
										 ConvaiSettings);
	}

#if PLATFORM_WINDOWS
	// Load all DLLs from the ThirdParty directory
	FString DllDirectory = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("Convai/Source/ThirdParty/ConvaiWebRTC/lib/win64"));

	TArray<FString> DllFiles;
	IFileManager::Get().FindFiles(DllFiles, *FPaths::Combine(DllDirectory, TEXT("*.dll")), true, false);

	for (const FString &DllFile : DllFiles)
	{
		FString FullDllPath = FPaths::Combine(DllDirectory, DllFile);
		void *DllHandle = FPlatformProcess::GetDllHandle(*FullDllPath);

		if (DllHandle)
		{
			CONVAI_LOG(LogConvai, Log, TEXT("Successfully loaded %s"), *DllFile);
			ConvaiDllHandles.Add(DllFile, DllHandle);
		}
		else
		{
			CONVAI_LOG(LogConvai, Error, TEXT("Failed to load %s from %s"), *DllFile, *FullDllPath);
		}
	}
#elif PLATFORM_MAC
	// Load all dylib files from the ThirdParty directory
	FString DylibDirectory = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("Convai/Source/ThirdParty/ConvaiWebRTC/lib/mac"));

	TArray<FString> DylibFiles;
	IFileManager::Get().FindFiles(DylibFiles, *FPaths::Combine(DylibDirectory, TEXT("*.dylib")), true, false);

	for (const FString &DylibFile : DylibFiles)
	{
		FString FullDylibPath = FPaths::Combine(DylibDirectory, DylibFile);
		void *DylibHandle = FPlatformProcess::GetDllHandle(*FullDylibPath);

		if (DylibHandle)
		{
			CONVAI_LOG(LogConvai, Log, TEXT("Successfully loaded %s"), *DylibFile);
			ConvaiDllHandles.Add(DylibFile, DylibHandle);
		}
		else
		{
			CONVAI_LOG(LogConvai, Error, TEXT("Failed to load %s from %s"), *DylibFile, *FullDylibPath);
		}
	}
#elif PLATFORM_LINUX
	// Load all so files from the ThirdParty directory
	FString SoDirectory = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("Convai/Source/ThirdParty/ConvaiWebRTC/lib/linux"));

	TArray<FString> SoFiles;
	IFileManager::Get().FindFiles(SoFiles, *FPaths::Combine(SoDirectory, TEXT("*.so")), true, false);

	for (const FString &SoFile : SoFiles)
	{
		FString FullSoPath = FPaths::Combine(SoDirectory, SoFile);
		void *SoHandle = FPlatformProcess::GetDllHandle(*FullSoPath);

		if (SoHandle)
		{
			CONVAI_LOG(LogConvai, Log, TEXT("Successfully loaded %s"), *SoFile);
			ConvaiDllHandles.Add(SoFile, SoHandle);
		}
		else
		{
			CONVAI_LOG(LogConvai, Error, TEXT("Failed to load %s from %s"), *SoFile, *FullSoPath);
		}
	}
#endif
}

void Convai::ShutdownModule()
{
	// Unload all dynamically loaded libraries
	for (auto &Pair : ConvaiDllHandles)
	{
		FPlatformProcess::FreeDllHandle(Pair.Value);
	}
	ConvaiDllHandles.Empty();

	if (UObjectInitialized())
	{
		ConvaiSettings->RemoveFromRoot();
	}

	// Unregister settings
	if (ISettingsModule *SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->UnregisterSettings("Project", "Plugins", "Convai");
	}

	if (!GExitPurge)
	{
		ConvaiSettings->RemoveFromRoot();
	}
	else
	{
		ConvaiSettings = nullptr;
	}
}

UConvaiSettings *Convai::GetConvaiSettings() const
{
	check(ConvaiSettings);
	return ConvaiSettings;
}

void UConvaiSettings::SetAPIKey(const FString &NewApiKey)
{
	API_Key = NewApiKey;
	SaveSettings();
}

void UConvaiSettings::SetAuthToken(const FString &NewAuthToken)
{
	AuthToken = NewAuthToken;
	SaveSettings();
}

void UConvaiSettings::SaveSettings()
{
	SaveConfig();
	UpdateDefaultConfigFile();
	UE_LOG(LogConvai, Log, TEXT("Convai settings saved to config"));
}

#undef LOCTEXT_NAMESPACE
