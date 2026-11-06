/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiNetworkDiagnostics.h
 *
 * Collects network diagnostic information.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/LogExport/IConvaiInfoCollector.h"

/**
 * Tests network connectivity and diagnostics.
 */
class FConvaiNetworkDiagnostics : public IConvaiInfoCollector
{
public:
    FConvaiNetworkDiagnostics() = default;
    virtual ~FConvaiNetworkDiagnostics() = default;

    // IConvaiInfoCollector interface
    virtual TSharedPtr<FJsonObject> CollectInfo() const override;
    virtual FString GetCollectorName() const override { return TEXT("NetworkDiagnostics"); }
    virtual bool IsAvailable() const override { return true; }

private:
    /** Test Convai API reachability */
    TSharedPtr<FJsonObject> TestConvaiAPI() const;

    /** Test WebSocket connectivity */
    TSharedPtr<FJsonObject> TestWebSocketConnection() const;

    /** Test DNS resolution for Convai domains */
    TSharedPtr<FJsonObject> TestDNSResolution() const;

    /** Measure network latency */
    TSharedPtr<FJsonObject> MeasureLatency() const;

    /** Detect proxy/firewall settings */
    TSharedPtr<FJsonObject> DetectProxyFirewall() const;

    /** Get network adapter information */
    TArray<TSharedPtr<FJsonValue>> GetNetworkAdapters() const;

    /** Helper: Perform HTTP GET request with timeout */
    bool PerformHTTPRequest(const FString &URL, int32 &OutStatusCode, FString &OutResponse, double &OutLatency) const;

    /** Helper: Resolve domain to IP */
    bool ResolveDomain(const FString &Domain, TArray<FString> &OutIPs) const;
};
