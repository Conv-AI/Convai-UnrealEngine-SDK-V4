/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConfigurationService.cpp
 *
 * Implementation of configuration management service.
 */

#include "Services/ConfigurationService.h"
#include "Convai/Convai.h" // Convai module interface and UConvaiSettings
#include "Logging/ConvaiEditorConfigLog.h"
#include "Misc/ConfigCacheIni.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Utility/ConvaiValidationUtils.h"
#include "Utility/ConvaiConstants.h"
#include "Utility/ConvaiConfigurationDefaults.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/Configuration/IConfigurationValidator.h"
#include "Misc/FileHelper.h"
#include "Models/ConvaiUserInfo.h"

const FString FConfigurationService::CONFIG_SECTION = TEXT("ConvaiEditor");
const FString FConfigurationService::CONFIG_FILE = TEXT("ConvaiEditorSettings");

const FString FConfigurationService::DEFAULT_THEME_ID = TEXT("Dark");
const int32 FConfigurationService::DEFAULT_WINDOW_WIDTH = static_cast<int32>(ConvaiEditor::Constants::Layout::Window::MainWindowWidth);
const int32 FConfigurationService::DEFAULT_WINDOW_HEIGHT = static_cast<int32>(ConvaiEditor::Constants::Layout::Window::MainWindowHeight);
const float FConfigurationService::DEFAULT_MIN_WINDOW_WIDTH = ConvaiEditor::Constants::Layout::Window::MainWindowMinWidth;
const float FConfigurationService::DEFAULT_MIN_WINDOW_HEIGHT = ConvaiEditor::Constants::Layout::Window::MainWindowMinHeight;

namespace ConvaiConfigRanges
{
    constexpr int32 MIN_WINDOW_DIMENSION = 55;
    constexpr int32 MAX_WINDOW_DIMENSION = 4096;
    constexpr float MIN_WINDOW_DIMENSION_F = 55.0f;
    constexpr float MAX_WINDOW_DIMENSION_F = 4096.0f;
}

FConfigurationService::FConfigurationService()
{
}

void FConfigurationService::Startup()
{
    CleanupOldBackups();
    EnsureConfigFileExists();
    InitializeDefaults();

    auto ValidatorResult = FConvaiDIContainerManager::Get().Resolve<IConfigurationValidator>();

    if (ValidatorResult.IsSuccess())
    {
        Validator = ValidatorResult.GetValue();
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Configuration validator unavailable"));
    }

    if (Validator.IsValid())
    {
        ValidateAndFixConfiguration();
    }

    InvalidateCache();
}

void FConfigurationService::Shutdown()
{
    UE_LOG(LogConvaiEditorConfig, Log, TEXT("ConfigurationService: Shutting down..."));
    SaveConfig();
    UE_LOG(LogConvaiEditorConfig, Log, TEXT("ConfigurationService: Shutdown complete"));
}

FString FConfigurationService::GetString(const FString &Key, const FString &Default) const
{
    EnsureCacheValid();

    FScopeLock Lock(&ConfigCacheLock);

    if (const FString *CachedValue = ConfigCache.Find(Key))
    {
        return *CachedValue;
    }

    EnsureConfigFileLoaded();

    FString Value;
    FString ConfigFilePath = GetConfigFilePath();
    if (GConfig->GetString(*CONFIG_SECTION, *Key, Value, ConfigFilePath))
    {
        ConfigCache.Add(Key, Value);
        return Value;
    }

    ConfigCache.Add(Key, Default);
    return Default;
}

int32 FConfigurationService::GetInt(const FString &Key, int32 Default) const
{
    const FString StringValue = GetString(Key, FString::FromInt(Default));

    if (StringValue.IsNumeric())
    {
        return FCString::Atoi(*StringValue);
    }

    UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Invalid integer value for '%s', using default %d"), *Key, Default);
    return Default;
}

float FConfigurationService::GetFloat(const FString &Key, float Default) const
{
    const FString StringValue = GetString(Key, FString::SanitizeFloat(Default));

    if (StringValue.IsNumeric())
    {
        return FCString::Atof(*StringValue);
    }

    UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Invalid float value for '%s', using default %.2f"), *Key, Default);
    return Default;
}

