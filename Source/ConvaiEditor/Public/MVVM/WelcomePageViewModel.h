/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * WelcomePageViewModel.h
 *
 * ViewModel for the Welcome Page.
 */

#pragma once

#include "CoreMinimal.h"
#include "MVVM/ObservableProperty.h"
#include "MVVM/ValidatedProperty.h"
#include "MVVM/ViewModel.h"
#include "Services/IWelcomeService.h"

/** Welcome flow steps enum. */
enum class EWelcomeStep
{
    Welcome,    // Initial welcome screen
    ApiKeyInput // API key input and validation
};

/** ViewModel for the Welcome Page. */
class CONVAIEDITOR_API FWelcomePageViewModel : public FViewModelBase
{
public:
    FWelcomePageViewModel();
    virtual ~FWelcomePageViewModel() = default;

    /** Current step in the welcome flow */
    ConvaiEditor::TObservableProperty<EWelcomeStep> CurrentStep;

    /** API key input text - with validation */
    FValidatedString ApiKeyText;

    /** Error message to display */
    FObservableString ErrorMessage;

    /** Whether the API key is visible */
    FObservableBool IsApiKeyVisible;

    /** Whether validation is in progress */
    FObservableBool IsValidating;

    /** Whether to show the welcome step */
    bool IsWelcomeStep() const { return CurrentStep.Get() == EWelcomeStep::Welcome; }

    /** Whether to show the API key input step */
    bool IsApiKeyStep() const { return CurrentStep.Get() == EWelcomeStep::ApiKeyInput; }

    /** Whether to show error message */
    bool HasError() const { return !ErrorMessage.Get().IsEmpty(); }

    /** Whether the continue button should be enabled */
    bool CanContinue() const;

    /** Whether the validate button should be enabled */
    bool CanValidate() const;

    /** Command to continue from welcome to API key input */
    void ContinueToApiKey();

    /** Command to validate and store API key */
    void ValidateApiKey();

    /** Command to toggle API key visibility */
    void ToggleApiKeyVisibility();

    /** Command to close the welcome window */
    void CloseWelcome();

    /** Called when API key text changes */
    void OnApiKeyTextChanged(const FString &NewText);

    /** Called when API key validation succeeds */
    void OnApiKeyValidated(const FString &ApiKey);

    /** Called when API key validation fails */
    void OnApiKeyValidationFailed(const FString &Error);

    /** Initialize the ViewModel */
    virtual void Initialize() override;

    /** Clean up resources */
    virtual void Shutdown() override;

    /** Returns the type name used for registry lookup */
    static FName StaticType() { return TEXT("FWelcomePageViewModel"); }

private:
    /** Get the welcome service */
    TSharedPtr<IWelcomeService> GetWelcomeService() const;

    /** Clear error message */
    void ClearError();

    /** Set error message */
    void SetError(const FString &Error);

    /** Navigate to next step */
    void NavigateToStep(EWelcomeStep Step);

    TWeakPtr<IWelcomeService> WelcomeService;
};
