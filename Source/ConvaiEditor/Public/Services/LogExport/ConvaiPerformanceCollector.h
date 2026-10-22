/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiPerformanceCollector.h
 *
 * Collects performance metrics.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/LogExport/IConvaiInfoCollector.h"

/**
 * Collects FPS, memory, and rendering statistics.
 */
class FConvaiPerformanceCollector : public IConvaiInfoCollector
{
public:
    FConvaiPerformanceCollector() = default;
    virtual ~FConvaiPerformanceCollector() = default;

    // IConvaiInfoCollector interface
    virtual TSharedPtr<FJsonObject> CollectInfo() const override;
    virtual FString GetCollectorName() const override { return TEXT("PerformanceMetrics"); }
    virtual bool IsAvailable() const override { return true; }

private:
    /** Collect FPS statistics */
    TSharedPtr<FJsonObject> CollectFPSStats() const;

    /** Collect memory usage */
    TSharedPtr<FJsonObject> CollectMemoryStats() const;

    /** Collect rendering stats */
    TSharedPtr<FJsonObject> CollectRenderingStats() const;

    /** Get uptime information */
    TSharedPtr<FJsonObject> GetUptimeInfo() const;
};
