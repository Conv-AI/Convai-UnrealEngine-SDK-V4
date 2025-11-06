/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AuthWindowManager.h
 *
 * Manages authentication window lifecycle in the Convai Editor.
 */

#pragma once

#include "Services/IAuthWindowManager.h"
#include "Services/OAuth/IOAuthAuthenticationService.h"
#include "Services/IWelcomeService.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Delegates/Delegate.h"

/**
 * Manages authentication window lifecycle in the Convai Editor.
 */
class FAuthWindowManager : public IAuthWindowManager, public TSharedFromThis<FAuthWindowManager>
{
public:
    FAuthWindowManager();
    virtual ~FAuthWindowManager();

    /** Starts the authentication flow */
    virtual void StartAuthFlow() override;

    /** Handles successful authentication */
    virtual void OnAuthSuccess() override;

    /** Handles cancelled authentication */
    virtual void OnAuthCancelled() override;

    /** Handles authentication error */
    virtual void OnAuthError(const FString &Error) override;

    /** Returns whether the auth window is open */
    virtual bool IsAuthWindowOpen() const override;

    /** Returns whether the welcome window is open */
    virtual bool IsWelcomeWindowOpen() const override;

    /** Returns the current authentication flow state */
    virtual EAuthFlowState GetAuthState() const override;

    /** Closes the authentication window */
    virtual void CloseAuthWindow() override;

    /** Opens the welcome window */
    virtual void OpenWelcomeWindow() override;

    /** Closes the welcome window */
    virtual void CloseWelcomeWindow() override;

    /** Returns the auth flow started event delegate */
    virtual FOnAuthFlowStarted &OnAuthFlowStarted() override { return AuthFlowStartedDelegate; }

    /** Returns the auth flow completed event delegate */
    virtual FOnAuthFlowCompleted &OnAuthFlowCompleted() override { return AuthFlowCompletedDelegate; }

    /** Returns the welcome window requested event delegate */
    virtual FOnWelcomeWindowRequested &OnWelcomeWindowRequested() override { return WelcomeWindowRequestedDelegate; }

    /** Initializes the service */
    virtual void Startup() override;

    /** Shuts down the service */
    virtual void Shutdown() override;

private:
    EAuthFlowState CurrentState;
    FString LastErrorMessage;

    /** Transitions to a new state */
    void TransitionToState(EAuthFlowState NewState);

    /** Handles state transition logic */
    void HandleStateTransition(EAuthFlowState OldState, EAuthFlowState NewState);

    TSharedPtr<IOAuthAuthenticationService> AuthService;
    TSharedPtr<IWelcomeService> WelcomeService;

    /** Lazily resolves AuthService on first use */
    TSharedPtr<IOAuthAuthenticationService> GetAuthService();

    /** Lazily resolves WelcomeService on first use */
    TSharedPtr<IWelcomeService> GetWelcomeService();

    TWeakPtr<SWindow> AuthWindow;
    TWeakPtr<SWindow> WelcomeWindow;

    /** Handles OAuth success */
    void HandleOAuthSuccess();

    /** Handles OAuth failure */
    void HandleOAuthFailure(const FString &Error);

    FOnAuthFlowStarted AuthFlowStartedDelegate;
    FOnAuthFlowCompleted AuthFlowCompletedDelegate;
    FOnWelcomeWindowRequested WelcomeWindowRequestedDelegate;

    FDelegateHandle OAuthSuccessHandle;
    FDelegateHandle OAuthFailureHandle;
};