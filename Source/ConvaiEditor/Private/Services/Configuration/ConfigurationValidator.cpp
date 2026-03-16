/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConfigurationValidator.cpp
 *
 * Implementation of configuration validation system
 */

#include "Services/Configuration/ConfigurationValidator.h"
#include "Utility/ConvaiConfigurationDefaults.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "Misc/FileHelper.h"

using namespace ConvaiEditor::Configuration::Defaults;

FConfigurationValidator::FConfigurationValidator()
    : Schema(BuildDefaultSchema())
{
}

void FConfigurationValidator::Startup()
{
}

void FConfigurationValidator::Shutdown()
{
}

FConfigValidationResult FConfigurationValidator::Validate(const TSharedPtr<FJsonObject> &ConfigJson) const
{
    FConfigValidationResult Result;

    if (!ConfigJson.IsValid())
    {
        Result.AddIssue(FConfigValidationIssue(
            EConfigValidationSeverity::Critical,
            TEXT("root"),
            TEXT("Configuration JSON is null or invalid"),
            false));
        return Result;
    }

    Result.ConfigVersion = ValidateVersion(ConfigJson, Result);
    ValidateRequiredKeys(ConfigJson, Result);
    ValidateAllKeys(ConfigJson, Result);
    ValidateNoUnknownKeys(ConfigJson, Result);
    Result.bCanAutoFix = CanAutoFixResult(Result);

    return Result;
}

FConfigValidationResult FConfigurationValidator::ValidateFile(const FString &ConfigFilePath) const
{
    FConfigValidationResult Result;

    if (!FPaths::FileExists(ConfigFilePath))
    {
        Result.AddIssue(FConfigValidationIssue(
            EConfigValidationSeverity::Critical,
            TEXT("file"),
            FString::Printf(TEXT("Configuration file not found: %s"), *ConfigFilePath),
            false));
        return Result;
    }

    if (ConfigFilePath.EndsWith(TEXT(".ini")))
    {
        return ValidateIniFile(ConfigFilePath);
    }
    else
    {
        Result.AddIssue(FConfigValidationIssue(
            EConfigValidationSeverity::Critical,
            TEXT("file"),
            TEXT("Unsupported configuration file format. Only .ini files are supported."),
            false));
        return Result;
    }
}

FConfigValidationResult FConfigurationValidator::ValidateIniFile(const FString &ConfigFilePath) const
{
    FConfigValidationResult Result;

    FConfigCacheIni *ConfigCache = GConfig;
    if (!ConfigCache)
    {
        Result.AddIssue(FConfigValidationIssue(
            EConfigValidationSeverity::Critical,
            TEXT("config"),
            TEXT("Configuration system not available"),
            false));
        return Result;
    }

    for (const auto &KeyTypePair : Schema.ExpectedTypes)
    {
        const FString &Key = KeyTypePair.Key;
        const FString &ExpectedType = KeyTypePair.Value;

        FString Value;
        if (ConfigCache->GetString(TEXT("ConvaiEditor"), *Key, Value, ConfigFilePath))
        {
            TOptional<FConfigValidationIssue> Issue = ValidateKeyValue(Key, Value);
            if (Issue.IsSet())
            {
                Result.AddIssue(Issue.GetValue());
            }
        }
        else if (Schema.RequiredKeys.Contains(Key))
        {
            Result.AddIssue(FConfigValidationIssue(
                EConfigValidationSeverity::Error,
                Key,
                TEXT("Required configuration key is missing"),
                true));
        }
    }

    return Result;
}

bool FConfigurationValidator::AutoFix(TSharedPtr<FJsonObject> &ConfigJson, const FConfigValidationResult &ValidationResult) const
{
    if (!ConfigJson.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("ConfigurationValidator: cannot auto-fix null configuration"));
        return false;
    }

    if (!ValidationResult.bCanAutoFix)
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("ConfigurationValidator: configuration has issues that cannot be auto-fixed"));
        return false;
    }

    int32 FixedCount = 0;

    for (const FConfigValidationIssue &Issue : ValidationResult.Issues)
    {
        if (Issue.bCanAutoFix)
        {
            if (TryFixIssue(ConfigJson, Issue))
            {
                FixedCount++;
            }
        }
    }

    return FixedCount > 0;
}

