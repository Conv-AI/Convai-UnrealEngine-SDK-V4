/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * OAuthAuthenticationService.cpp
 *
 * Implementation of OAuth authentication service.
 */

#include "Services/OAuth/OAuthAuthenticationService.h"
#include "Services/ConvaiDIContainer.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Engine/Engine.h"
#include "Services/IWelcomeService.h"
#include "UI/Widgets/SConvaiLoadingScreen.h"
#include "Services/OAuth/IOAuthHttpServerService.h"
#include "Services/OAuth/IDecryptionService.h"
#include "UI/Shell/SAuthShell.h"
#include "Utility/ConvaiConstants.h"
#include "Utility/ConvaiErrorHandling.h"
#include "Models/ConvaiUserInfo.h"
#include "Services/ConfigurationService.h"
#include "Misc/App.h"
#include "HAL/PlatformProcess.h"

FOAuthAuthenticationService::FOAuthAuthenticationService()
    : bIsAuthenticating(false)
{
    bIsShuttingDown.store(false, std::memory_order_relaxed);
}

FOAuthAuthenticationService::~FOAuthAuthenticationService()
{
    Shutdown();
}

void FOAuthAuthenticationService::Startup()
{
    auto HttpServerResult = FConvaiDIContainerManager::Get().Resolve<IOAuthHttpServerService>();
    if (HttpServerResult.IsSuccess())
    {
        HttpServerService = HttpServerResult.GetValue();
    }
    else
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("OAuthAuthenticationService: failed to resolve HttpServerService - %s"),
               *HttpServerResult.GetError());
    }

    auto DecryptionResult = FConvaiDIContainerManager::Get().Resolve<IDecryptionService>();
    if (DecryptionResult.IsSuccess())
    {
        DecryptionService = DecryptionResult.GetValue();
    }
    else
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("OAuthAuthenticationService: failed to resolve DecryptionService - %s"),
               *DecryptionResult.GetError());
    }
}

void FOAuthAuthenticationService::Shutdown()
{
    bool expected = false;
    if (!bIsShuttingDown.compare_exchange_strong(expected, true, std::memory_order_seq_cst))
    {
        return;
    }

    CancelLogin();
    
    if (HttpServerService.IsValid())
    {
        HttpServerService->Shutdown();
    }
    HttpServerService.Reset();
    DecryptionService.Reset();
}

bool FOAuthAuthenticationService::IsAuthenticated() const
{
    return !DecryptedApiKey.IsEmpty();
}

void FOAuthAuthenticationService::StartLogin()
{
    if (bIsAuthenticating)
    {
        return;
    }

    if (!HttpServerService.IsValid())
    {
        AuthFailureDelegate.Broadcast(TEXT("HTTP Server service unavailable"));
        return;
    }

    if (!HttpServerService->StartServer(ConvaiEditor::Constants::OAuth::DefaultPorts))
    {
        AuthFailureDelegate.Broadcast(TEXT("Failed to start local HTTP server"));
        return;
    }

    HttpServerService->OnApiKeyReceived().AddSP(this, &FOAuthAuthenticationService::HandleAuthDataReceived);

    OpenBrowserWindow(HttpServerService->GetPort());

    bIsAuthenticating = true;
}

void FOAuthAuthenticationService::CancelLogin()
{
    if (!bIsAuthenticating)
    {
        return;
    }

    bIsAuthenticating = false;

    if (HttpServerService.IsValid())
    {
        HttpServerService->OnApiKeyReceived().RemoveAll(this);
        HttpServerService->StopServer();
    }

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
    if (AuthShell.IsValid())
    {
        if (TSharedPtr<SAuthShell> Shell = AuthShell.Pin())
        {
            if (FSlateApplication::IsInitialized())
            {
                Shell->RequestDestroyWindow();
            }
            
            // CRITICAL: CEF requires time to cleanup GPU/thread resources - defer destruction to avoid crash
            TWeakPtr<SAuthShell> CapturedShell = AuthShell;
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
                [CapturedShell](float) -> bool
                {
                    return false;
                }
            ), 0.3f);
        }
        AuthShell.Reset();
    }
