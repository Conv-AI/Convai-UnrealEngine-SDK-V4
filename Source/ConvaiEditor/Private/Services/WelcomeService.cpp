/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * WelcomeService.cpp
 *
 * Implementation of welcome flow service.
 */

#include "Services/WelcomeService.h"
#include "Services/ConfigurationService.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/ApiValidationService.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "Framework/Application/SlateApplication.h"
#include "UI/Pages/SWelcomePage.h"
#include "Widgets/SWindow.h"
#include "Styling/CoreStyle.h"
#include "Modules/ModuleManager.h"
#include "UI/Shell/SWelcomeShell.h"
#include "Utility/ConvaiConstants.h"
#include "Utility/ConvaiWindowUtils.h"
#include "Utility/ConvaiErrorHandling.h"
#include "Services/IWelcomeWindowManager.h"
#include "Async/Async.h"

#define LOCTEXT_NAMESPACE "FWelcomeService"

const FString FWelcomeService::WELCOME_COMPLETED_KEY = TEXT("welcome.completed");
const FString FWelcomeService::API_KEY_KEY = TEXT("ApiKey");

FWelcomeService::FWelcomeService()
{
}

void FWelcomeService::Startup()
{
    auto ValidationResult = FConvaiDIContainerManager::Get().Resolve<IApiValidationService>();
    if (ValidationResult.IsSuccess())
    {
        ValidationService = ValidationResult.GetValue();
        TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
        if (ValidationServicePtr.IsValid())
        {
            ValidationResultHandle = ValidationServicePtr->OnApiKeyValidationResultDetailed().AddSP(this, &FWelcomeService::OnApiKeyValidationResult);
        }
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeService: failed to resolve ApiValidationService - %s"), *ValidationResult.GetError());
    }
}

void FWelcomeService::Shutdown()
{
    if (ValidationService.IsValid() && ValidationResultHandle.IsValid())
    {
        TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
        if (ValidationServicePtr.IsValid())
        {
            ValidationServicePtr->OnApiKeyValidationResultDetailed().Remove(ValidationResultHandle);
        }
    }

    ValidationService.Reset();
    ValidationResultHandle.Reset();
}

bool FWelcomeService::HasCompletedWelcome() const
{
    FScopeLock Lock(&StateLock);

    auto ConfigSvc = GetConfigurationService();
    if (!ConfigSvc.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("WelcomeService: ConfigurationService not available, assuming welcome not completed"));
        return false;
    }

    return ConfigSvc->GetBool(WELCOME_COMPLETED_KEY, false);
}

void FWelcomeService::MarkWelcomeCompleted()
{
    FScopeLock Lock(&StateLock);

    auto ConfigSvc = GetConfigurationService();
    if (ConfigSvc.IsValid())
    {
        ConfigSvc->SetBool(WELCOME_COMPLETED_KEY, true);
        ConfigSvc->SaveConfig();
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeService: failed to mark welcome completed - ConfigurationService not available"));
    }

    OnWelcomeCompletedDelegate.Broadcast();
}

bool FWelcomeService::HasValidApiKey() const
{
    FScopeLock Lock(&StateLock);

    auto ValidationResult = FConvaiDIContainerManager::Get().Resolve<IApiValidationService>();
    if (ValidationResult.IsSuccess())
    {
        TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationResult.GetValue();
        if (ValidationServicePtr.IsValid())
        {
            TOptional<bool> ApiValidationResult = ValidationServicePtr->GetLastAuthenticationValidationResult();
            if (ApiValidationResult.IsSet())
            {
                return ApiValidationResult.GetValue();
            }
        }
    }

    FString StoredApiKey = GetStoredApiKey();
    return !StoredApiKey.IsEmpty() && IsValidApiKeyFormat(StoredApiKey);
}

bool FWelcomeService::ValidateAndStoreApiKey(const FString &ApiKey)
{
    FScopeLock Lock(&StateLock);

    if (!IsValidApiKeyFormat(ApiKey))
    {
        FString ErrorMessage = TEXT("Invalid API key format. Please enter a valid Convai API key.");
        OnApiKeyValidationFailedDelegate.Broadcast(ErrorMessage);
        return false;
    }

    auto ConfigSvc = GetConfigurationService();
    if (!ConfigSvc.IsValid())
    {
        FString ErrorMessage = TEXT("Failed to store API key: Configuration service not available");
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeService: %s"), *ErrorMessage);
        OnApiKeyValidationFailedDelegate.Broadcast(ErrorMessage);
        return false;
    }

    ConfigSvc->SetApiKey(ApiKey);
    ConfigSvc->SaveConfig();

    TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
    if (ValidationServicePtr.IsValid())
    {
        PendingValidationApiKey = ApiKey;

        ValidationServicePtr->ValidateApiKey(ApiKey, true);

        return true;
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("WelcomeService: API validation service not available, storing API key without validation"));
        OnApiKeyValidatedDelegate.Broadcast(ApiKey);
        MarkWelcomeCompleted();

        auto WindowResult = ConvaiEditor::ErrorHandling::SafeOpenConvaiWindow(true);
        if (WindowResult.IsFailure())
        {
            UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeService: failed to open Convai window - %s"), *WindowResult.GetError());
        }

        return true;
    }
}