const FConfigurationSchema &FConfigurationValidator::GetSchema() const
{
    return Schema;
}

int32 FConfigurationValidator::GetCurrentSchemaVersion() const
{
    return Schema.Version;
}

bool FConfigurationValidator::NeedsMigration(int32 ConfigVersion) const
{
    return ConfigVersion < Schema.Version;
}

TOptional<FConfigValidationIssue> FConfigurationValidator::ValidateKeyValue(const FString &Key, const FString &Value) const
{
    if (!Schema.ExpectedTypes.Contains(Key))
    {
        return FConfigValidationIssue(
            EConfigValidationSeverity::Warning,
            Key,
            TEXT("Unknown configuration key"),
            false);
    }

    const FString &ExpectedType = Schema.ExpectedTypes[Key];

    if (!ValidateValueType(Value, ExpectedType))
    {
        FConfigValidationIssue Issue(
            EConfigValidationSeverity::Error,
            Key,
            FString::Printf(TEXT("Invalid type. Expected: %s"), *ExpectedType),
            true);
        Issue.ExpectedValue = ExpectedType;
        Issue.ActualValue = Value;
        return Issue;
    }

    if (Schema.Constraints.Contains(Key))
    {
        const FString &Constraint = Schema.Constraints[Key];
        if (!ValidateConstraint(Value, Constraint))
        {
            FConfigValidationIssue Issue(
                EConfigValidationSeverity::Error,
                Key,
                FString::Printf(TEXT("Value violates constraint: %s"), *Constraint),
                true);
            Issue.ActualValue = Value;
            return Issue;
        }
    }

    return TOptional<FConfigValidationIssue>();
}

FString FConfigurationValidator::GetDefaultValue(const FString &Key) const
{
    if (Schema.Defaults.Contains(Key))
    {
        return Schema.Defaults[Key];
    }
    return FString();
}

bool FConfigurationValidator::IsRequiredKey(const FString &Key) const
{
    return Schema.RequiredKeys.Contains(Key);
}

FString FConfigurationValidator::GetExpectedType(const FString &Key) const
{
    if (Schema.ExpectedTypes.Contains(Key))
    {
        return Schema.ExpectedTypes[Key];
    }
    return FString();
}

int32 FConfigurationValidator::ValidateVersion(const TSharedPtr<FJsonObject> &ConfigJson, FConfigValidationResult &Result) const
{
    const FString VersionKey = Keys::META_CONFIG_VERSION;

    if (!ConfigJson->HasField(VersionKey))
    {
        Result.AddIssue(FConfigValidationIssue(
            EConfigValidationSeverity::Warning,
            VersionKey,
            TEXT("Configuration version not specified. Assuming version 1."),
            true));
        Result.bNeedsMigration = true;
        return 1;
    }

    int32 ConfigVersion = static_cast<int32>(ConfigJson->GetNumberField(VersionKey));

    if (ConfigVersion < 1)
    {
        Result.AddIssue(FConfigValidationIssue(
            EConfigValidationSeverity::Error,
            VersionKey,
            FString::Printf(TEXT("Invalid configuration version: %d"), ConfigVersion),
            true));
        return 1;
    }

    if (ConfigVersion > Schema.Version)
    {
        Result.AddIssue(FConfigValidationIssue(
            EConfigValidationSeverity::Warning,
            VersionKey,
            FString::Printf(TEXT("Configuration version (%d) is newer than schema version (%d). Some features may not work."),
                            ConfigVersion, Schema.Version),
            false));
    }
    else if (ConfigVersion < Schema.Version)
    {
        Result.AddIssue(FConfigValidationIssue(
            EConfigValidationSeverity::Info,
            VersionKey,
            FString::Printf(TEXT("Configuration version (%d) is older than schema version (%d). Migration available."),
                            ConfigVersion, Schema.Version),
            true));
        Result.bNeedsMigration = true;
    }

    return ConfigVersion;
}

