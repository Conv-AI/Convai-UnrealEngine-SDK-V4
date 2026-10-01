/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * NetworkConnectivityMonitor.cpp
 *
 * Implementation of network connectivity monitoring.
 */

#include "Utility/NetworkConnectivityMonitor.h"
#include "ConvaiEditor.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

namespace ConvaiEditor
{
    FNetworkConnectivityMonitor::FNetworkConnectivityMonitor(const FConfig &InConfig)
        : Config(InConfig), bIsConnected(true),
          bWasConnected(true), CurrentProbeIndex(0), LastSuccessfulCheckTime(0.0)
    {
        bIsMonitoring.store(false, std::memory_order_relaxed);
        SharedState = MakeShared<FNetworkMonitorSharedState>();
        
        if (Config.bAutoStart)
        {
            Start();
        }
    }

    FNetworkConnectivityMonitor::~FNetworkConnectivityMonitor()
    {
        if (SharedState.IsValid())
        {
            SharedState->bIsActive.store(false, std::memory_order_seq_cst);
        }
        Stop();
    }

    void FNetworkConnectivityMonitor::Start()
    {
        if (bIsMonitoring.load(std::memory_order_acquire))
        {
            return;
        }

        bIsMonitoring.store(true, std::memory_order_release);

        // CRITICAL: Use SharedState to safely access monitor after potential destruction
        // Capture SharedState by value to extend lifetime, and 'this' for method access
        // Check SharedState->bIsActive BEFORE using 'this' to prevent dangling pointer
        TSharedPtr<FNetworkMonitorSharedState> CapturedState = SharedState;
        FNetworkConnectivityMonitor* MonitorPtr = this;
        
        TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([CapturedState, MonitorPtr](float DeltaTime) -> bool
            {
                // SAFETY: Check shared state first - if false, object is being destroyed
                if (!CapturedState.IsValid() || !CapturedState->bIsActive.load(std::memory_order_acquire))
                {
                    return false; // Stop ticker - do NOT touch 'this'
                }
                
                // SharedState indicates object is alive - safe to use MonitorPtr
                if (!CapturedState->bCheckInProgress.load(std::memory_order_acquire))
                {
                    MonitorPtr->PerformConnectivityCheck();
                }
                
                return true;
            }),
            Config.CheckIntervalSeconds);

        CheckNow();
    }

    void FNetworkConnectivityMonitor::Stop()
    {
        if (!bIsMonitoring.load(std::memory_order_acquire))
        {
            return;
        }

        bIsMonitoring.store(false, std::memory_order_release);

        if (TickerHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
            TickerHandle.Reset();
        }
    }

    void FNetworkConnectivityMonitor::CheckNow()
    {
        if (!SharedState.IsValid() || SharedState->bCheckInProgress.load(std::memory_order_acquire))
        {
            return;
        }

        PerformConnectivityCheck();
    }

    void FNetworkConnectivityMonitor::PerformConnectivityCheck()
    {
        if (!SharedState.IsValid() || !SharedState->bIsActive.load(std::memory_order_acquire))
        {
            return;
        }
        
        if (Config.ProbeUrls.Num() == 0)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("No probe URLs configured for network monitoring"));
            return;
        }

        SharedState->bCheckInProgress.store(true, std::memory_order_release);
        CurrentProbeIndex = 0;

        const FString &ProbeUrl = Config.ProbeUrls[CurrentProbeIndex];

        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
        Request->SetURL(ProbeUrl);
        Request->SetVerb(TEXT("HEAD"));
        Request->SetTimeout(Config.ProbeTimeoutSeconds);

        // CRITICAL: Capture SharedState to safely check if object is alive
        // Do NOT touch MonitorPtr if SharedState->bIsActive is false
        TSharedPtr<FNetworkMonitorSharedState> CapturedState = SharedState;
        FNetworkConnectivityMonitor* MonitorPtr = this;
        TSharedPtr<FConfig> ConfigPtr = MakeShared<FConfig>(Config);
        
        Request->OnProcessRequestComplete().BindLambda(
            [CapturedState, MonitorPtr, ConfigPtr, ProbeUrl](FHttpRequestPtr Req, FHttpResponsePtr Response, bool bWasSuccessful)
            {
                // SAFETY: Check if monitor is still alive before touching 'this'
                if (!CapturedState.IsValid() || !CapturedState->bIsActive.load(std::memory_order_acquire))
                {
                    return; // Monitor destroyed - do NOT touch MonitorPtr
                }
                
                if (!Req.IsValid())
                {
                    MonitorPtr->HandleProbeResponse(false);
                    return;
                }

                bool bProbeSuccess = bWasSuccessful && Response.IsValid() &&
                                     (Response->GetResponseCode() >= 200 && Response->GetResponseCode() < 400);

                if (bProbeSuccess)
                {
                    MonitorPtr->HandleProbeResponse(true);
                }
                else
                {
                    MonitorPtr->CurrentProbeIndex++;

                    if (MonitorPtr->CurrentProbeIndex < ConfigPtr->ProbeUrls.Num())
                    {
                        const FString &NextUrl = ConfigPtr->ProbeUrls[MonitorPtr->CurrentProbeIndex];

                        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> NextRequest = FHttpModule::Get().CreateRequest();
                        NextRequest->SetURL(NextUrl);
                        NextRequest->SetVerb(TEXT("HEAD"));
                        NextRequest->SetTimeout(ConfigPtr->ProbeTimeoutSeconds);

                        NextRequest->OnProcessRequestComplete().BindLambda(
                            [CapturedState, MonitorPtr](FHttpRequestPtr Req2, FHttpResponsePtr Response2, bool bSuccess2)
                            {
                                // SAFETY: Check again for nested callback
                                if (!CapturedState.IsValid() || !CapturedState->bIsActive.load(std::memory_order_acquire))
                                {
                                    return; // Monitor destroyed
                                }
                                
                                if (!Req2.IsValid())
                                {
                                    MonitorPtr->HandleProbeResponse(false);
                                    return;
                                }

                                bool bProbeSuccess2 = bSuccess2 && Response2.IsValid() &&
                                                      (Response2->GetResponseCode() >= 200 && Response2->GetResponseCode() < 400);
                                MonitorPtr->HandleProbeResponse(bProbeSuccess2);
                            });

                        NextRequest->ProcessRequest();
                    }
                    else
                    {
                        MonitorPtr->HandleProbeResponse(false);
                    }
                }
            });

        Request->ProcessRequest();
    }

    void FNetworkConnectivityMonitor::HandleProbeResponse(bool bSuccess)
    {
        if (!SharedState.IsValid() || !SharedState->bIsActive.load(std::memory_order_acquire))
        {
            return;
        }
        
        SharedState->bCheckInProgress.store(false, std::memory_order_release);

        bWasConnected = bIsConnected;
        bIsConnected = bSuccess;

        if (bSuccess)
        {
            LastSuccessfulCheckTime = FPlatformTime::Seconds();
        }

        if (bWasConnected != bIsConnected)
        {
            if (Config.bEnableLogging)
            {
                if (bIsConnected)
                {
                    UE_LOG(LogConvaiEditor, Log, TEXT("Network connectivity restored"));
                }
                else
                {
                    UE_LOG(LogConvaiEditor, Warning, TEXT("Network connectivity lost"));
                }
            }

            ConnectivityChangedDelegate.Broadcast(bIsConnected);
        }
    }

} // namespace ConvaiEditor
