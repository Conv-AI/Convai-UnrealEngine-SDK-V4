// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Convai.h"
#include "Developer/Settings/Public/ISettingsModule.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "Utility/Log/ConvaiLogger.h"
#include "HAL/FileManager.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"

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

	// Get the plugin base directory using IPluginManager (works for both C++ and Blueprint-only projects)
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Convai"));
	if (!Plugin.IsValid())
	{
		CONVAI_LOG(LogConvai, Error, TEXT("Failed to find Convai plugin"));
		return;
	}

	const FString PluginBaseDir = Plugin->GetBaseDir();

	// Fallback: copy third-party libraries to Binaries if they are missing
	// (handles cases where Fab or other distribution methods don't copy them)
	EnsureThirdPartyLibrariesCopied(PluginBaseDir);

#if PLATFORM_WINDOWS
	// Load all DLLs from the plugin's Binaries directory
	FString DllDirectory = FPaths::Combine(PluginBaseDir, TEXT("Binaries/Win64"));

	TArray<FString> DllFiles;
	IFileManager::Get().FindFiles(DllFiles, *FPaths::Combine(DllDirectory, TEXT("*.dll")), true, false);

	// Push the DLL directory to the search path before loading
	FPlatformProcess::PushDllDirectory(*DllDirectory);

	for (const FString &DllFile : DllFiles)
	{
		// Skip UnrealEditor DLLs as they are loaded by the engine
		if (DllFile.StartsWith(TEXT("UnrealEditor")))
		{
			continue;
		}

		FString FullDllPath = FPaths::Combine(DllDirectory, DllFile);

		if (void *DllHandle = FPlatformProcess::GetDllHandle(*FullDllPath))
		{
			CONVAI_LOG(LogConvai, Log, TEXT("Successfully loaded %s"), *DllFile);
			ConvaiDllHandles.Add(DllFile, DllHandle);
		}
		else
		{
			CONVAI_LOG(LogConvai, Error, TEXT("Failed to load %s from %s"), *DllFile, *FullDllPath);
		}
	}

	// Pop the DLL directory from the search path
	FPlatformProcess::PopDllDirectory(*DllDirectory);

#elif PLATFORM_MAC
	// Load all dylib files from the plugin's Binaries directory
	FString DylibDirectory = FPaths::Combine(PluginBaseDir, TEXT("Binaries/Mac"));

	TArray<FString> DylibFiles;
	IFileManager::Get().FindFiles(DylibFiles, *FPaths::Combine(DylibDirectory, TEXT("*.dylib")), true, false);

	// Push the dylib directory to the search path before loading
	FPlatformProcess::PushDllDirectory(*DylibDirectory);

	for (const FString &DylibFile : DylibFiles)
	{
		// Skip UnrealEditor dylib as they are loaded by the engine
		if (DylibFile.StartsWith(TEXT("UnrealEditor")))
		{
			continue;
		}

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

	// Pop the dylib directory from the search path
	FPlatformProcess::PopDllDirectory(*DylibDirectory);

#elif PLATFORM_LINUX
	// Load all so files from the plugin's Binaries directory
	FString SoDirectory = FPaths::Combine(PluginBaseDir, TEXT("Binaries/Linux"));

	TArray<FString> SoFiles;
	IFileManager::Get().FindFiles(SoFiles, *FPaths::Combine(SoDirectory, TEXT("*.so")), true, false);

	// Push the so directory to the search path before loading
	FPlatformProcess::PushDllDirectory(*SoDirectory);

	for (const FString &SoFile : SoFiles)
	{
		// Skip UnrealEditor shared objects as they are loaded by the engine
		if (SoFile.StartsWith(TEXT("UnrealEditor")) || SoFile.StartsWith(TEXT("libUnrealEditor")))
		{
			continue;
		}

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

	// Pop the so directory from the search path
	FPlatformProcess::PopDllDirectory(*SoDirectory);
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

void Convai::EnsureThirdPartyLibrariesCopied(const FString& PluginBaseDir)
{
#if PLATFORM_WINDOWS
	const FString BinDir = FPaths::Combine(PluginBaseDir, TEXT("Binaries/Win64"));
	const FString ThirdPartyLibDir = FPaths::Combine(PluginBaseDir, TEXT("Source/ThirdParty/ConvaiWebRTC/lib/release/win64"));
	const FString SearchPattern = TEXT("*.dll");
#elif PLATFORM_MAC
	const FString BinDir = FPaths::Combine(PluginBaseDir, TEXT("Binaries/Mac"));
	const FString ThirdPartyLibDir = FPaths::Combine(PluginBaseDir, TEXT("Source/ThirdParty/ConvaiWebRTC/lib/release/mac"));
	const FString SearchPattern = TEXT("*.dylib");
#elif PLATFORM_LINUX
	const FString BinDir = FPaths::Combine(PluginBaseDir, TEXT("Binaries/Linux"));
	const FString ThirdPartyLibDir = FPaths::Combine(PluginBaseDir, TEXT("Source/ThirdParty/ConvaiWebRTC/lib/release/linux"));
	const FString SearchPattern = TEXT("*.so*");
#else
	return;
#endif

	if (!FPaths::DirectoryExists(ThirdPartyLibDir))
	{
		CONVAI_LOG(LogConvai, Warning, TEXT("Third party dir don't exist : %s"), *ThirdPartyLibDir);
		return;
	}

	TArray<FString> SourceFiles;
	IFileManager::Get().FindFiles(SourceFiles, *FPaths::Combine(ThirdPartyLibDir, SearchPattern), true, false);

	if (SourceFiles.Num() == 0)
	{
		CONVAI_LOG(LogConvai, Warning, TEXT("Convai webrtc dlls are missing"));
		return;
	}

	TArray<FString> MissingFiles;
	for (const FString& SourceFile : SourceFiles)
	{
		if (!FPaths::FileExists(FPaths::Combine(BinDir, SourceFile)))
		{
			MissingFiles.Add(SourceFile);
		}
	}

	if (MissingFiles.Num() == 0)
	{
		return;
	}

	IFileManager::Get().MakeDirectory(*BinDir, true);
	CONVAI_LOG(LogConvai, Warning, TEXT("Third-party libraries missing from Binaries (%d of %d), copying from %s as fallback"),
		MissingFiles.Num(), SourceFiles.Num(), *ThirdPartyLibDir);

	for (const FString& MissingFile : MissingFiles)
	{
		const FString SrcPath = FPaths::Combine(ThirdPartyLibDir, MissingFile);
		const FString DstPath = FPaths::Combine(BinDir, MissingFile);

		const uint32 CopyResult = IFileManager::Get().Copy(*DstPath, *SrcPath, true, true);
		if (CopyResult == COPY_OK)
		{
			CONVAI_LOG(LogConvai, Log, TEXT("Copied %s to %s"), *MissingFile, *BinDir);
		}
		else
		{
			CONVAI_LOG(LogConvai, Error, TEXT("Failed to copy %s to %s (Error: %u)"), *MissingFile, *DstPath, CopyResult);
		}
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
	TryUpdateDefaultConfigFile();
	UE_LOG(LogConvai, Log, TEXT("Convai settings saved to config"));
}

#undef LOCTEXT_NAMESPACE