void FConfigurationValidator::ValidateRequiredKeys(const TSharedPtr<FJsonObject> &ConfigJson, FConfigValidationResult &Result) const
{
    for (const FString &RequiredKey : Schema.RequiredKeys)
    {
        if (!ConfigJson->HasField(RequiredKey))
        {
            FConfigValidationIssue Issue(
                EConfigValidationSeverity::Error,
                RequiredKey,
                TEXT("Required configuration key is missing"),
                true);
            Issue.SuggestedFix = FString::Printf(TEXT("Add default value: %s"), *GetDefaultValue(RequiredKey));
            Result.AddIssue(Issue);
        }
    }
}

void FConfigurationValidator::ValidateAllKeys(const TSharedPtr<FJsonObject> &ConfigJson, FConfigValidationResult &Result) const
{
    for (const auto &Field : ConfigJson->Values)
    {
        const FString &Key = Field.Key;
        const TSharedPtr<FJsonValue> &Value = Field.Value;

        if (Key.StartsWith(TEXT("meta.")))
        {
            continue;
        }

        if (!Schema.ExpectedTypes.Contains(Key))
        {
            Result.AddIssue(FConfigValidationIssue(
                EConfigValidationSeverity::Warning,
                Key,
                TEXT("Unknown configuration key (will be ignored)"),
                false));
            continue;
        }

        const FString &ExpectedType = Schema.ExpectedTypes[Key];

        FString ValueStr = GetJsonValueAsString(Value);
        if (!ValidateValueType(ValueStr, ExpectedType))
        {
            FConfigValidationIssue Issue(
                EConfigValidationSeverity::Error,
                Key,
                FString::Printf(TEXT("Invalid type. Expected: %s, Got: %s"), *ExpectedType, *GetJsonValueType(Value)),
                true);
            Issue.ExpectedValue = ExpectedType;
            Issue.ActualValue = ValueStr;
            Result.AddIssue(Issue);
            continue;
        }

        if (Schema.Constraints.Contains(Key))
        {
            const FString &Constraint = Schema.Constraints[Key];
            if (!ValidateConstraint(ValueStr, Constraint))
            {
                FConfigValidationIssue Issue(
                    EConfigValidationSeverity::Error,
                    Key,
                    FString::Printf(TEXT("Value violates constraint: %s"), *Constraint),
                    true);
                Issue.ActualValue = ValueStr;
                Issue.SuggestedFix = FString::Printf(TEXT("Use value within constraint: %s"), *Constraint);
                Result.AddIssue(Issue);
            }
        }
    }
}

void FConfigurationValidator::ValidateNoUnknownKeys(const TSharedPtr<FJsonObject> &ConfigJson, FConfigValidationResult &Result) const
{
}

bool FConfigurationValidator::CanAutoFixResult(const FConfigValidationResult &Result) const
{
    for (const FConfigValidationIssue &Issue : Result.Issues)
    {
        if (Issue.IsError() && !Issue.bCanAutoFix)
        {
            return false;
        }
    }
    return true;
}

bool FConfigurationValidator::TryFixIssue(TSharedPtr<FJsonObject> &ConfigJson, const FConfigValidationIssue &Issue) const
{
    if (!Issue.bCanAutoFix)
    {
        return false;
    }

    if (Issue.Message.Contains(TEXT("missing")))
    {
        FString DefaultValue = GetDefaultValue(Issue.Key);
        if (!DefaultValue.IsEmpty())
        {
            SetJsonValue(ConfigJson, Issue.Key, DefaultValue);
            return true;
        }
    }

    if (Issue.Message.Contains(TEXT("Invalid type")) || Issue.Message.Contains(TEXT("violates constraint")))
    {
        FString DefaultValue = GetDefaultValue(Issue.Key);
        if (!DefaultValue.IsEmpty())
        {
            SetJsonValue(ConfigJson, Issue.Key, DefaultValue);
            return true;
        }
    }

    if (Issue.Key == Keys::META_CONFIG_VERSION)
    {
        ConfigJson->SetNumberField(Issue.Key, Schema.Version);
        return true;
    }

    return false;
}

