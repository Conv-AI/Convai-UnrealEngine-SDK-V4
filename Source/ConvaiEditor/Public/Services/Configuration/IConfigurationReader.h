/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IConfigurationReader.h
 *
 * Read-only interface for configuration access.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/ConvaiDIContainer.h"

/**
 * Read-only interface for configuration values.
 */
class CONVAIEDITOR_API IConfigurationReader : public IConvaiService
{
public:
    virtual ~IConfigurationReader() = default;

    virtual FString GetString(const FString &Key, const FString &Default = FString()) const = 0;
    virtual int32 GetInt(const FString &Key, int32 Default = 0) const = 0;
    virtual float GetFloat(const FString &Key, float Default = 0.0f) const = 0;
    virtual bool GetBool(const FString &Key, bool Default = false) const = 0;

    virtual int32 GetWindowWidth() const = 0;
    virtual int32 GetWindowHeight() const = 0;
    virtual float GetMinWindowWidth() const = 0;
    virtual float GetMinWindowHeight() const = 0;

    static FName StaticType() { return TEXT("IConfigurationReader"); }
};