bool FConfigurationService::GetBool(const FString &Key, bool Default) const
{
    const FString StringValue = GetString(Key, Default ? TEXT("true") : TEXT("false"));

    if (StringValue == TEXT("true") || StringValue == TEXT("1"))
    {
        return true;
    }
    else if (StringValue == TEXT("false") || StringValue == TEXT("0"))
    {
        return false;
    }

    UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Invalid boolean value for '%s', using default %s"), *Key, Default ? TEXT("true") : TEXT("false"));
    return Default;
}

void FConfigurationService::SetString(const FString &Key, const FString &Value)
{
    bool bApiKeyChanged = (Key == ConvaiEditor::Constants::ConfigKey_ApiKey);
    bool bAuthTokenChanged = (Key == ConvaiEditor::Constants::ConfigKey_AuthToken);

    FString OldApiKey, OldAuthToken;
    if (bApiKeyChanged)
    {
        OldApiKey = GetString(Key, TEXT(""));
    }
    if (bAuthTokenChanged)
    {
        OldAuthToken = GetString(Key, TEXT(""));
    }

    {
        FScopeLock Lock(&ConfigCacheLock);
        ConfigCache.Add(Key, Value);
    }

    EnsureConfigFileLoaded();

    FString ConfigFilePath = GetConfigFilePath();
    GConfig->SetString(*CONFIG_SECTION, *Key, *Value, ConfigFilePath);
    OnConfigChangedDelegate.Broadcast(Key, Value);

    if (bApiKeyChanged && OldApiKey != Value)
    {
        OnApiKeyChangedDelegate.Broadcast(Value);
        NotifyAuthenticationChanged();
    }

    if (bAuthTokenChanged && OldAuthToken != Value)
    {
        OnAuthTokenChangedDelegate.Broadcast(Value);
        NotifyAuthenticationChanged();
    }
}

void FConfigurationService::SetInt(const FString &Key, int32 Value)
{
    SetString(Key, FString::FromInt(Value));
}

void FConfigurationService::SetFloat(const FString &Key, float Value)
{
    SetString(Key, FString::SanitizeFloat(Value));
}

void FConfigurationService::SetBool(const FString &Key, bool Value)
{
    SetString(Key, Value ? TEXT("true") : TEXT("false"));
}

FString FConfigurationService::GetApiKey() const
{
    if (Convai::IsAvailable())
    {
        UConvaiSettings *Settings = Convai::Get().GetConvaiSettings();
        if (Settings)
        {
            return Settings->API_Key;
        }
    }

    UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Convai module unavailable - cannot read API key"));
    return TEXT("");
}

void FConfigurationService::SetApiKey(const FString &ApiKey)
{
    if (Convai::IsAvailable())
    {
        UConvaiSettings *Settings = Convai::Get().GetConvaiSettings();
        if (Settings)
        {
            Settings->SetAPIKey(ApiKey);

            OnApiKeyChangedDelegate.Broadcast(ApiKey);
            NotifyAuthenticationChanged();

            return;
        }
    }

    UE_LOG(LogConvaiEditorConfig, Error, TEXT("Convai module unavailable - cannot set API key"));
}

FString FConfigurationService::GetAuthToken() const
{
    if (Convai::IsAvailable())
    {
        UConvaiSettings *Settings = Convai::Get().GetConvaiSettings();
        if (Settings)
        {
            return Settings->AuthToken;
        }
    }

    UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Convai module unavailable - cannot read Auth Token"));
    return TEXT("");
}

void FConfigurationService::SetAuthToken(const FString &AuthToken)
{
    if (Convai::IsAvailable())
    {
        UConvaiSettings *Settings = Convai::Get().GetConvaiSettings();
        if (Settings)
        {
            Settings->SetAuthToken(AuthToken);

            OnAuthTokenChangedDelegate.Broadcast(AuthToken);
            NotifyAuthenticationChanged();

            return;
        }
    }

    UE_LOG(LogConvaiEditorConfig, Error, TEXT("Convai module unavailable - cannot set Auth Token"));
}

