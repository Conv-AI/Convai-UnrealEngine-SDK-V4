/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiNetworkDiagnostics.cpp
 *
 * Implementation of network diagnostics collector.
 */

#include "Services/LogExport/ConvaiNetworkDiagnostics.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

TSharedPtr<FJsonObject> FConvaiNetworkDiagnostics::CollectInfo() const
{
    TSharedPtr<FJsonObject> NetworkInfo = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> ProxyTest = DetectProxyFirewall();
    if (ProxyTest.IsValid())
    {
        NetworkInfo->SetObjectField(TEXT("ProxyFirewall"), ProxyTest);
    }

    TArray<TSharedPtr<FJsonValue>> Adapters = GetNetworkAdapters();
    if (Adapters.Num() > 0)
    {
        NetworkInfo->SetArrayField(TEXT("NetworkAdapters"), Adapters);
    }

    NetworkInfo->SetStringField(TEXT("TestTimestamp"), FDateTime::UtcNow().ToIso8601());

    return NetworkInfo;
}

TSharedPtr<FJsonObject> FConvaiNetworkDiagnostics::TestConvaiAPI() const
{
    TSharedPtr<FJsonObject> APIInfo = MakeShared<FJsonObject>();

    const FString ConvaiAPIURL = TEXT("https://api.convai.com/health");

    int32 StatusCode = 0;
    FString Response;
    double Latency = 0.0;

    bool bSuccess = PerformHTTPRequest(ConvaiAPIURL, StatusCode, Response, Latency);

    APIInfo->SetBoolField(TEXT("Reachable"), bSuccess && StatusCode == 200);
    APIInfo->SetNumberField(TEXT("StatusCode"), (double)StatusCode);
    APIInfo->SetNumberField(TEXT("LatencyMs"), Latency);
    APIInfo->SetStringField(TEXT("URL"), ConvaiAPIURL);

    if (!bSuccess)
    {
        APIInfo->SetStringField(TEXT("Error"), TEXT("Failed to connect"));
    }

    return APIInfo;
}

TSharedPtr<FJsonObject> FConvaiNetworkDiagnostics::TestWebSocketConnection() const
{
    TSharedPtr<FJsonObject> WSInfo = MakeShared<FJsonObject>();

    WSInfo->SetBoolField(TEXT("SubsystemAvailable"), true);
    WSInfo->SetStringField(TEXT("Note"), TEXT("Full WebSocket test requires active connection"));

    return WSInfo;
}

TSharedPtr<FJsonObject> FConvaiNetworkDiagnostics::TestDNSResolution() const
{
    TSharedPtr<FJsonObject> DNSInfo = MakeShared<FJsonObject>();

    TArray<FString> DomainsToTest = {
        TEXT("api.convai.com"),
        TEXT("convai.com")};

    TArray<TSharedPtr<FJsonValue>> ResolutionResults;

    for (const FString &Domain : DomainsToTest)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("Domain"), Domain);

        TArray<FString> IPs;
        bool bResolved = ResolveDomain(Domain, IPs);

        Result->SetBoolField(TEXT("Resolved"), bResolved);

        if (bResolved && IPs.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> IPArray;
            for (const FString &IP : IPs)
            {
                IPArray.Add(MakeShared<FJsonValueString>(IP));
            }
            Result->SetArrayField(TEXT("IPs"), IPArray);
        }
        else
        {
            Result->SetStringField(TEXT("Error"), TEXT("Failed to resolve"));
        }

        ResolutionResults.Add(MakeShared<FJsonValueObject>(Result));
    }

    DNSInfo->SetArrayField(TEXT("Results"), ResolutionResults);

    return DNSInfo;
}

TSharedPtr<FJsonObject> FConvaiNetworkDiagnostics::MeasureLatency() const
{
    TSharedPtr<FJsonObject> LatencyInfo = MakeShared<FJsonObject>();

    const int32 NumPings = 3;
    TArray<double> Latencies;

    for (int32 i = 0; i < NumPings; ++i)
    {
        int32 StatusCode;
        FString Response;
        double Latency;

        if (PerformHTTPRequest(TEXT("https://api.convai.com/health"), StatusCode, Response, Latency))
        {
            Latencies.Add(Latency);
        }
    }

    if (Latencies.Num() > 0)
    {
        double Sum = 0.0;
        double Min = Latencies[0];
        double Max = Latencies[0];

        for (double Lat : Latencies)
        {
            Sum += Lat;
            if (Lat < Min)
                Min = Lat;
            if (Lat > Max)
                Max = Lat;
        }

        double Average = Sum / Latencies.Num();

        LatencyInfo->SetNumberField(TEXT("AverageMs"), Average);
        LatencyInfo->SetNumberField(TEXT("MinMs"), Min);
        LatencyInfo->SetNumberField(TEXT("MaxMs"), Max);
        LatencyInfo->SetNumberField(TEXT("SampleCount"), (double)Latencies.Num());
    }
    else
    {
        LatencyInfo->SetStringField(TEXT("Error"), TEXT("Failed to measure latency"));
    }

    return LatencyInfo;
}

