/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IConfigurationWriter.h
 *
 * Write interface for configuration modification.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/ConvaiDIContainer.h"

/**
 * Write interface for configuration values.
 */
class CONVAIEDITOR_API IConfigurationWriter : public IConvaiService
{
public:
    virtual ~IConfigurationWriter() = default;

    virtual void SetString(const FString &Key, const FString &Value) = 0;
    virtual void SetInt(const FString &Key, int32 Value) = 0;
    virtual void SetFloat(const FString &Key, float Value) = 0;
    virtual void SetBool(const FString &Key, bool Value) = 0;

    virtual void SaveConfig() = 0;
    virtual void ReloadConfig() = 0;

    static FName StaticType() { return TEXT("IConfigurationWriter"); }
};
