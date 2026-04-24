/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * NetworkConnectivityMonitor.h
 *
 * Monitors network connectivity and notifies listeners when connection state changes.
 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include <atomic>

namespace ConvaiEditor
{
    /** Shared state that outlives the monitor instance to prevent dangling pointer access in async callbacks */
    struct FNetworkMonitorSharedState
    {
        std::atomic<bool> bIsActive{true};
        std::atomic<bool> bCheckInProgress{false};
        FCriticalSection Mutex;
        
        FNetworkMonitorSharedState() = default;
        ~FNetworkMonitorSharedState() = default;
    };

    /** Monitors network connectivity using lightweight HTTP probes */
    class CONVAIEDITOR_API FNetworkConnectivityMonitor
    {
    public:
        /** Delegate called when connectivity state changes */
        DECLARE_MULTICAST_DELEGATE_OneParam(FOnConnectivityChanged, bool /* bIsConnected */);

        /** Configuration for network connectivity monitoring */
        struct FConfig
        {
            /** Check interval in seconds (default: 10.0) */
            float CheckIntervalSeconds = 10.0f;

            /** Timeout for probe requests in seconds (default: 3.0) */
            float ProbeTimeoutSeconds = 3.0f;

            /** URLs to probe for connectivity (tried in order) */
            TArray<FString> ProbeUrls = {
                TEXT("https://www.google.com"),
                TEXT("https://www.cloudflare.com"),
                TEXT("https://api.convai.com")};

            /** Enable logging */
            bool bEnableLogging = true;

            /** Start monitoring immediately */
            bool bAutoStart = true;
        };

        explicit FNetworkConnectivityMonitor(const FConfig &InConfig = FConfig());

        ~FNetworkConnectivityMonitor();

        /** Start monitoring network connectivity */
        void Start();

        /** Stop monitoring network connectivity */
        void Stop();

        /** Check if currently monitoring */
        bool IsMonitoring() const { return bIsMonitoring.load(std::memory_order_acquire); }

        /** Get current connectivity state */
        bool IsConnected() const { return bIsConnected; }

        /** Manually trigger a connectivity check */
        void CheckNow();

        /** Get delegate for connectivity changes */
        FOnConnectivityChanged &OnConnectivityChanged() { return ConnectivityChangedDelegate; }

    private:
        void PerformConnectivityCheck();
        void HandleProbeResponse(bool bSuccess);

        FConfig Config;
        bool bIsConnected;
        bool bWasConnected;
        std::atomic<bool> bIsMonitoring{false};
        TSharedPtr<FNetworkMonitorSharedState> SharedState;
        int32 CurrentProbeIndex;
        FTSTicker::FDelegateHandle TickerHandle;
        FOnConnectivityChanged ConnectivityChangedDelegate;
        double LastSuccessfulCheckTime;
    };

} // namespace ConvaiEditor