TPair<FString, FString> FConfigurationService::GetAuthHeaderAndKey() const
{
    FString API_Key = GetApiKey();
    FString AuthToken = GetAuthToken();

    FString KeyOrToken;
    FString HeaderString;

    if (!API_Key.IsEmpty())
    {
        KeyOrToken = API_Key;
        HeaderString = ConvaiEditor::Constants::API_Key_Header;
    }
    else if (!AuthToken.IsEmpty())
    {
        KeyOrToken = AuthToken;
        HeaderString = ConvaiEditor::Constants::Auth_Token_Header;
    }
    else
    {
        KeyOrToken = TEXT("");
        HeaderString = TEXT("");
    }

    return TPair<FString, FString>(HeaderString, KeyOrToken);
}

bool FConfigurationService::HasApiKey() const
{
    return !GetApiKey().IsEmpty();
}

bool FConfigurationService::HasAuthToken() const
{
    return !GetAuthToken().IsEmpty();
}

bool FConfigurationService::HasAuthentication() const
{
    return HasApiKey() || HasAuthToken();
}

void FConfigurationService::ClearAuthentication()
{
    SetApiKey(TEXT(""));
    SetAuthToken(TEXT(""));
    ClearUserInfo();
}

void FConfigurationService::SetUserInfo(const FConvaiUserInfo &UserInfo)
{
    SetString(TEXT("userInfo.username"), UserInfo.Username);
    SetString(TEXT("userInfo.email"), UserInfo.Email);
    SaveConfig();
}

bool FConfigurationService::GetUserInfo(FConvaiUserInfo &OutUserInfo) const
{
    OutUserInfo.Username = GetString(TEXT("userInfo.username"), TEXT(""));
    OutUserInfo.Email = GetString(TEXT("userInfo.email"), TEXT(""));

    return OutUserInfo.IsValid();
}

void FConfigurationService::ClearUserInfo()
{
    SetString(TEXT("userInfo.username"), TEXT(""));
    SetString(TEXT("userInfo.email"), TEXT(""));
}

bool FConfigurationService::IsEditorUIEnabled() const
{
    return GetBool(TEXT("editorUI.enabled"), true);
}

FString FConfigurationService::GetThemeId() const
{
    return GetString(TEXT("theme.id"), DEFAULT_THEME_ID);
}

void FConfigurationService::SetThemeId(const FString &InThemeId)
{
    SetString(TEXT("theme.id"), InThemeId);
}

int32 FConfigurationService::GetWindowWidth() const
{
    const int32 Value = GetInt(TEXT("window.initialWidth"), DEFAULT_WINDOW_WIDTH);
    if (!FConvaiValidationUtils::IsIntInRange(Value, ConvaiConfigRanges::MIN_WINDOW_DIMENSION, ConvaiConfigRanges::MAX_WINDOW_DIMENSION, TEXT("window.initialWidth")))
    {
        return DEFAULT_WINDOW_WIDTH;
    }
    return Value;
}

int32 FConfigurationService::GetWindowHeight() const
{
    const int32 Value = GetInt(TEXT("window.initialHeight"), DEFAULT_WINDOW_HEIGHT);
    if (!FConvaiValidationUtils::IsIntInRange(Value, ConvaiConfigRanges::MIN_WINDOW_DIMENSION, ConvaiConfigRanges::MAX_WINDOW_DIMENSION, TEXT("window.initialHeight")))
    {
        return DEFAULT_WINDOW_HEIGHT;
    }
    return Value;
}

float FConfigurationService::GetMinWindowWidth() const
{
    const float Value = GetFloat(TEXT("window.minWidth"), DEFAULT_MIN_WINDOW_WIDTH);
    if (!FConvaiValidationUtils::IsFloatInRange(Value, ConvaiConfigRanges::MIN_WINDOW_DIMENSION_F, ConvaiConfigRanges::MAX_WINDOW_DIMENSION_F, TEXT("window.minWidth")))
    {
        return DEFAULT_MIN_WINDOW_WIDTH;
    }
    return Value;
}

float FConfigurationService::GetMinWindowHeight() const
{
    const float Value = GetFloat(TEXT("window.minHeight"), DEFAULT_MIN_WINDOW_HEIGHT);
    if (!FConvaiValidationUtils::IsFloatInRange(Value, ConvaiConfigRanges::MIN_WINDOW_DIMENSION_F, ConvaiConfigRanges::MAX_WINDOW_DIMENSION_F, TEXT("window.minHeight")))
    {
        return DEFAULT_MIN_WINDOW_HEIGHT;
    }
    return Value;
}