#endif

    if (CloseBrowserTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(CloseBrowserTickerHandle);
        CloseBrowserTickerHandle.Reset();
    }

    AuthWindowClosedDelegate.Broadcast();

    if (OnWindowClosedCallback.IsBound())
    {
        OnWindowClosedCallback.Execute();
    }
}

void FOAuthAuthenticationService::OpenBrowserWindow(int32 Port)
{
    FString Url = FString::Printf(TEXT("https://login.convai.com/?ue=true&port=%d"), Port);

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 1)
    // Use external browser for UE versions older than 5.1 due to outdated CEF
    FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
#else
    // Open in embedded CEF browser (SAuthShell)
    // SAuthShell will handle OAuth buttons (Google/GitHub) by opening them in external browser
    // while keeping email/password login inside CEF
    TSharedRef<SAuthShell> Shell = SNew(SAuthShell);
    Shell->InitWithURL(Url);

    AuthShellClosedHandle = Shell->GetOnWindowClosedEvent().AddSP(this, &FOAuthAuthenticationService::HandleAuthShellWindowClosed);

    FSlateApplication::Get().AddWindow(Shell);
    AuthShell = Shell;
#endif
}

void FOAuthAuthenticationService::CloseBrowserWindow()
{
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
    if (AuthShell.IsValid())
    {
        if (TSharedPtr<SAuthShell> Shell = AuthShell.Pin())
        {
            if (AuthShellClosedHandle.IsValid())
            {
                Shell->GetOnWindowClosedEvent().Remove(AuthShellClosedHandle);
                AuthShellClosedHandle.Reset();
            }

            if (FSlateApplication::IsInitialized())
            {
                Shell->RequestDestroyWindow();
            }
        }
        AuthShell.Reset();
    }
#else
#endif
}

