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
            float CheckIntervalSeconds;
            float ProbeTimeoutSeconds;
            TArray<FString> ProbeUrls;
            bool bEnableLogging;
            bool bAutoStart;

            // Defaults via ctor body, NOT via NSDMI — clang 20 (UE 5.8) rejects
            // NSDMIs on a nested struct used as a default arg of the enclosing
            // class's constructor (see ContentFeedCacheManager.h for the same
            // pattern and a longer note).
            FConfig()
                : CheckIntervalSeconds(10.0f)
                , ProbeTimeoutSeconds(3.0f)
                , ProbeUrls({ TEXT("https://www.google.com"),
                              TEXT("https://www.cloudflare.com"),
                              TEXT("https://api.convai.com") })
                , bEnableLogging(true)
                , bAutoStart(true)
            {}
        };

        // `= {}` (not `= FConfig()`) so clang 20 in UE 5.8 doesn't try to
        // instantiate FConfig's default member initializers while still parsing
        // the enclosing class.
        explicit FNetworkConnectivityMonitor(const FConfig &InConfig = {});

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