void FConfigurationService::SaveConfig()
{
    if (GConfig)
    {
        EnsureConfigFileLoaded();

        FString ConfigFilePath = GetConfigFilePath();
        GConfig->Flush(false, ConfigFilePath);
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("Configuration save failed - GConfig unavailable"));
    }
}

void FConfigurationService::ReloadConfig()
{
    InvalidateCache();

    if (GConfig)
    {
        FString ConfigFilePath = GetConfigFilePath();
        GConfig->LoadFile(ConfigFilePath);
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("Configuration reload failed - GConfig unavailable"));
    }
}

void FConfigurationService::ClearWindowDimensions()
{
    if (GConfig)
    {
        EnsureConfigFileLoaded();

        FString ConfigFilePath = GetConfigFilePath();
        GConfig->RemoveKey(*CONFIG_SECTION, TEXT("window.initialWidth"), ConfigFilePath);
        GConfig->RemoveKey(*CONFIG_SECTION, TEXT("window.initialHeight"), ConfigFilePath);
        GConfig->RemoveKey(*CONFIG_SECTION, TEXT("window.minWidth"), ConfigFilePath);
        GConfig->RemoveKey(*CONFIG_SECTION, TEXT("window.minHeight"), ConfigFilePath);
        SaveConfig();
    }

    UE_LOG(LogConvaiEditorConfig, Log, TEXT("Window dimensions cleared from config - will use constants on next startup"));
}

void FConfigurationService::EnsureConfigFileExists()
{
    FString ConfigFilePath = GetConfigFilePath();

    if (ConfigFilePath.IsEmpty())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("EnsureConfigFileExists: Unable to determine config file path"));
        return;
    }

    // Check if the file exists
    if (!FPaths::FileExists(ConfigFilePath))
    {
        bIsFirstTimeSetup = true;

        // Ensure the directory exists
        FString ConfigDir = FPaths::GetPath(ConfigFilePath);
        if (!IFileManager::Get().DirectoryExists(*ConfigDir))
        {
            if (!IFileManager::Get().MakeDirectory(*ConfigDir, true))
            {
                UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to create config directory: %s"), *ConfigDir);
                return;
            }
        }

        // Create an empty INI file with the section header
        FString InitialContent = TEXT("[ConvaiEditor]\n");
        if (!FFileHelper::SaveStringToFile(InitialContent, *ConfigFilePath))
        {
            UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to create config file: %s"), *ConfigFilePath);
            return;
        }

        UE_LOG(LogConvaiEditorConfig, Log, TEXT("Created new configuration file at: %s"), *ConfigFilePath);
    }

    // Ensure the config file is loaded into GConfig's cache
    EnsureConfigFileLoaded();
}

void FConfigurationService::EnsureConfigFileLoaded() const
{
    if (!GConfig)
    {
        return;
    }

    FString ConfigFilePath = GetConfigFilePath();
    if (ConfigFilePath.IsEmpty())
    {
        return;
    }

    // Check if the config file is already in GConfig's cache
    FConfigFile *ConfigFile = GConfig->FindConfigFile(ConfigFilePath);
    if (!ConfigFile)
    {
        // Load the file into GConfig's cache
        GConfig->LoadFile(ConfigFilePath);
    }
}