bool FConfigurationValidator::ValidateValueType(const FString &Value, const FString &ExpectedType) const
{
    if (ExpectedType == Types::INT)
    {
        return Value.IsNumeric() && !Value.Contains(TEXT("."));
    }
    else if (ExpectedType == Types::FLOAT)
    {
        return Value.IsNumeric();
    }
    else if (ExpectedType == Types::BOOL)
    {
        return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
               Value.Equals(TEXT("false"), ESearchCase::IgnoreCase);
    }
    else if (ExpectedType == Types::STRING)
    {
        return true; // All values can be strings
    }

    return false;
}

bool FConfigurationValidator::ValidateConstraint(const FString &Value, const FString &Constraint) const
{
    if (Constraint.StartsWith(TEXT("range(")))
    {
        FString RangeStr = Constraint.Mid(6, Constraint.Len() - 7);
        FString MinStr, MaxStr;
        if (RangeStr.Split(TEXT(","), &MinStr, &MaxStr))
        {
            if (Value.IsNumeric())
            {
                double NumValue = FCString::Atod(*Value);
                double MinValue = FCString::Atod(*MinStr);
                double MaxValue = FCString::Atod(*MaxStr);
                return NumValue >= MinValue && NumValue <= MaxValue;
            }
        }
        return false;
    }
    else if (Constraint.StartsWith(TEXT("enum(")))
    {
        FString EnumStr = Constraint.Mid(5, Constraint.Len() - 6);
        TArray<FString> ValidValues;
        EnumStr.ParseIntoArray(ValidValues, TEXT(","));

        return ValidValues.Contains(Value);
    }

    return true;
}

FString FConfigurationValidator::GetJsonValueAsString(const TSharedPtr<FJsonValue> &Value) const
{
    if (!Value.IsValid())
    {
        return FString();
    }

    switch (Value->Type)
    {
    case EJson::Number:
    {
        double NumValue = Value->AsNumber();
        if (FMath::IsNearlyEqual(NumValue, FMath::RoundToDouble(NumValue)))
        {
            return FString::FromInt(static_cast<int32>(NumValue));
        }
        return FString::SanitizeFloat(NumValue);
    }
    case EJson::String:
        return Value->AsString();
    case EJson::Boolean:
        return Value->AsBool() ? TEXT("true") : TEXT("false");
    default:
        return FString();
    }
}

FString FConfigurationValidator::GetJsonValueType(const TSharedPtr<FJsonValue> &Value) const
{
    if (!Value.IsValid())
    {
        return TEXT("null");
    }

    switch (Value->Type)
    {
    case EJson::Number:
    {
        double NumValue = Value->AsNumber();
        if (FMath::IsNearlyEqual(NumValue, FMath::RoundToDouble(NumValue)))
        {
            return Types::INT;
        }
        return Types::FLOAT;
    }
    case EJson::String:
        return Types::STRING;
    case EJson::Boolean:
        return Types::BOOL;
    case EJson::Array:
        return TEXT("array");
    case EJson::Object:
        return TEXT("object");
    default:
        return TEXT("unknown");
    }
}

void FConfigurationValidator::SetJsonValue(TSharedPtr<FJsonObject> &ConfigJson, const FString &Key, const FString &Value) const
{
    FString ExpectedType = GetExpectedType(Key);

    if (ExpectedType == Types::INT)
    {
        ConfigJson->SetNumberField(Key, FCString::Atoi(*Value));
    }
    else if (ExpectedType == Types::FLOAT)
    {
        ConfigJson->SetNumberField(Key, FCString::Atod(*Value));
    }
    else if (ExpectedType == Types::BOOL)
    {
        ConfigJson->SetBoolField(Key, Value.Equals(TEXT("true"), ESearchCase::IgnoreCase));
    }
    else
    {
        ConfigJson->SetStringField(Key, Value);
    }
}
