/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AuthWindowManager.cpp
 *
 * Implementation of authentication window manager.
 */

#include "Services/AuthWindowManager.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/IWelcomeWindowManager.h"
#include "UI/Shell/SAuthShell.h"
#include "UI/Pages/SWelcomePage.h"
#include "Framework/Application/SlateApplication.h"
#include "Logging/ConvaiEditorConfigLog.h"

FAuthWindowManager::FAuthWindowManager()
    : CurrentState(EAuthFlowState::Welcome), LastErrorMessage()
{
}

FAuthWindowManager::~FAuthWindowManager()
{
    Shutdown();
}

void FAuthWindowManager::Startup()
{
}

TSharedPtr<IOAuthAuthenticationService> FAuthWindowManager::GetAuthService()
{
    if (!AuthService.IsValid())
    {
        auto AuthResult = FConvaiDIContainerManager::Get().Resolve<IOAuthAuthenticationService>();
        if (AuthResult.IsSuccess())
        {
            AuthService = AuthResult.GetValue();

            if (AuthService.IsValid())
            {
                OAuthSuccessHandle = AuthService->OnAuthSuccess().AddSP(this, &FAuthWindowManager::HandleOAuthSuccess);
                OAuthFailureHandle = AuthService->OnAuthFailure().AddSP(this, &FAuthWindowManager::HandleOAuthFailure);
                AuthService->SetOnWindowClosedCallback(FSimpleDelegate::CreateSP(this, &FAuthWindowManager::OnAuthCancelled));
            }
        }
    }
    return AuthService;
}

TSharedPtr<IWelcomeService> FAuthWindowManager::GetWelcomeService()
{
    if (!WelcomeService.IsValid())
    {
        auto WelcomeResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeService>();
        if (WelcomeResult.IsSuccess())
        {
            WelcomeService = WelcomeResult.GetValue();
        }
    }
    return WelcomeService;
}

void FAuthWindowManager::Shutdown()
{
    if (AuthService.IsValid())
    {
        if (OAuthSuccessHandle.IsValid())
        {
            AuthService->OnAuthSuccess().Remove(OAuthSuccessHandle);
            OAuthSuccessHandle.Reset();
        }

        if (OAuthFailureHandle.IsValid())
        {
            AuthService->OnAuthFailure().Remove(OAuthFailureHandle);
            OAuthFailureHandle.Reset();
        }
    }

    CloseAuthWindow();
    CloseWelcomeWindow();

    AuthService.Reset();
    WelcomeService.Reset();
}

void FAuthWindowManager::StartAuthFlow()
{
    if (CurrentState != EAuthFlowState::Welcome)
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Cannot start auth flow from state: %d"), (int32)CurrentState);
        return;
    }

    CloseWelcomeWindow();

    auto Service = GetAuthService();
    if (Service.IsValid())
    {
        Service->StartLogin();
        TransitionToState(EAuthFlowState::Authenticating);
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("Auth service not available"));
        OnAuthError(TEXT("Authentication service not available"));
    }
}

void FAuthWindowManager::OnAuthSuccess()
{
    TransitionToState(EAuthFlowState::Success);
}

void FAuthWindowManager::OnAuthCancelled()
{
    TransitionToState(EAuthFlowState::Welcome);
}

void FAuthWindowManager::OnAuthError(const FString &Error)
{
    LastErrorMessage = Error;
    TransitionToState(EAuthFlowState::Error);
}

bool FAuthWindowManager::IsAuthWindowOpen() const
{
    return AuthWindow.IsValid();
}

bool FAuthWindowManager::IsWelcomeWindowOpen() const
{
    auto WelcomeWindowManagerResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeWindowManager>();
    if (WelcomeWindowManagerResult.IsSuccess())
    {
        TSharedPtr<IWelcomeWindowManager> WelcomeWindowManager = WelcomeWindowManagerResult.GetValue();
        return WelcomeWindowManager->IsWelcomeWindowOpen();
    }

    return WelcomeWindow.IsValid();
}

EAuthFlowState FAuthWindowManager::GetAuthState() const
{
    return CurrentState;
}

void FAuthWindowManager::CloseAuthWindow()
{
    if (AuthWindow.IsValid())
    {
        AuthWindow.Pin()->RequestDestroyWindow();
        AuthWindow.Reset();
    }

    auto Service = GetAuthService();
    if (Service.IsValid() && CurrentState == EAuthFlowState::Authenticating)
    {
        Service->CancelLogin();
    }
}

void FAuthWindowManager::OpenWelcomeWindow()
{
    auto WelcomeWindowManagerResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeWindowManager>();
    if (WelcomeWindowManagerResult.IsSuccess())
    {
        TSharedPtr<IWelcomeWindowManager> WelcomeWindowManager = WelcomeWindowManagerResult.GetValue();

        if (WelcomeWindowManager->IsWelcomeWindowOpen())
        {
            return;
        }

        WelcomeWindowManager->OnWelcomeWindowClosed().AddLambda([this]()
                                                                {
            if (CurrentState == EAuthFlowState::Authenticating)
            {
                OnAuthCancelled();
            } });

        WelcomeWindowManager->ShowWelcomeWindow();
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to resolve WelcomeWindowManager - %s"), *WelcomeWindowManagerResult.GetError());
    }
}

void FAuthWindowManager::CloseWelcomeWindow()
{
    auto WelcomeWindowManagerResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeWindowManager>();
    if (WelcomeWindowManagerResult.IsSuccess())
    {
        TSharedPtr<IWelcomeWindowManager> WelcomeWindowManager = WelcomeWindowManagerResult.GetValue();
        WelcomeWindowManager->CloseWelcomeWindow();
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to resolve WelcomeWindowManager - %s"), *WelcomeWindowManagerResult.GetError());
    }
}

void FAuthWindowManager::TransitionToState(EAuthFlowState NewState)
{
    EAuthFlowState OldState = CurrentState;
    CurrentState = NewState;

    HandleStateTransition(OldState, NewState);
}

void FAuthWindowManager::HandleStateTransition(EAuthFlowState OldState, EAuthFlowState NewState)
{
    switch (NewState)
    {
    case EAuthFlowState::Welcome:
        if (!IsWelcomeWindowOpen())
        {
            OpenWelcomeWindow();
        }
        break;

    case EAuthFlowState::Authenticating:
        CloseWelcomeWindow();
        AuthFlowStartedDelegate.Broadcast();
        break;

    case EAuthFlowState::Success:
        CloseAuthWindow();
        CloseWelcomeWindow();
        AuthFlowCompletedDelegate.Broadcast();
        break;

    case EAuthFlowState::Error:
        CloseAuthWindow();
        OpenWelcomeWindow();
        AuthFlowCompletedDelegate.Broadcast();
        break;
    }
}

void FAuthWindowManager::HandleOAuthSuccess()
{
    OnAuthSuccess();
}

void FAuthWindowManager::HandleOAuthFailure(const FString &Error)
{
    UE_LOG(LogConvaiEditorConfig, Warning, TEXT("OAuth authentication failed - %s"), *Error);
    OnAuthError(Error);
}