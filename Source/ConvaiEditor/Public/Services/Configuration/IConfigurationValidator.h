/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IConfigurationValidator.h
 *
 * Interface for configuration validation.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

/**
 * Validation severity levels.
 */
enum class EConfigValidationSeverity : uint8
{
    Info,
    Warning,
    Error,
    Critical
};

/**
 * Validation issue entry.
 */
struct CONVAIEDITOR_API FConfigValidationIssue
{
    /** Severity level of this issue */
    EConfigValidationSeverity Severity;

    /** Configuration key that has the issue */
    FString Key;

    /** Human-readable description of the issue */
    FString Message;

    /** Expected value or format */
    FString ExpectedValue;

    /** Actual value found */
    FString ActualValue;

    /** Can this issue be auto-fixed? */
    bool bCanAutoFix;

    /** Suggested fix action */
    FString SuggestedFix;

    FConfigValidationIssue()
        : Severity(EConfigValidationSeverity::Info), bCanAutoFix(false)
    {
    }

    FConfigValidationIssue(
        EConfigValidationSeverity InSeverity,
        const FString &InKey,
        const FString &InMessage,
        bool bInCanAutoFix = false)
        : Severity(InSeverity), Key(InKey), Message(InMessage), bCanAutoFix(bInCanAutoFix)
    {
    }

    bool IsError() const
    {
        return Severity == EConfigValidationSeverity::Error ||
               Severity == EConfigValidationSeverity::Critical;
    }

    FString GetSeverityString() const
    {
        switch (Severity)
        {
        case EConfigValidationSeverity::Info:
            return TEXT("Info");
        case EConfigValidationSeverity::Warning:
            return TEXT("Warning");
        case EConfigValidationSeverity::Error:
            return TEXT("Error");
        case EConfigValidationSeverity::Critical:
            return TEXT("Critical");
        default:
            return TEXT("Unknown");
        }
    }

    FString ToString() const
    {
        return FString::Printf(TEXT("[%s] %s: %s"),
                               *GetSeverityString(), *Key, *Message);
    }
};

/**
 * Complete validation result.
 */
struct CONVAIEDITOR_API FConfigValidationResult
{
    /** Is the configuration valid? */
    bool bIsValid;

    /** Configuration format version found */
    int32 ConfigVersion;

    /** All validation issues found */
    TArray<FConfigValidationIssue> Issues;

    /** Can the configuration be auto-fixed? */
    bool bCanAutoFix;

    /** Does migration need to run? */
    bool bNeedsMigration;

    /** Should fallback to defaults? */
    bool bShouldFallback;

    FConfigValidationResult()
        : bIsValid(true), ConfigVersion(1), bCanAutoFix(false), bNeedsMigration(false), bShouldFallback(false)
    {
    }

    void AddIssue(const FConfigValidationIssue &Issue)
    {
        Issues.Add(Issue);

        // Update validation status
        if (Issue.IsError())
        {
            bIsValid = false;
        }

        // Update auto-fix capability
        if (!Issue.bCanAutoFix && Issue.IsError())
        {
            bCanAutoFix = false;
        }

        // Critical issues require fallback
        if (Issue.Severity == EConfigValidationSeverity::Critical)
        {
            bShouldFallback = true;
        }
    }

    int32 GetIssueCount(EConfigValidationSeverity Severity) const
    {
        return Issues.FilterByPredicate([Severity](const FConfigValidationIssue &Issue)
                                        { return Issue.Severity == Severity; })
            .Num();
    }

    TArray<FConfigValidationIssue> GetErrors() const
    {
        return Issues.FilterByPredicate([](const FConfigValidationIssue &Issue)
                                        { return Issue.IsError(); });
    }

    FString GetSummary() const
    {
        return FString::Printf(
            TEXT("Validation Result: %s | Version: %d | Issues: %d (Info: %d, Warnings: %d, Errors: %d, Critical: %d)"),
            bIsValid ? TEXT("VALID") : TEXT("INVALID"),
            ConfigVersion,
            Issues.Num(),
            GetIssueCount(EConfigValidationSeverity::Info),
            GetIssueCount(EConfigValidationSeverity::Warning),
            GetIssueCount(EConfigValidationSeverity::Error),
            GetIssueCount(EConfigValidationSeverity::Critical));
    }
};

/**
 * Configuration schema definition.
 */
struct CONVAIEDITOR_API FConfigurationSchema
{
    /** Schema version */
    int32 Version;

    /** Expected configuration keys with their types */
    TMap<FString, FString> ExpectedTypes;

    /** Required keys (must exist) */
    TSet<FString> RequiredKeys;

    /** Optional keys (can be missing, will use defaults) */
    TSet<FString> OptionalKeys;

    /** Value constraints (min/max for numbers, regex for strings, etc.) */
    TMap<FString, FString> Constraints;

    /** Default values for missing optional keys */
    TMap<FString, FString> Defaults;

    FConfigurationSchema()
        : Version(1)
    {
    }
};

/**
 * Interface for configuration validation.
 */
class CONVAIEDITOR_API IConfigurationValidator : public IConvaiService
{
public:
    virtual ~IConfigurationValidator() = default;

    virtual FConfigValidationResult Validate(const TSharedPtr<FJsonObject> &ConfigJson) const = 0;
    virtual FConfigValidationResult ValidateFile(const FString &ConfigFilePath) const = 0;
    virtual bool AutoFix(TSharedPtr<FJsonObject> &ConfigJson, const FConfigValidationResult &ValidationResult) const = 0;
    virtual const FConfigurationSchema &GetSchema() const = 0;
    virtual int32 GetCurrentSchemaVersion() const = 0;
    virtual bool NeedsMigration(int32 ConfigVersion) const = 0;
    virtual TOptional<FConfigValidationIssue> ValidateKeyValue(const FString &Key, const FString &Value) const = 0;
    virtual FString GetDefaultValue(const FString &Key) const = 0;
    virtual bool IsRequiredKey(const FString &Key) const = 0;
    virtual FString GetExpectedType(const FString &Key) const = 0;

    static FName StaticType() { return TEXT("IConfigurationValidator"); }
};
