/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SAccountPage.h
 *
 * Account page displaying user subscription details and API key.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Pages/SBasePage.h"
#include "UI/Widgets/SRoundedBox.h"
#include "UI/Widgets/SLoadingIndicator.h"
#include "Styling/ConvaiStyle.h"
#include "MVVM/AccountPageViewModel.h"
#include "Services/ApiValidationService.h"

class IConfigurationService;
class IApiValidationService;

/**
 * Account page displaying user subscription details and API key.
 */
class CONVAIEDITOR_API SAccountPage : public SBasePage
{
public:
    SLATE_BEGIN_ARGS(SAccountPage) {}
    SLATE_END_ARGS()

    SAccountPage();

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    static FName StaticClass()
    {
        static FName TypeName = FName("SAccountPage");
        return TypeName;
    }

    virtual bool IsA(const FName &TypeName) const override
    {
        return TypeName == StaticClass() || SBasePage::IsA(TypeName);
    }

    virtual TSharedPtr<FViewModelBase> GetViewModel() const override
    {
        return StaticCastSharedPtr<FViewModelBase>(AccountViewModel);
    }

    virtual void OnPageActivated() override;

    ~SAccountPage();

private:
    TSharedRef<SWidget> CreateAccountDetailsBox();
    TSharedRef<SWidget> CreateApiKeyBox();
    TSharedRef<SWidget> CreateUsagesBox();
    TSharedRef<SWidget> CreateUsageProgressBar(const FText &Label, TAttribute<float> Percent, TAttribute<FText> ValueText);

    FReply OnToggleApiKeyVisibility();
    const FSlateBrush *GetApiKeyVisibilityBrush() const;
    void OnApiKeyChanged(const FText &NewText);
    void OnApiKeyCommitted(const FText &NewText, ETextCommit::Type CommitType);
    FText GetApiKeyText() const;

    bool bIsApiKeyVisible = false;
    FString ApiKeyValue = TEXT("");
    FString MaskedApiKey = TEXT("••••••••••••••••••••••••••••••");
    TSharedPtr<class SEditableTextBox> ApiKeyTextBox;

    TWeakPtr<IConfigurationService> ConfigService;
    FDelegateHandle ApiKeyChangedHandle;
    FDelegateHandle AuthenticationResultHandle;

    void HandleApiKeyChanged(const FString &NewApiKey);
    void HandleAuthenticationChanged();

    TWeakPtr<IApiValidationService> ValidationService;
    FDelegateHandle ValidationResultHandle;

    void HandleValidationResult(const FApiValidationResult &Result);

    bool bIsApiKeyValid = false;
    bool IsApiKeyValidating() const;
    TOptional<bool> GetApiKeyValidationResult() const;

    TSharedPtr<FAccountPageViewModel> AccountViewModel;
    FDelegateHandle UsageChangedHandle;
    FDelegateHandle LoadingStateChangedHandle;

    void RefreshAccountData();
    void HandleLoadingStateChanged(bool bIsLoading, const FText &Message);

    TSharedPtr<class SLoadingIndicator> LoadingIndicatorWidget;

    FText GetUserName() const;
    FText GetPlanName() const;
    FText GetRenewDate() const;
    float GetInteractionUsageCurrent() const;
    float GetInteractionUsageLimit() const;
    float GetElevenlabsUsageCurrent() const;
    float GetElevenlabsUsageLimit() const;
    float GetCoreAPIUsageCurrent() const;
    float GetCoreAPIUsageLimit() const;
    float GetPixelStreamingUsageCurrent() const;
    float GetPixelStreamingUsageLimit() const;

    float GetInteractionUsagePercent() const;
    FText GetInteractionUsageText() const;
    float GetElevenlabsUsagePercent() const;
    FText GetElevenlabsUsageText() const;
    float GetCoreAPIUsagePercent() const;
    FText GetCoreAPIUsageText() const;
    float GetPixelStreamingUsagePercent() const;
    FText GetPixelStreamingUsageText() const;
};
