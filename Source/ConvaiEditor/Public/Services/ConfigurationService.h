/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConfigurationService.h
 *
 * Service for plugin configuration management.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Utility/ConvaiConstants.h"
#include "Services/Configuration/IConfigurationReader.h"
#include "Services/Configuration/IConfigurationWriter.h"
#include "Services/Configuration/IAuthProvider.h"
#include "Services/Configuration/IThemeProvider.h"

// Forward declarations for cross-module dependencies
class Convai;
class UConvaiSettings;

/** Interface for configuration management. */
class CONVAIEDITOR_API IConfigurationService : public IConvaiService
{
public:
    virtual ~IConfigurationService() = default;

    virtual FString GetString(const FString &Key, const FString &Default = FString()) const = 0;
    virtual int32 GetInt(const FString &Key, int32 Default = 0) const = 0;
    virtual float GetFloat(const FString &Key, float Default = 0.0f) const = 0;
    virtual bool GetBool(const FString &Key, bool Default = false) const = 0;

    virtual void SetString(const FString &Key, const FString &Value) = 0;
    virtual void SetInt(const FString &Key, int32 Value) = 0;
    virtual void SetFloat(const FString &Key, float Value) = 0;
    virtual void SetBool(const FString &Key, bool Value) = 0;

    virtual FString GetApiKey() const = 0;
    virtual void SetApiKey(const FString &ApiKey) = 0;
    virtual FString GetAuthToken() const = 0;
    virtual void SetAuthToken(const FString &AuthToken) = 0;
    virtual TPair<FString, FString> GetAuthHeaderAndKey() const = 0;
    virtual bool HasApiKey() const = 0;
    virtual bool HasAuthToken() const = 0;
    virtual bool HasAuthentication() const = 0;
    virtual void ClearAuthentication() = 0;

    virtual void SetUserInfo(const struct FConvaiUserInfo &UserInfo) = 0;
    virtual bool GetUserInfo(struct FConvaiUserInfo &OutUserInfo) const = 0;
    virtual void ClearUserInfo() = 0;

    virtual FString GetThemeId() const = 0;
    virtual void SetThemeId(const FString &InThemeId) = 0;
    virtual int32 GetWindowWidth() const = 0;
    virtual int32 GetWindowHeight() const = 0;
    virtual float GetMinWindowWidth() const = 0;
    virtual float GetMinWindowHeight() const = 0;

    virtual void SaveConfig() = 0;
    virtual void ReloadConfig() = 0;
    virtual void ClearWindowDimensions() = 0;

    DECLARE_MULTICAST_DELEGATE_TwoParams(FOnConfigChanged, const FString & /*Key*/, const FString & /*Value*/);
    virtual FOnConfigChanged &OnConfigChanged() = 0;

    DECLARE_MULTICAST_DELEGATE_OneParam(FOnApiKeyChanged, const FString & /*NewApiKey*/);
    virtual FOnApiKeyChanged &OnApiKeyChanged() = 0;

    DECLARE_MULTICAST_DELEGATE_OneParam(FOnAuthTokenChanged, const FString & /*NewAuthToken*/);
    virtual FOnAuthTokenChanged &OnAuthTokenChanged() = 0;

    DECLARE_MULTICAST_DELEGATE(FOnAuthenticationChanged);
    virtual FOnAuthenticationChanged &OnAuthenticationChanged() = 0;

    static FName StaticType() { return TEXT("IConfigurationService"); }
};

/** Configuration service implementation with authentication and theme management. */
class CONVAIEDITOR_API FConfigurationService : public IConfigurationService,
                                               public IConfigurationReader,
                                               public IConfigurationWriter,
                                               public IAuthProvider,
                                               public IThemeProvider
{
public:
    FConfigurationService();
    virtual ~FConfigurationService() = default;

    virtual void Startup() override;
    virtual void Shutdown() override;

    virtual FString GetString(const FString &Key, const FString &Default = FString()) const override;
    virtual int32 GetInt(const FString &Key, int32 Default = 0) const override;
    virtual float GetFloat(const FString &Key, float Default = 0.0f) const override;
    virtual bool GetBool(const FString &Key, bool Default = false) const override;

    virtual void SetString(const FString &Key, const FString &Value) override;
    virtual void SetInt(const FString &Key, int32 Value) override;
    virtual void SetFloat(const FString &Key, float Value) override;
    virtual void SetBool(const FString &Key, bool Value) override;

    virtual FString GetApiKey() const override;
    virtual void SetApiKey(const FString &ApiKey) override;
    virtual FString GetAuthToken() const override;
    virtual void SetAuthToken(const FString &AuthToken) override;
    virtual TPair<FString, FString> GetAuthHeaderAndKey() const override;
    virtual bool HasApiKey() const override;
    virtual bool HasAuthToken() const override;
    virtual bool HasAuthentication() const override;
    virtual void ClearAuthentication() override;

    virtual void SetUserInfo(const struct FConvaiUserInfo &UserInfo) override;
    virtual bool GetUserInfo(struct FConvaiUserInfo &OutUserInfo) const override;
    virtual void ClearUserInfo() override;

    virtual FString GetThemeId() const override;
    virtual void SetThemeId(const FString &InThemeId) override;
    virtual int32 GetWindowWidth() const override;
    virtual int32 GetWindowHeight() const override;
    virtual float GetMinWindowWidth() const override;
    virtual float GetMinWindowHeight() const override;

    virtual void SaveConfig() override;
    virtual void ReloadConfig() override;
    virtual void ClearWindowDimensions() override;

    virtual FOnConfigChanged &OnConfigChanged() override { return OnConfigChangedDelegate; }
    virtual FOnApiKeyChanged &OnApiKeyChanged() override { return OnApiKeyChangedDelegate; }
    virtual FOnAuthTokenChanged &OnAuthTokenChanged() override { return OnAuthTokenChangedDelegate; }
    virtual FOnAuthenticationChanged &OnAuthenticationChanged() override { return OnAuthenticationChangedDelegate; }

    static FName StaticType() { return TEXT("IConfigurationService"); }

private:
    static const FString CONFIG_SECTION;
    static const FString CONFIG_FILE;
    static const FString DEFAULT_THEME_ID;
    static const int32 DEFAULT_WINDOW_WIDTH;
    static const int32 DEFAULT_WINDOW_HEIGHT;
    static const float DEFAULT_MIN_WINDOW_WIDTH;
    static const float DEFAULT_MIN_WINDOW_HEIGHT;

    void InitializeDefaults();
    void EnsureConfigFileExists();
    FString GetConfigFilePath() const;
    void NotifyAuthenticationChanged();
    void ValidateAndFixConfiguration();
    void ResetToDefaults();
    bool CreateConfigBackup() const;

    FOnConfigChanged OnConfigChangedDelegate;
    FOnApiKeyChanged OnApiKeyChangedDelegate;
    FOnAuthTokenChanged OnAuthTokenChangedDelegate;
    FOnAuthenticationChanged OnAuthenticationChangedDelegate;

    mutable FCriticalSection ConfigCacheLock;
    mutable TMap<FString, FString> ConfigCache;
    mutable bool bCacheValid = false;

    void EnsureCacheValid() const;
    void InvalidateCache();
    void CleanupOldBackups() const;

    TWeakPtr<class IConfigurationValidator> Validator;
};
