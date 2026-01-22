// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Public/ConvaiDefinitions.h"
#include "Convai.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogConvai, Log, All);

UCLASS(config = Engine, defaultconfig)
class CONVAI_API UConvaiSettings : public UObject
{
	GENERATED_BODY()

public:
	UConvaiSettings(const FObjectInitializer &ObjectInitializer)
		: Super(ObjectInitializer)
	{
		API_Key = "";
		EnableNewActionSystem = false;
	}
	/* API Key Issued from the website (Managed automatically by Convai Editor UI - Read Only) */
	UPROPERTY(Config, VisibleAnywhere, Category = "Convai", meta = (DisplayName = "API Key"))
	FString API_Key;

	/* Enable new actions system */
	UPROPERTY(Config, EditAnywhere, Category = "Convai", meta = (DisplayName = "Enable New Action System (Experimental)"))
	bool EnableNewActionSystem;

	/* Authentication token used for Convai Connect (Managed automatically by Convai Editor UI - Read Only) */
	UPROPERTY(Config, VisibleAnywhere, AdvancedDisplay, Category = "Convai", meta = (DisplayName = "Auth Token"))
	FString AuthToken;

	/* Custom Server URL (Used for debugging) */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	FString CustomURL;

	/* Custom Beta API URL (Used for debugging) */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	FString CustomBetaURL;

	/* Custom Production API URL (Used for debugging) */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	FString CustomProdURL;

	/* Test Character ID (Used for debugging) */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	FString TestCharacterID;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	bool AllowInsecureConnection;

	/*
	 * Forces the AI to include vision parameters in its initial connection setup,
	 * allowing vision components set after Begin Play to function properly.
	 */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	bool AlwaysAllowVision;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	EC_LipSyncMode LipSyncMode = EC_LipSyncMode::Auto;

	/* Extra Parameters (Used for debugging) */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	FString ExtraParams;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	TMap<FString, FString> CustomPrams;

	UPROPERTY(Config, VisibleAnywhere, Category = "Long Term Memory")
	TArray<FConvaiSpeakerInfo> SpeakerIDs;

	/** Programmatically set API key and save to config (Used by Editor UI) */
	void SetAPIKey(const FString &NewApiKey);

	/** Programmatically set Auth Token and save to config (Used by Editor UI) */
	void SetAuthToken(const FString &NewAuthToken);

	/** Save settings to config file */
	void SaveSettings();
};

class CONVAI_API Convai : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	void StartupModule();
	void ShutdownModule();

	/**
	 * Singleton-like access to this module's interface.  This is just for convenience!
	 * Beware of calling this during the shutdown phase, though.  Your module might have been unloaded already.
	 *
	 * @return Returns singleton instance, loading the module on demand if needed
	 */
	static inline Convai &Get()
	{
		return FModuleManager::LoadModuleChecked<Convai>("Convai");
	}

	/**
	 * Checks to see if this module is loaded and ready.  It is only valid to call Get() if IsAvailable() returns true.
	 *
	 * @return True if the module is loaded and ready to use
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("Convai");
	}

	virtual bool IsGameModule() const override
	{
		return true;
	}

	/** Getter for internal settings object to support runtime configuration changes */
	UConvaiSettings *GetConvaiSettings() const;

protected:
	/** Module settings */
	UConvaiSettings *ConvaiSettings;

	// Map to store handles to dynamically loaded DLLs
	TMap<FString, void *> ConvaiDllHandles;
};