TSharedPtr<FJsonObject> FConvaiNetworkDiagnostics::DetectProxyFirewall() const
{
    TSharedPtr<FJsonObject> ProxyInfo = MakeShared<FJsonObject>();

    FString HTTPProxy = FPlatformMisc::GetEnvironmentVariable(TEXT("HTTP_PROXY"));
    FString HTTPSProxy = FPlatformMisc::GetEnvironmentVariable(TEXT("HTTPS_PROXY"));
    FString NoProxy = FPlatformMisc::GetEnvironmentVariable(TEXT("NO_PROXY"));

    ProxyInfo->SetBoolField(TEXT("HTTPProxyDetected"), !HTTPProxy.IsEmpty());
    ProxyInfo->SetBoolField(TEXT("HTTPSProxyDetected"), !HTTPSProxy.IsEmpty());

    if (!HTTPProxy.IsEmpty())
    {
        ProxyInfo->SetStringField(TEXT("HTTPProxy"), HTTPProxy);
    }

    if (!HTTPSProxy.IsEmpty())
    {
        ProxyInfo->SetStringField(TEXT("HTTPSProxy"), HTTPSProxy);
    }

    if (!NoProxy.IsEmpty())
    {
        ProxyInfo->SetStringField(TEXT("NoProxy"), NoProxy);
    }

    return ProxyInfo;
}

TArray<TSharedPtr<FJsonValue>> FConvaiNetworkDiagnostics::GetNetworkAdapters() const
{
    TArray<TSharedPtr<FJsonValue>> Adapters;

    ISocketSubsystem *SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (SocketSubsystem)
    {
        TArray<TSharedPtr<FInternetAddr>> Addresses;
        if (SocketSubsystem->GetLocalAdapterAddresses(Addresses))
        {
            for (const TSharedPtr<FInternetAddr> &Address : Addresses)
            {
                if (Address.IsValid())
                {
                    TSharedPtr<FJsonObject> AdapterObj = MakeShared<FJsonObject>();
                    AdapterObj->SetStringField(TEXT("Address"), Address->ToString(false));
                    Adapters.Add(MakeShared<FJsonValueObject>(AdapterObj));
                }
            }
        }
    }

    return Adapters;
}

bool FConvaiNetworkDiagnostics::PerformHTTPRequest(const FString &URL, int32 &OutStatusCode, FString &OutResponse, double &OutLatency) const
{
    if (!FHttpModule::Get().IsHttpEnabled())
    {
        return false;
    }

    TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(URL);
    Request->SetVerb(TEXT("GET"));
    Request->SetTimeout(5.0f);

    double StartTime = FPlatformTime::Seconds();

    Request->ProcessRequest();

    double WaitStartTime = FPlatformTime::Seconds();
    while (Request->GetStatus() == EHttpRequestStatus::Processing)
    {
        FPlatformProcess::Sleep(0.01f);

        if (FPlatformTime::Seconds() - WaitStartTime > 10.0)
        {
            return false;
        }
    }

    OutLatency = (FPlatformTime::Seconds() - StartTime) * 1000.0;

    if (Request->GetStatus() == EHttpRequestStatus::Succeeded)
    {
        FHttpResponsePtr Response = Request->GetResponse();
        if (Response.IsValid())
        {
            OutStatusCode = Response->GetResponseCode();
            OutResponse = Response->GetContentAsString();
            return true;
        }
    }

    return false;
}

bool FConvaiNetworkDiagnostics::ResolveDomain(const FString &Domain, TArray<FString> &OutIPs) const
{
    ISocketSubsystem *SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        return false;
    }

    TSharedPtr<FInternetAddr> Address = SocketSubsystem->GetAddressFromString(Domain);
    if (!Address.IsValid())
    {
        FAddressInfoResult Result = SocketSubsystem->GetAddressInfo(*Domain, nullptr, EAddressInfoFlags::Default, NAME_None);

        if (Result.ReturnCode == SE_NO_ERROR && Result.Results.Num() > 0)
        {
            for (const auto &AddrResult : Result.Results)
            {
                OutIPs.Add(AddrResult.Address->ToString(false));
            }
            return true;
        }
        return false;
    }

    OutIPs.Add(Address->ToString(false));
    return true;
}
