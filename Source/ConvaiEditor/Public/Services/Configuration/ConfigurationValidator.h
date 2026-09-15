/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConfigurationValidator.h
 *
 * Concrete implementation of configuration validator
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/Configuration/IConfigurationValidator.h"

/**
 * Configuration validator implementation.
 */
class CONVAIEDITOR_API FConfigurationValidator : public IConfigurationValidator
{
public:
    FConfigurationValidator();
    virtual ~FConfigurationValidator() = default;

    virtual void Startup() override;
    virtual void Shutdown() override;
    static FName StaticType() { return TEXT("IConfigurationValidator"); }

    virtual FConfigValidationResult Validate(const TSharedPtr<FJsonObject> &ConfigJson) const override;
    virtual FConfigValidationResult ValidateFile(const FString &ConfigFilePath) const override;
    virtual bool AutoFix(TSharedPtr<FJsonObject> &ConfigJson, const FConfigValidationResult &ValidationResult) const override;
    virtual const FConfigurationSchema &GetSchema() const override;
    virtual int32 GetCurrentSchemaVersion() const override;
    virtual bool NeedsMigration(int32 ConfigVersion) const override;
    virtual TOptional<FConfigValidationIssue> ValidateKeyValue(const FString &Key, const FString &Value) const override;
    virtual FString GetDefaultValue(const FString &Key) const override;
    virtual bool IsRequiredKey(const FString &Key) const override;
    virtual FString GetExpectedType(const FString &Key) const override;

private:
    FConfigValidationResult ValidateIniFile(const FString &ConfigFilePath) const;
    int32 ValidateVersion(const TSharedPtr<FJsonObject> &ConfigJson, FConfigValidationResult &Result) const;
    void ValidateRequiredKeys(const TSharedPtr<FJsonObject> &ConfigJson, FConfigValidationResult &Result) const;
    void ValidateAllKeys(const TSharedPtr<FJsonObject> &ConfigJson, FConfigValidationResult &Result) const;
    void ValidateNoUnknownKeys(const TSharedPtr<FJsonObject> &ConfigJson, FConfigValidationResult &Result) const;
    bool CanAutoFixResult(const FConfigValidationResult &Result) const;
    bool TryFixIssue(TSharedPtr<FJsonObject> &ConfigJson, const FConfigValidationIssue &Issue) const;
    bool ValidateValueType(const FString &Value, const FString &ExpectedType) const;
    bool ValidateConstraint(const FString &Value, const FString &Constraint) const;
    FString GetJsonValueAsString(const TSharedPtr<FJsonValue> &Value) const;
    FString GetJsonValueType(const TSharedPtr<FJsonValue> &Value) const;
    void SetJsonValue(TSharedPtr<FJsonObject> &ConfigJson, const FString &Key, const FString &Value) const;

    FConfigurationSchema Schema;
};