void FConfigurationService::InitializeDefaults()
{
    using namespace ConvaiEditor::Configuration::Defaults;

    FString ConfigFilePath = GetConfigFilePath();

    if (!GConfig)
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("InitializeDefaults: GConfig unavailable"));
        return;
    }

    EnsureConfigFileLoaded();

    // Ensure configuration file exists with all default values
    bool bNeedsSave = false;

    // Editor UI settings
    bool bExistingEditorUIEnabled;
    if (!GConfig->GetBool(*CONFIG_SECTION, *Keys::EDITOR_UI_ENABLED, bExistingEditorUIEnabled, ConfigFilePath))
    {
        SetBool(Keys::EDITOR_UI_ENABLED, Values::EDITOR_UI_ENABLED);
        bNeedsSave = true;
    }

    // Window settings
    int32 ExistingWidth;
    if (!GConfig->GetInt(*CONFIG_SECTION, *Keys::WINDOW_INITIAL_WIDTH, ExistingWidth, ConfigFilePath))
    {
        SetInt(Keys::WINDOW_INITIAL_WIDTH, Values::WINDOW_INITIAL_WIDTH);
        bNeedsSave = true;
    }

    int32 ExistingHeight;
    if (!GConfig->GetInt(*CONFIG_SECTION, *Keys::WINDOW_INITIAL_HEIGHT, ExistingHeight, ConfigFilePath))
    {
        SetInt(Keys::WINDOW_INITIAL_HEIGHT, Values::WINDOW_INITIAL_HEIGHT);
        bNeedsSave = true;
    }

    float ExistingMinWidth;
    if (!GConfig->GetFloat(*CONFIG_SECTION, *Keys::WINDOW_MIN_WIDTH, ExistingMinWidth, ConfigFilePath))
    {
        SetFloat(Keys::WINDOW_MIN_WIDTH, Values::WINDOW_MIN_WIDTH);
        bNeedsSave = true;
    }

    float ExistingMinHeight;
    if (!GConfig->GetFloat(*CONFIG_SECTION, *Keys::WINDOW_MIN_HEIGHT, ExistingMinHeight, ConfigFilePath))
    {
        SetFloat(Keys::WINDOW_MIN_HEIGHT, Values::WINDOW_MIN_HEIGHT);
        bNeedsSave = true;
    }

    // Theme settings
    FString ExistingTheme;
    if (!GConfig->GetString(*CONFIG_SECTION, *Keys::THEME_ID, ExistingTheme, ConfigFilePath))
    {
        SetString(Keys::THEME_ID, Values::THEME_ID);
        bNeedsSave = true;
    }

    // Navigation settings
    int32 ExistingMaxHistorySize;
    if (!GConfig->GetInt(*CONFIG_SECTION, *Keys::NAVIGATION_MAX_HISTORY_SIZE, ExistingMaxHistorySize, ConfigFilePath))
    {
        SetInt(Keys::NAVIGATION_MAX_HISTORY_SIZE, Values::NAVIGATION_MAX_HISTORY_SIZE);
        bNeedsSave = true;
    }

    // Meta settings
    int32 ExistingConfigVersion;
    if (!GConfig->GetInt(*CONFIG_SECTION, *Keys::META_CONFIG_VERSION, ExistingConfigVersion, ConfigFilePath))
    {
        SetInt(Keys::META_CONFIG_VERSION, CURRENT_SCHEMA_VERSION);
        bNeedsSave = true;
    }

    if (bNeedsSave)
    {
        SaveConfig();

        if (bIsFirstTimeSetup)
        {
            UE_LOG(LogConvaiEditorConfig, Log, TEXT("Configuration initialized with default values"));
        }
        else
        {
            UE_LOG(LogConvaiEditorConfig, Log, TEXT("Configuration updated with missing default values"));
        }
    }
}

FString FConfigurationService::GetConfigFilePath() const
{
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Convai"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("Convai plugin not found"));
        return TEXT("");
    }

    FString ConfigFilePath = FPaths::Combine(
        Plugin->GetBaseDir(),
        TEXT("Config"),
        TEXT("ConvaiEditorSettings.ini"));

    return FPaths::ConvertRelativePathToFull(ConfigFilePath);
}

void FConfigurationService::NotifyAuthenticationChanged()
{
    OnAuthenticationChangedDelegate.Broadcast();
}

void FConfigurationService::EnsureCacheValid() const
{
    FScopeLock Lock(&ConfigCacheLock);

    if (!bCacheValid)
    {
        ConfigCache.Empty();
        bCacheValid = true;
    }
}

void FConfigurationService::InvalidateCache()
{
    FScopeLock Lock(&ConfigCacheLock);
    bCacheValid = false;
}