void FWelcomeService::OnApiKeyValidationResult(const FApiValidationResult &Result)
{
    TWeakPtr<FWelcomeService> WeakSelfPtr = AsShared();

    AsyncTask(ENamedThreads::GameThread, [WeakSelfPtr, Result]()
              {
        TSharedPtr<FWelcomeService> PinnedThis = WeakSelfPtr.Pin();
        if (!PinnedThis.IsValid())
        {
            UE_LOG(LogConvaiEditorConfig, Warning, TEXT("WelcomeService: instance destroyed during API key validation result processing"));
            return;
        }

        FScopeLock Lock(&PinnedThis->StateLock);

        if (Result.bIsValid)
        {
            if (PinnedThis->IsWelcomeWindowOpen() && PinnedThis->OnApiKeyValidatedDelegate.IsBound())
            {
                PinnedThis->OnApiKeyValidatedDelegate.Broadcast(PinnedThis->PendingValidationApiKey);
            }
            
            if (PinnedThis->IsWelcomeWindowOpen())
            {
                PinnedThis->MarkWelcomeCompleted();
                PinnedThis->CloseWelcomeWindow();
                
                auto WindowResult = ConvaiEditor::ErrorHandling::SafeOpenConvaiWindow(true);
                if (WindowResult.IsFailure())
                {
                    UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeService: failed to open Convai window - %s"), *WindowResult.GetError());
                }
            }
        }
        else
        {
            FString ErrorMessage = TEXT("API key validation failed. Please check your API key and try again.");
            
            if (PinnedThis->IsWelcomeWindowOpen() && PinnedThis->OnApiKeyValidationFailedDelegate.IsBound())
            {
                PinnedThis->OnApiKeyValidationFailedDelegate.Broadcast(ErrorMessage);
            }

            if (PinnedThis->IsWelcomeWindowOpen())
            {
                auto ConfigSvc = PinnedThis->GetConfigurationService();
                if (ConfigSvc.IsValid())
                {
                    ConfigSvc->SetApiKey(TEXT(""));
                    ConfigSvc->SaveConfig();
                }
            }
        }

        PinnedThis->PendingValidationApiKey.Empty(); });
}

FString FWelcomeService::GetStoredApiKey() const
{
    FScopeLock Lock(&StateLock);

    auto ConfigSvc = GetConfigurationService();
    if (ConfigSvc.IsValid())
    {
        return ConfigSvc->GetApiKey();
    }

    return FString();
}

void FWelcomeService::ShowWelcomeWindowIfNeeded()
{
    if (HasCompletedWelcome() && HasValidApiKey())
    {
        return;
    }

    ShowWelcomeWindow();
}

void FWelcomeService::ShowWelcomeWindow()
{
    auto WelcomeWindowManagerResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeWindowManager>();
    if (WelcomeWindowManagerResult.IsSuccess())
    {
        TSharedPtr<IWelcomeWindowManager> WelcomeWindowManager = WelcomeWindowManagerResult.GetValue();
        WelcomeWindowManager->ShowWelcomeWindow();

        if (WelcomeWindowManager->IsWelcomeWindowOpen())
        {
        }
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeService: failed to resolve WelcomeWindowManager - %s"), *WelcomeWindowManagerResult.GetError());
    }
}

void FWelcomeService::CloseWelcomeWindow()
{
    auto WelcomeWindowManagerResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeWindowManager>();
    if (WelcomeWindowManagerResult.IsSuccess())
    {
        TSharedPtr<IWelcomeWindowManager> WelcomeWindowManager = WelcomeWindowManagerResult.GetValue();
        WelcomeWindowManager->CloseWelcomeWindow();
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeService: failed to resolve WelcomeWindowManager - %s"), *WelcomeWindowManagerResult.GetError());
    }
}

bool FWelcomeService::IsWelcomeWindowOpen() const
{
    auto WelcomeWindowManagerResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeWindowManager>();
    if (WelcomeWindowManagerResult.IsSuccess())
    {
        TSharedPtr<IWelcomeWindowManager> WelcomeWindowManager = WelcomeWindowManagerResult.GetValue();
        return WelcomeWindowManager->IsWelcomeWindowOpen();
    }

    UE_LOG(LogConvaiEditorConfig, Warning, TEXT("WelcomeService: WelcomeWindowManager not available, assuming window is closed"));
    return false;
}

TSharedPtr<IConfigurationService> FWelcomeService::GetConfigurationService() const
{
    auto Result = FConvaiDIContainerManager::Get().Resolve<IConfigurationService>();
    if (Result.IsSuccess())
    {
        return Result.GetValue();
    }

    UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeService: failed to resolve ConfigurationService - %s"), *Result.GetError());
    return nullptr;
}

bool FWelcomeService::IsValidApiKeyFormat(const FString &ApiKey) const
{
    if (ApiKey.IsEmpty())
    {
        return false;
    }

    if (ApiKey.Len() < 20 || ApiKey.Len() > 100)
    {
        return false;
    }

    for (TCHAR Char : ApiKey)
    {
        if (!FChar::IsAlnum(Char) && Char != TEXT('_') && Char != TEXT('-'))
        {
            return false;
        }
    }

    return true;
}

void FWelcomeService::OnWelcomeWindowClosed(const TSharedRef<SWindow> &ClosedWindow)
{
}

#undef LOCTEXT_NAMESPACE