void FOAuthAuthenticationService::HandleAuthDataReceived(const FString &EncryptedKey, const FString &EncryptedUserInfo)
{
    HttpServerService->StopServer();

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
    if (TSharedPtr<SAuthShell> Shell = AuthShell.Pin())
    {
        Shell->ShowOverlay(FText::FromString("Completing authentication..."));
    }
#endif

    if (!DecryptionService.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("OAuthAuthenticationService: DecryptionService unavailable"));
        AuthFailureDelegate.Broadcast(TEXT("Decryption service unavailable"));
        bIsAuthenticating = false;
        return;
    }

    TWeakPtr<FOAuthAuthenticationService> WeakSelfPtr = AsShared();

    DecryptionService->DecryptAsync(
        EncryptedKey,
        [WeakSelfPtr, EncryptedUserInfo](const FString &DecryptedData)
        {
            TSharedPtr<FOAuthAuthenticationService> PinnedThis = WeakSelfPtr.Pin();
            if (!PinnedThis.IsValid())
            {
                UE_LOG(LogConvaiEditor, Warning, TEXT("OAuthAuthenticationService: instance destroyed during decryption"));
                return;
            }

            PinnedThis->DecryptedApiKey = DecryptedData;

            if (!EncryptedUserInfo.IsEmpty() && PinnedThis->DecryptionService.IsValid())
            {
                PinnedThis->DecryptionService->DecryptAsync(
                    EncryptedUserInfo,
                    [WeakSelfPtr](const FString &DecryptedUserInfo)
                    {
                        FConvaiUserInfo UserInfo;
                        if (FConvaiUserInfo::FromJson(DecryptedUserInfo, UserInfo))
                        {
                            auto ConfigResult = FConvaiDIContainerManager::Get().Resolve<IConfigurationService>();
                            if (ConfigResult.IsSuccess())
                            {
                                TSharedPtr<IConfigurationService> ConfigSvc = ConfigResult.GetValue();
                                ConfigSvc->SetUserInfo(UserInfo);
                            }
                        }
                    },
                    [](const FString &ErrorMessage)
                    {
                        UE_LOG(LogConvaiEditor, Warning, TEXT("OAuthAuthenticationService: user info decryption failed - %s"), *ErrorMessage);
                    });
            }

            auto WelcomeResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeService>();
            if (WelcomeResult.IsSuccess())
            {
                TSharedPtr<IWelcomeService> WelcomeSvc = WelcomeResult.GetValue();
                WelcomeSvc->ValidateAndStoreApiKey(PinnedThis->DecryptedApiKey);
            }

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
            if (TSharedPtr<SAuthShell> Shell = PinnedThis->AuthShell.Pin())
            {
                Shell->ShowOverlay(FText::FromString("Launching Convai..."));
            }

            if (PinnedThis->AuthShell.IsValid())
            {
#endif
                PinnedThis->CloseBrowserTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                    FTickerDelegate::CreateLambda([WeakSelfPtr](float)
                                                  {
                                                  TSharedPtr<FOAuthAuthenticationService> PinnedThis = WeakSelfPtr.Pin();
                                                  if (!PinnedThis.IsValid())
                                                  {
                                                      return false;
                                                  }

                                                auto WelcomeResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeService>();
                                                if (WelcomeResult.IsSuccess())
                                                {
                                                    TSharedPtr<IWelcomeService> WelcomeSvc = WelcomeResult.GetValue();
                                                    WelcomeSvc->MarkWelcomeCompleted();
                                                }

                                                auto WindowResult = ConvaiEditor::ErrorHandling::SafeOpenConvaiWindow(false);
                                                if (WindowResult.IsFailure())
                                                {
                                                    UE_LOG(LogConvaiEditor, Error, TEXT("OAuthAuthenticationService: failed to open Convai window - %s"), *WindowResult.GetError());
                                                }

                                                PinnedThis->CloseBrowserWindow();
                                                  return false; }),
                    ConvaiEditor::Constants::OAuth::WindowCloseDelaySeconds);
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
            }
#endif

            PinnedThis->AuthSuccessDelegate.Broadcast();
            PinnedThis->bIsAuthenticating = false;
        },
        [WeakSelfPtr](const FString &ErrorMessage)
        {
            TSharedPtr<FOAuthAuthenticationService> PinnedThis = WeakSelfPtr.Pin();
            if (!PinnedThis.IsValid())
            {
                UE_LOG(LogConvaiEditor, Warning, TEXT("OAuthAuthenticationService: instance destroyed during error handling"));
                return;
            }

            UE_LOG(LogConvaiEditor, Error, TEXT("OAuthAuthenticationService: authentication failed - %s"), *ErrorMessage);

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
            if (TSharedPtr<SAuthShell> Shell = PinnedThis->AuthShell.Pin())
            {
                Shell->ShowOverlay(FText::FromString(FString::Printf(TEXT("Decryption failed: %s"), *ErrorMessage)));
            }
#endif

            PinnedThis->AuthFailureDelegate.Broadcast(ErrorMessage);
            PinnedThis->bIsAuthenticating = false;
        });
}

void FOAuthAuthenticationService::ShowLoadingScreen()
{
    if (LoadingWindow.IsValid())
    {
        return;
    }

    TSharedRef<SWindow> Window = SNew(SWindow)
                                     .Title(FText::FromString("Convai"))
                                     .AutoCenter(EAutoCenter::PrimaryWorkArea)
                                     .CreateTitleBar(false)
                                     .SizingRule(ESizingRule::FixedSize)
                                     .ClientSize(FVector2D(400, 200));

    Window->SetContent(SNew(SConvaiLoadingScreen).Message(FText::FromString("Finishing authentication...")));

    FSlateApplication::Get().AddWindow(Window);
    LoadingWindow = Window;
}

void FOAuthAuthenticationService::HideLoadingScreen()
{
    if (TSharedPtr<SWindow> Window = LoadingWindow.Pin())
    {
        if (FSlateApplication::IsInitialized())
        {
            Window->RequestDestroyWindow();
        }
        LoadingWindow.Reset();
    }
}

void FOAuthAuthenticationService::HandleAuthShellWindowClosed(const TSharedRef<SWindow, ESPMode::ThreadSafe> &Window)
{
    CancelLogin();
}