void FConfigurationService::ValidateAndFixConfiguration()
{
    if (!Validator.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Configuration validation skipped - validator unavailable"));
        return;
    }

    TSharedPtr<IConfigurationValidator> ValidatorPtr = Validator.Pin();
    if (!ValidatorPtr.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Validator service expired, skipping validation"));
        return;
    }

    FString ConfigFilePath = GetConfigFilePath();

    if (!FPaths::FileExists(ConfigFilePath))
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("INI config file not found"));
        return;
    }

    FConfigValidationResult ValidationResult = ValidatorPtr->ValidateFile(ConfigFilePath);

    UE_LOG(LogConvaiEditorConfig, Log, TEXT("Configuration validation completed - %d issues found"),
           ValidationResult.Issues.Num());

    if (!ValidationResult.bIsValid)
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Configuration validation failed - %d issues found"),
               ValidationResult.Issues.Num());

        for (const FConfigValidationIssue &Issue : ValidationResult.Issues)
        {
            FString SeverityStr;
            switch (Issue.Severity)
            {
            case EConfigValidationSeverity::Critical:
                SeverityStr = TEXT("CRITICAL");
                break;
            case EConfigValidationSeverity::Error:
                SeverityStr = TEXT("ERROR");
                break;
            case EConfigValidationSeverity::Warning:
                SeverityStr = TEXT("WARNING");
                break;
            case EConfigValidationSeverity::Info:
                SeverityStr = TEXT("INFO");
                break;
            default:
                SeverityStr = TEXT("UNKNOWN");
                break;
            }

            UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  [%s] Key='%s': %s"),
                   *SeverityStr, *Issue.Key, *Issue.Message);

            if (!Issue.ActualValue.IsEmpty())
            {
                UE_LOG(LogConvaiEditorConfig, Warning, TEXT("    Actual value: '%s'"), *Issue.ActualValue);
            }
            if (!Issue.ExpectedValue.IsEmpty())
            {
                UE_LOG(LogConvaiEditorConfig, Warning, TEXT("    Expected value: '%s'"), *Issue.ExpectedValue);
            }
            if (!Issue.SuggestedFix.IsEmpty())
            {
                UE_LOG(LogConvaiEditorConfig, Warning, TEXT("    Suggested fix: %s"), *Issue.SuggestedFix);
            }
        }

        if (ValidationResult.bShouldFallback)
        {
            UE_LOG(LogConvaiEditorConfig, Error, TEXT("Critical configuration error - resetting to defaults"));
            ResetToDefaults();
        }
    }
}

void FConfigurationService::ResetToDefaults()
{
    using namespace ConvaiEditor::Configuration::Defaults;

    SetBool(Keys::EDITOR_UI_ENABLED, Values::EDITOR_UI_ENABLED);
    SetInt(Keys::WINDOW_INITIAL_WIDTH, Values::WINDOW_INITIAL_WIDTH);
    SetInt(Keys::WINDOW_INITIAL_HEIGHT, Values::WINDOW_INITIAL_HEIGHT);
    SetFloat(Keys::WINDOW_MIN_WIDTH, Values::WINDOW_MIN_WIDTH);
    SetFloat(Keys::WINDOW_MIN_HEIGHT, Values::WINDOW_MIN_HEIGHT);
    SetString(Keys::THEME_ID, Values::THEME_ID);
    SetInt(Keys::NAVIGATION_MAX_HISTORY_SIZE, Values::NAVIGATION_MAX_HISTORY_SIZE);
    SetInt(Keys::META_CONFIG_VERSION, CURRENT_SCHEMA_VERSION);

    SaveConfig();
}

void FConfigurationService::CleanupOldBackups() const
{
    FString ConfigFilePath = GetConfigFilePath();
    FString ConfigDir = FPaths::GetPath(ConfigFilePath);
    FString ConfigName = FPaths::GetBaseFilename(ConfigFilePath);

    TArray<FString> BackupFiles;
    IFileManager::Get().FindFiles(BackupFiles, *FPaths::Combine(ConfigDir, TEXT("*.backup")), true, false);

    int32 CleanedCount = 0;
    for (const FString &BackupFile : BackupFiles)
    {
        if (BackupFile.Contains(ConfigName))
        {
            FString FullPath = FPaths::Combine(ConfigDir, BackupFile);
            if (IFileManager::Get().Delete(*FullPath, false, true))
            {
                CleanedCount++;
            }
        }
    }

    if (CleanedCount > 0)
    {
        UE_LOG(LogConvaiEditorConfig, Log, TEXT("Cleaned up %d old backup files"), CleanedCount);
    }
}
