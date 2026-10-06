/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SWelcomePage.h
 *
 * Welcome page for onboarding new users.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "MVVM/WelcomePageViewModel.h"
#include "MVVM/SlateBinding.h"
#include "Services/OAuth/IOAuthAuthenticationService.h"
#include "Services/IAuthWindowManager.h"

/**
 * Welcome page for onboarding new users.
 */
class CONVAIEDITOR_API SWelcomePage : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SWelcomePage) {}
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

private:
    TSharedRef<SWidget> CreateMainContent();
    TSharedRef<SWidget> CreateWelcomeStep();
    TSharedRef<SWidget> CreateApiKeyStep();
    TSharedRef<SWidget> CreateApiKeyInput();
    TSharedRef<SWidget> CreateErrorMessage();
    TSharedRef<SWidget> CreateButton(const FText &Text, const FOnClicked &OnClicked, bool bIsPrimary = false);

    FReply OnContinueClicked();
    FReply OnValidateClicked();
    void OnApiKeyTextChanged(const FText &NewText);
    void OnApiKeyTextCommitted(const FText &NewText, ETextCommit::Type CommitType);
    FReply OnToggleApiKeyVisibilityClicked();
    FReply OnCloseClicked();
    FReply OnConnectClicked();
    void HandleAuthSuccess();
    void HandleAuthFailure(const FString &Error);

    EVisibility GetWelcomeStepVisibility() const;
    EVisibility GetApiKeyStepVisibility() const;
    EVisibility GetErrorMessageVisibility() const;
    FText GetApiKeyText() const;
    FText GetErrorMessageText() const;
    const FSlateBrush *GetApiKeyVisibilityBrush() const;
    bool IsContinueButtonEnabled() const;
    bool IsValidateButtonEnabled() const;

    TSharedPtr<FWelcomePageViewModel> ViewModel;
    TSharedPtr<class SEditableTextBox> ApiKeyTextBox;
    TSharedPtr<IOAuthAuthenticationService> AuthService;
    FDelegateHandle AuthSuccessHandle;
    FDelegateHandle AuthFailureHandle;
};
