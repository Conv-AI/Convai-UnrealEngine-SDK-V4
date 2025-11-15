/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * OAuthHttpServerService.h
 *
 * Manages local HTTP server for OAuth callback handling.
 */

#pragma once

#include "Services/OAuth/IOAuthHttpServerService.h"
#include "HttpServerModule.h"
#include "HttpRouteHandle.h"
#include "HttpServerResponse.h"

/**
 * Manages local HTTP server for OAuth callback handling.
 */
class FOAuthHttpServerService : public IOAuthHttpServerService, public TSharedFromThis<FOAuthHttpServerService>
{
public:
    FOAuthHttpServerService();
    virtual ~FOAuthHttpServerService() override;

    virtual void Startup() override;
    virtual void Shutdown() override;
    virtual bool StartServer(const TArray<int32> &PreferredPorts) override;
    virtual void StopServer() override;
    virtual bool IsRunning() const override { return bIsRunning; }
    virtual int32 GetPort() const override { return CurrentPort; }
    virtual FOnApiKeyReceived &OnApiKeyReceived() override { return ApiKeyReceivedDelegate; }

private:
    bool IsPortFree(int32 Port) const;
    bool HandleControlRequest(const FHttpServerRequest &Request, const FHttpResultCallback &OnComplete);

    /** Loads Convai logo from plugin resources as base64 */
    FString LoadLogoAsBase64() const;

    /** Returns full path to logo file from plugin resources */
    static FString GetLogoPath();

    /** Generates success response HTML */
    static FString GenerateSuccessHTML(const FString &LogoBase64);

    /** Generates error response HTML */
    static FString GenerateErrorHTML(const FString &LogoBase64);

    bool bIsRunning;
    int32 CurrentPort;
    FHttpRouteHandle ControlRouteHandle;
    FOnApiKeyReceived ApiKeyReceivedDelegate;
};