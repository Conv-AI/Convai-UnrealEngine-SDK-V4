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
#include "HAL/PlatformProcess.h"

FAuthWindowManager::FAuthWindowManager()
    : CurrentState(EAuthFlowState::Welcome), LastErrorMessage()
{
    bIsShuttingDown.store(false, std::memory_order_relaxed);
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

TSharedPtr<IWelcomeWindowManager> FAuthWindowManager::GetWelcomeWindowManager()
{
    if (!WelcomeWindowManager.IsValid())
    {
        auto Result = FConvaiDIContainerManager::Get().Resolve<IWelcomeWindowManager>();
        if (Result.IsSuccess())
        {
            WelcomeWindowManager = Result.GetValue();
        }
    }
    return WelcomeWindowManager;
}

void FAuthWindowManager::Shutdown()
{
    bool expected = false;
    if (!bIsShuttingDown.compare_exchange_strong(expected, true, std::memory_order_seq_cst))
    {
        return;
    }

    UE_LOG(LogConvaiEditorConfig, Log, TEXT("AuthWindowManager: Shutting down..."));

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

        AuthService->Shutdown();
    }

    CloseAuthWindow();
    CloseWelcomeWindow();

    // CRITICAL: CEF requires time to cleanup GPU/thread resources - defer service destruction to avoid crash
    TSharedPtr<IOAuthAuthenticationService> CapturedAuthService = AuthService;
    TSharedPtr<IWelcomeService> CapturedWelcomeService = WelcomeService;
    TSharedPtr<IWelcomeWindowManager> CapturedWelcomeWindowManager = WelcomeWindowManager;
    
    AuthService.Reset();
    WelcomeService.Reset();
    WelcomeWindowManager.Reset();
    
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [CapturedAuthService, CapturedWelcomeService, CapturedWelcomeWindowManager](float) -> bool
        {
            return false;
        }
    ), 0.3f);

    UE_LOG(LogConvaiEditorConfig, Log, TEXT("AuthWindowManager: Shutdown complete"));
}

void FAuthWindowManager::StartAuthFlow()
{
    if (CurrentState != EAuthFlowState::Welcome)
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Cannot start auth flow from state: %d"), (int32)CurrentState);
        return;
    }

    // Ensure WelcomeWindowManager is cached before closing to prevent DI deadlock during shutdown
    GetWelcomeWindowManager();

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
    // Use cached WelcomeWindowManager if available, otherwise try to resolve
    if (WelcomeWindowManager.IsValid())
    {
        return WelcomeWindowManager->IsWelcomeWindowOpen();
    }

    auto WelcomeWindowManagerResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeWindowManager>();
    if (WelcomeWindowManagerResult.IsSuccess())
    {
        return WelcomeWindowManagerResult.GetValue()->IsWelcomeWindowOpen();
    }

    return WelcomeWindow.IsValid();
}

EAuthFlowState FAuthWindowManager::GetAuthState() const
{
    return CurrentState;
}

void FAuthWindowManager::CloseAuthWindow()
{
    if (TSharedPtr<SWindow> Window = AuthWindow.Pin())
    {
        if (FSlateApplication::IsInitialized())
        {
            Window->RequestDestroyWindow();
        }
        AuthWindow.Reset();
    }

    // Use cached AuthService instead of resolving from DI container to avoid deadlock during shutdown
    if (AuthService.IsValid() && CurrentState == EAuthFlowState::Authenticating)
    {
        AuthService->CancelLogin();
    }
}

void FAuthWindowManager::OpenWelcomeWindow()
{
    TSharedPtr<IWelcomeWindowManager> Manager = GetWelcomeWindowManager();
    if (Manager.IsValid())
    {
        if (Manager->IsWelcomeWindowOpen())
        {
            return;
        }

        WelcomeWindowClosedHandle = Manager->OnWelcomeWindowClosed().AddSP(this, &FAuthWindowManager::HandleWelcomeWindowClosedDuringAuth);

        Manager->ShowWelcomeWindow();
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to resolve WelcomeWindowManager"));
    }
}

void FAuthWindowManager::CloseWelcomeWindow()
{
    if (WelcomeWindowManager.IsValid())
    {
        if (WelcomeWindowClosedHandle.IsValid())
        {
            WelcomeWindowManager->OnWelcomeWindowClosed().Remove(WelcomeWindowClosedHandle);
            WelcomeWindowClosedHandle.Reset();
        }

        WelcomeWindowManager->CloseWelcomeWindow();
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

void FAuthWindowManager::HandleWelcomeWindowClosedDuringAuth()
{
    if (CurrentState == EAuthFlowState::Authenticating)
    {
        OnAuthCancelled();
    }
}