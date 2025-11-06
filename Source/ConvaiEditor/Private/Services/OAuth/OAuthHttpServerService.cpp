/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * OAuthHttpServerService.cpp
 *
 * Implementation of OAuth HTTP server service.
 */

#include "Services/OAuth/OAuthHttpServerService.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HttpPath.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeExit.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Base64.h"
#include "Interfaces/IPluginManager.h"
#include "Utility/ConvaiConstants.h"
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 7
#include "HttpRequestHandler.h"
#endif

FOAuthHttpServerService::FOAuthHttpServerService()
    : bIsRunning(false), CurrentPort(-1)
{
}

FOAuthHttpServerService::~FOAuthHttpServerService()
{
    Shutdown();
}

void FOAuthHttpServerService::Startup()
{
}

void FOAuthHttpServerService::Shutdown()
{
    StopServer();
}

bool FOAuthHttpServerService::IsPortFree(int32 Port) const
{
    ISocketSubsystem *SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        return false;
    }

    FSocket *TestSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("PortTest"), false);
    if (!TestSocket)
    {
        return false;
    }

    TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
    Addr->SetAnyAddress();
    Addr->SetPort(Port);

    bool bCanBind = TestSocket->Bind(*Addr);
    TestSocket->Close();
    SocketSubsystem->DestroySocket(TestSocket);
    return bCanBind;
}

bool FOAuthHttpServerService::StartServer(const TArray<int32> &PreferredPorts)
{
    if (bIsRunning)
    {
        return true;
    }

    for (int32 Port : PreferredPorts)
    {
        if (!IsPortFree(Port))
        {
            continue;
        }

        FHttpServerModule &HttpServerModule = FHttpServerModule::Get();

        TSharedPtr<IHttpRouter> Router = HttpServerModule.GetHttpRouter(Port);
        if (!Router.IsValid())
        {
            continue;
        }

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 4)
        ControlRouteHandle = Router->BindRoute(FHttpPath(TEXT("/control")), EHttpServerRequestVerbs::VERB_GET,
                                               [WeakThis = TWeakPtr<FOAuthHttpServerService>(AsShared())](const FHttpServerRequest &Request, const FHttpResultCallback &OnComplete)
                                               {
                                                   if (TSharedPtr<FOAuthHttpServerService> Pinned = WeakThis.Pin())
                                                   {
                                                       return Pinned->HandleControlRequest(Request, OnComplete);
                                                   }
                                                   return false;
                                               });
#else
        ControlRouteHandle = Router->BindRoute(
            FHttpPath(TEXT("/control")),
            EHttpServerRequestVerbs::VERB_GET,
            FHttpRequestHandler::CreateSP(AsShared(), &FOAuthHttpServerService::HandleControlRequest));
#endif

        HttpServerModule.StartAllListeners();

        CurrentPort = Port;
        bIsRunning = true;
        return true;
    }

    UE_LOG(LogConvaiEditor, Error, TEXT("OAuthHttpServerService: failed to start on all preferred ports"));
    return false;
}

void FOAuthHttpServerService::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }

    if (!FModuleManager::Get().IsModuleLoaded("HTTPServer"))
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("OAuthHttpServerService: HTTPServer module already unloaded during shutdown, skipping server cleanup"));
        bIsRunning = false;
        CurrentPort = -1;
        return;
    }

    FHttpServerModule &HttpServerModule = FHttpServerModule::Get();
    HttpServerModule.StopAllListeners();

    if (CurrentPort > 0)
    {
        if (TSharedPtr<IHttpRouter> Router = HttpServerModule.GetHttpRouter(CurrentPort))
        {
            Router->UnbindRoute(ControlRouteHandle);
        }
    }

    bIsRunning = false;
    CurrentPort = -1;
}

