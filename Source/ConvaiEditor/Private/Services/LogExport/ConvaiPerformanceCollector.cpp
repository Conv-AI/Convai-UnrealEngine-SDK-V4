/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiPerformanceCollector.cpp
 *
 * Implementation of performance metrics collector.
 */

#include "Services/LogExport/ConvaiPerformanceCollector.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"

TSharedPtr<FJsonObject> FConvaiPerformanceCollector::CollectInfo() const
{
    TSharedPtr<FJsonObject> PerfInfo = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> FPSStats = CollectFPSStats();
    if (FPSStats.IsValid())
    {
        PerfInfo->SetObjectField(TEXT("FPS"), FPSStats);
    }

    TSharedPtr<FJsonObject> MemoryStats = CollectMemoryStats();
    if (MemoryStats.IsValid())
    {
        PerfInfo->SetObjectField(TEXT("Memory"), MemoryStats);
    }

    TSharedPtr<FJsonObject> RenderingStats = CollectRenderingStats();
    if (RenderingStats.IsValid())
    {
        PerfInfo->SetObjectField(TEXT("Rendering"), RenderingStats);
    }

    TSharedPtr<FJsonObject> UptimeInfo = GetUptimeInfo();
    if (UptimeInfo.IsValid())
    {
        PerfInfo->SetObjectField(TEXT("Uptime"), UptimeInfo);
    }

    PerfInfo->SetStringField(TEXT("CollectionTimestamp"), FDateTime::UtcNow().ToIso8601());

    return PerfInfo;
}

TSharedPtr<FJsonObject> FConvaiPerformanceCollector::CollectFPSStats() const
{
    TSharedPtr<FJsonObject> FPSInfo = MakeShared<FJsonObject>();

    float CurrentFPS = 1.0f / FApp::GetDeltaTime();

    FPSInfo->SetNumberField(TEXT("CurrentFPS"), (double)CurrentFPS);
    FPSInfo->SetNumberField(TEXT("DeltaTimeMs"), (double)(FApp::GetDeltaTime() * 1000.0f));

    float TargetFrameRate = GEngine ? GEngine->GetMaxFPS() : 60.0f;
    FPSInfo->SetNumberField(TEXT("TargetFPS"), (double)TargetFrameRate);

    return FPSInfo;
}

TSharedPtr<FJsonObject> FConvaiPerformanceCollector::CollectMemoryStats() const
{
    TSharedPtr<FJsonObject> MemInfo = MakeShared<FJsonObject>();

    FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();

    MemInfo->SetNumberField(TEXT("UsedPhysicalMB"), MemStats.UsedPhysical / (1024.0 * 1024.0));
    MemInfo->SetNumberField(TEXT("AvailablePhysicalMB"), MemStats.AvailablePhysical / (1024.0 * 1024.0));
    MemInfo->SetNumberField(TEXT("UsedVirtualMB"), MemStats.UsedVirtual / (1024.0 * 1024.0));
    MemInfo->SetNumberField(TEXT("PeakUsedPhysicalMB"), MemStats.PeakUsedPhysical / (1024.0 * 1024.0));

    if (MemStats.TotalPhysical > 0)
    {
        double UsagePercent = ((double)MemStats.UsedPhysical / (double)MemStats.TotalPhysical) * 100.0;
        MemInfo->SetNumberField(TEXT("UsagePercent"), UsagePercent);
    }

    return MemInfo;
}

TSharedPtr<FJsonObject> FConvaiPerformanceCollector::CollectRenderingStats() const
{
    TSharedPtr<FJsonObject> RenderInfo = MakeShared<FJsonObject>();

    RenderInfo->SetBoolField(TEXT("IsEditor"), GIsEditor);
    RenderInfo->SetBoolField(TEXT("IsGame"), !GIsEditor);

    if (GEngine && GEngine->GameViewport)
    {
        FVector2D ViewportSize;
        GEngine->GameViewport->GetViewportSize(ViewportSize);

        RenderInfo->SetNumberField(TEXT("ViewportWidth"), (double)ViewportSize.X);
        RenderInfo->SetNumberField(TEXT("ViewportHeight"), (double)ViewportSize.Y);
    }

    return RenderInfo;
}

TSharedPtr<FJsonObject> FConvaiPerformanceCollector::GetUptimeInfo() const
{
    TSharedPtr<FJsonObject> UptimeInfo = MakeShared<FJsonObject>();

    double CurrentTime = FPlatformTime::Seconds();
    UptimeInfo->SetNumberField(TEXT("UptimeSeconds"), CurrentTime);
    UptimeInfo->SetStringField(TEXT("UptimeFormatted"),
                               FString::Printf(TEXT("%dh %dm %ds"),
                                               (int32)(CurrentTime / 3600),
                                               (int32)((int32)CurrentTime % 3600) / 60,
                                               (int32)CurrentTime % 60));

    return UptimeInfo;
}
