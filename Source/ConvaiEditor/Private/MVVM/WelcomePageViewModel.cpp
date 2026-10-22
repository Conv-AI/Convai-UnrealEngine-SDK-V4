/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * WelcomePageViewModel.cpp
 *
 * Implementation of the welcome page view model.
 */

#include "MVVM/WelcomePageViewModel.h"
#include "Services/ConvaiDIContainer.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "Utility/ConvaiValidationUtils.h"

FWelcomePageViewModel::FWelcomePageViewModel()
{
    CurrentStep.Set(EWelcomeStep::Welcome);
    ApiKeyText.Set(FString());
    ErrorMessage.Set(FString());
    IsApiKeyVisible.Set(false);
    IsValidating.Set(false);
}

void FWelcomePageViewModel::Initialize()
{
    FViewModelBase::Initialize();

    FConvaiValidationUtils::ResolveServiceWithCallbacks<IWelcomeService>(
        TEXT("FWelcomePageViewModel::Initialize"),
        [this](TSharedPtr<IWelcomeService> Service)
        {
            WelcomeService = Service;

            if (TSharedPtr<IWelcomeService> ServicePtr = WelcomeService.Pin())
            {
                FDelegateHandle ValidatedHandle = ServicePtr->OnApiKeyValidated().AddSP(this, &FWelcomePageViewModel::OnApiKeyValidated);
                FDelegateHandle ValidationFailedHandle = ServicePtr->OnApiKeyValidationFailed().AddSP(this, &FWelcomePageViewModel::OnApiKeyValidationFailed);

                TrackDelegate(ServicePtr->OnApiKeyValidated(), ValidatedHandle);
                TrackDelegate(ServicePtr->OnApiKeyValidationFailed(), ValidationFailedHandle);
            }
        },
        [](const FString &Error)
        {
            UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeService resolution failed - %s"), *Error);
        });
}

void FWelcomePageViewModel::Shutdown()
{
    FViewModelBase::Shutdown();
    WelcomeService.Reset();
}

bool FWelcomePageViewModel::CanContinue() const
{
    return IsWelcomeStep();
}

bool FWelcomePageViewModel::CanValidate() const
{
    bool bApiKeyStep = IsApiKeyStep();
    bool bNotEmpty = !ApiKeyText.Get().IsEmpty();
    bool bNotValidating = !IsValidating.Get();
    return bApiKeyStep && bNotEmpty && bNotValidating;
}

void FWelcomePageViewModel::ContinueToApiKey()
{
    if (!CanContinue())
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Cannot continue to API key step from current state"));
        return;
    }

    NavigateToStep(EWelcomeStep::ApiKeyInput);
    ClearError();
}

void FWelcomePageViewModel::ValidateApiKey()
{
    if (!CanValidate())
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Cannot validate API key in current state"));
        return;
    }

    FString ApiKey = ApiKeyText.Get();
    if (ApiKey.IsEmpty())
    {
        SetError(TEXT("Please enter an API key"));
        return;
    }

    IsValidating.Set(true);
    ClearError();

    if (TSharedPtr<IWelcomeService> Service = WelcomeService.Pin())
    {
        bool bValidationStarted = Service->ValidateAndStoreApiKey(ApiKey);
        if (bValidationStarted)
        {
            // IsValidating will be set to false by the validation result callbacks
        }
        else
        {
            UE_LOG(LogConvaiEditorConfig, Warning, TEXT("API key validation failed to start"));
            IsValidating.Set(false);
            SetError(TEXT("Failed to start validation. Please try again."));
        }
    }
    else
    {
        SetError(TEXT("Service not available. Please try again."));
        IsValidating.Set(false);
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("WelcomeService not available in ValidateApiKey"));
    }
}

void FWelcomePageViewModel::ToggleApiKeyVisibility()
{
    bool bCurrentVisibility = IsApiKeyVisible.Get();
    IsApiKeyVisible.Set(!bCurrentVisibility);
}

void FWelcomePageViewModel::CloseWelcome()
{
    // Close the welcome window
    if (TSharedPtr<IWelcomeService> Service = WelcomeService.Pin())
    {
        Service->CloseWelcomeWindow();
    }
}

void FWelcomePageViewModel::OnApiKeyTextChanged(const FString &NewText)
{
    ApiKeyText.Set(NewText);
    ClearError();
}

void FWelcomePageViewModel::OnApiKeyValidated(const FString &ApiKey)
{
    IsValidating.Set(false);
    ClearError();
}

void FWelcomePageViewModel::OnApiKeyValidationFailed(const FString &Error)
{
    IsValidating.Set(false);
    SetError(Error);
}

TSharedPtr<IWelcomeService> FWelcomePageViewModel::GetWelcomeService() const
{
    return WelcomeService.Pin();
}

void FWelcomePageViewModel::ClearError()
{
    ErrorMessage.Set(FString());
}

void FWelcomePageViewModel::SetError(const FString &Error)
{
    ErrorMessage.Set(Error);
}

void FWelcomePageViewModel::NavigateToStep(EWelcomeStep Step)
{
    CurrentStep.Set(Step);
}
