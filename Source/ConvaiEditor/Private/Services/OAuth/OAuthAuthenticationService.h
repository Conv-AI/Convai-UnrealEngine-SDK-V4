/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * OAuthAuthenticationService.h
 *
 * Manages OAuth authentication flow in the Convai Editor.
 */

#pragma once

#include "Services/OAuth/IOAuthAuthenticationService.h"
#include "Services/OAuth/IOAuthHttpServerService.h"
#include "Services/OAuth/IDecryptionService.h"
#include "Templates/SharedPointer.h"
#include "UI/Widgets/SAuthStatusOverlay.h"
#include "Containers/Ticker.h"
#include "UI/Shell/SAuthShell.h"
#include <atomic>

/**
 * Manages OAuth authentication flow in the Convai Editor.
 */
class FOAuthAuthenticationService : public IOAuthAuthenticationService, public TSharedFromThis<FOAuthAuthenticationService>
{
public:
    FOAuthAuthenticationService();
    virtual ~FOAuthAuthenticationService() override;

    /** Initializes the service */
    virtual void Startup() override;

    /** Shuts down the service */
    virtual void Shutdown() override;

    /** Starts the login flow */
    virtual void StartLogin() override;

    /** Cancels the current login flow */
    virtual void CancelLogin() override;

    /** Returns whether user is authenticated */
    virtual bool IsAuthenticated() const override;

    /** Returns the auth success event delegate */
    virtual FOnAuthSuccess &OnAuthSuccess() override { return AuthSuccessDelegate; }

    /** Returns the auth failure event delegate */
    virtual FOnAuthFailure &OnAuthFailure() override { return AuthFailureDelegate; }

    /** Returns the auth window closed event delegate */
    virtual FOnAuthWindowClosed &OnAuthWindowClosed() override { return AuthWindowClosedDelegate; }

    /** Returns whether authentication is in progress */
    virtual bool IsAuthInProgress() const override { return bIsAuthenticating; }

    /** Sets the window closed callback */
    virtual void SetOnWindowClosedCallback(FSimpleDelegate Callback) override { OnWindowClosedCallback = Callback; }

private:
    void HandleAuthDataReceived(const FString &EncryptedKey, const FString &EncryptedUserInfo);

    /** Opens browser window for login */
    void OpenBrowserWindow(int32 Port);

    /** Closes browser window */
    void CloseBrowserWindow();

    void ShowLoadingScreen();
    void HideLoadingScreen();

    void HandleAuthShellWindowClosed(const TSharedRef<SWindow, ESPMode::ThreadSafe> &Window);

    TSharedPtr<IOAuthHttpServerService> HttpServerService;
    TSharedPtr<IDecryptionService> DecryptionService;
    TWeakPtr<SAuthShell> AuthShell;
    FDelegateHandle AuthShellClosedHandle;
    FTSTicker::FDelegateHandle CloseBrowserTickerHandle;
    TWeakPtr<SWindow> AuthBrowserWindow;
    TWeakPtr<SWindow> LoadingWindow;
    FString DecryptedApiKey;
    FOnAuthSuccess AuthSuccessDelegate;
    FOnAuthFailure AuthFailureDelegate;
    FOnAuthWindowClosed AuthWindowClosedDelegate;
    FSimpleDelegate OnWindowClosedCallback;
    bool bIsAuthenticating;
    std::atomic<bool> bIsShuttingDown{false};
};