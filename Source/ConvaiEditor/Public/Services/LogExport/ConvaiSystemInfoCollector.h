/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiSystemInfoCollector.h
 *
 * Collects system and hardware information.
 */

#pragma once

#include "CoreMinimal.h"
#include "IConvaiInfoCollector.h"

/**
 * Collects OS, hardware, and locale information.
 */
class FConvaiSystemInfoCollector : public IConvaiInfoCollector
{
public:
    FConvaiSystemInfoCollector() = default;
    virtual ~FConvaiSystemInfoCollector() = default;

    // IConvaiInfoCollector interface
    virtual TSharedPtr<FJsonObject> CollectInfo() const override;
    virtual FString GetCollectorName() const override { return TEXT("SystemInfo"); }
    virtual bool IsAvailable() const override { return true; }

private:
    /** Collect Operating System information */
    TSharedPtr<FJsonObject> CollectOSInfo() const;

    /** Collect Hardware information (CPU, GPU, RAM) */
    TSharedPtr<FJsonObject> CollectHardwareInfo() const;

    /** Collect Locale and Region information */
    TSharedPtr<FJsonObject> CollectLocaleInfo() const;

    /** Get CPU brand string */
    FString GetCPUBrand() const;

    /** Get GPU adapter information */
    TArray<TSharedPtr<FJsonValue>> GetGPUInfo() const;

    /** Get total physical RAM in GB */
    float GetTotalPhysicalRAM() const;
};