FString FOAuthHttpServerService::GenerateSuccessHTML(const FString &LogoBase64)
{
    return FString::Printf(TEXT(R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Authentication Successful - Convai</title>
    <style>
        @import url('https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;500;600&display=swap');
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Space Grotesk', -apple-system, BlinkMacSystemFont, 'Segoe UI', system-ui, sans-serif;
            background: #0a0a0a; display: flex; justify-content: center; align-items: center;
            min-height: 100vh; padding: 20px;
        }
        .container { text-align: center; max-width: 480px; width: 100%%; }
        .logo-container { margin-bottom: 48px; animation: fadeIn 0.6s ease-out; }
        @keyframes fadeIn { from { opacity: 0; transform: translateY(-10px); } to { opacity: 1; transform: translateY(0); } }
        .logo { display: flex; align-items: center; justify-content: center; margin-bottom: 16px; }
        .logo img { height: 48px; width: auto; }
        h1 { font-size: 24px; color: #ffffff; margin-bottom: 12px; font-weight: 500; animation: fadeIn 0.6s ease-out 0.4s both; }
        p { font-size: 15px; color: #888888; line-height: 1.6; margin-bottom: 8px; animation: fadeIn 0.6s ease-out 0.5s both; }
        .message { font-size: 14px; color: #00d9a3; margin-top: 32px; animation: fadeIn 0.6s ease-out 0.6s both; }
    </style>
</head>
<body>
    <div class="container">
        <div class="logo-container"><div class="logo"><img src="data:image/png;base64,%s" alt="Convai Logo" /></div></div>
        <h1>Authentication Successful!</h1>
        <p>You can now return to Unreal Engine.</p>
        <div class="message">Please close this window.</div>
    </div>
    <script>
        if (window.history && window.history.replaceState) {
            const cleanUrl = window.location.protocol + '//' + window.location.host + '/auth/success';
            window.history.replaceState({}, document.title, cleanUrl);
        }
    </script>
</body>
</html>
    )"),
                           *LogoBase64);
}

FString FOAuthHttpServerService::GenerateErrorHTML(const FString &LogoBase64)
{
    return FString::Printf(TEXT(R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Authentication Error - Convai</title>
    <style>
        @import url('https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;500;600&display=swap');
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Space Grotesk', -apple-system, BlinkMacSystemFont, 'Segoe UI', system-ui, sans-serif;
            background: #0a0a0a; display: flex; justify-content: center; align-items: center;
            min-height: 100vh; padding: 20px;
        }
        .container { text-align: center; max-width: 480px; width: 100%%; }
        .logo-container { margin-bottom: 48px; }
        .logo { display: flex; align-items: center; justify-content: center; margin-bottom: 16px; }
        .logo img { height: 48px; width: auto; }
        h1 { font-size: 24px; color: #ffffff; margin-bottom: 12px; font-weight: 500; }
        p { font-size: 15px; color: #888888; line-height: 1.6; margin-bottom: 8px; }
    </style>
</head>
<body>
    <div class="container">
        <div class="logo-container"><div class="logo"><img src="data:image/png;base64,%s" alt="Convai Logo" /></div></div>
        <h1>Authentication Error</h1>
        <p>Invalid endpoint or missing authentication data.</p>
        <p>Please try logging in again.</p>
    </div>
</body>
</html>
    )"),
                           *LogoBase64);
}

bool FOAuthHttpServerService::HandleControlRequest(const FHttpServerRequest &Request, const FHttpResultCallback &OnComplete)
{
    FString ApiKey;
    if (Request.QueryParams.Contains("api_key"))
    {
        ApiKey = Request.QueryParams["api_key"];
    }

    FString UserInfo;
    if (Request.QueryParams.Contains("user_info"))
    {
        UserInfo = Request.QueryParams["user_info"];
    }

    FString LogoBase64 = LoadLogoAsBase64();
    FString ResponseHtml = !ApiKey.IsEmpty() ? GenerateSuccessHTML(LogoBase64) : GenerateErrorHTML(LogoBase64);

    if (!ApiKey.IsEmpty())
    {
        ApiKeyReceivedDelegate.Broadcast(ApiKey, UserInfo);
    }

    FTCHARToUTF8 UTF8String(*ResponseHtml);
    TArray<uint8> ResponseData;
    ResponseData.Append((uint8 *)UTF8String.Get(), UTF8String.Length());

    TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseData, TEXT("text/html; charset=utf-8"));
    OnComplete(MoveTemp(Response));
    return true;
}

FString FOAuthHttpServerService::GetLogoPath()
{
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Convai"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("OAuthHttpServerService: Convai plugin not found"));
        return FString();
    }

    return FPaths::Combine(
        Plugin->GetBaseDir(),
        ConvaiEditor::Constants::PluginResources::Root,
        ConvaiEditor::Constants::Icons::Logo);
}

FString FOAuthHttpServerService::LoadLogoAsBase64() const
{
    FString LogoPath = GetLogoPath();
    if (LogoPath.IsEmpty())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("OAuthHttpServerService: Failed to get logo path"));
        return FString();
    }

    TArray<uint8> FileData;
    if (!FFileHelper::LoadFileToArray(FileData, *LogoPath))
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("OAuthHttpServerService: Failed to load logo from path: %s"), *LogoPath);
        return FString();
    }

    return FBase64::Encode(FileData);
}