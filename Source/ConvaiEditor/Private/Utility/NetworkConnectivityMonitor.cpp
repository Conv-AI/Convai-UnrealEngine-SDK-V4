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
          bWasConnected(true), bIsMonitoring(false), bCheckInProgress(false), CurrentProbeIndex(0), LastSuccessfulCheckTime(0.0)
    {
        if (Config.bAutoStart)
        {
            Start();
        }
    }

    FNetworkConnectivityMonitor::~FNetworkConnectivityMonitor()
    {
        Stop();
    }

    void FNetworkConnectivityMonitor::Start()
    {
        if (bIsMonitoring)
        {
            return;
        }

        bIsMonitoring = true;

        TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateRaw(this, &FNetworkConnectivityMonitor::TickConnectivityCheck),
            Config.CheckIntervalSeconds);

        CheckNow();
    }

    void FNetworkConnectivityMonitor::Stop()
    {
        if (!bIsMonitoring)
        {
            return;
        }

        bIsMonitoring = false;

        if (TickerHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
            TickerHandle.Reset();
        }
    }

    void FNetworkConnectivityMonitor::CheckNow()
    {
        if (bCheckInProgress)
        {
            return;
        }

        PerformConnectivityCheck();
    }

    bool FNetworkConnectivityMonitor::TickConnectivityCheck(float DeltaTime)
    {
        if (!bCheckInProgress)
        {
            PerformConnectivityCheck();
        }

        return true;
    }

    void FNetworkConnectivityMonitor::PerformConnectivityCheck()
    {
        if (Config.ProbeUrls.Num() == 0)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("No probe URLs configured for network monitoring"));
            return;
        }

        bCheckInProgress = true;
        CurrentProbeIndex = 0;

        const FString &ProbeUrl = Config.ProbeUrls[CurrentProbeIndex];

        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
        Request->SetURL(ProbeUrl);
        Request->SetVerb(TEXT("HEAD"));
        Request->SetTimeout(Config.ProbeTimeoutSeconds);

        Request->OnProcessRequestComplete().BindLambda(
            [this, ProbeUrl](FHttpRequestPtr Req, FHttpResponsePtr Response, bool bWasSuccessful)
            {
                if (!Req.IsValid())
                {
                    HandleProbeResponse(false);
                    return;
                }

                bool bProbeSuccess = bWasSuccessful && Response.IsValid() &&
                                     (Response->GetResponseCode() >= 200 && Response->GetResponseCode() < 400);

                if (bProbeSuccess)
                {
                    HandleProbeResponse(true);
                }
                else
                {
                    CurrentProbeIndex++;

                    if (CurrentProbeIndex < Config.ProbeUrls.Num())
                    {
                        const FString &NextUrl = Config.ProbeUrls[CurrentProbeIndex];

                        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> NextRequest = FHttpModule::Get().CreateRequest();
                        NextRequest->SetURL(NextUrl);
                        NextRequest->SetVerb(TEXT("HEAD"));
                        NextRequest->SetTimeout(Config.ProbeTimeoutSeconds);

                        NextRequest->OnProcessRequestComplete().BindLambda(
                            [this](FHttpRequestPtr Req2, FHttpResponsePtr Response2, bool bSuccess2)
                            {
                                if (!Req2.IsValid())
                                {
                                    HandleProbeResponse(false);
                                    return;
                                }

                                bool bProbeSuccess2 = bSuccess2 && Response2.IsValid() &&
                                                      (Response2->GetResponseCode() >= 200 && Response2->GetResponseCode() < 400);
                                HandleProbeResponse(bProbeSuccess2);
                            });

                        NextRequest->ProcessRequest();
                    }
                    else
                    {
                        HandleProbeResponse(false);
                    }
                }
            });

        Request->ProcessRequest();
    }

    void FNetworkConnectivityMonitor::HandleProbeResponse(bool bSuccess)
    {
        bCheckInProgress = false;

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
