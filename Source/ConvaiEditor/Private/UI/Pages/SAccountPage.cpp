/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SAccountPage.cpp
 *
 * Implementation of the account page.
 */

#include "UI/Pages/SAccountPage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"
#include "UI/Widgets/SRoundedBox.h"
#include "UI/Widgets/SRoundedProgressBar.h"
#include "UI/Widgets/SLoadingIndicator.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SOverlay.h"
#include "Styling/SlateTypes.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateBrush.h"
#include "UI/Widgets/SConvaiApiKeyInputBox.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/ConfigurationService.h"
#include "Services/ApiValidationService.h"
#include "Utility/ConvaiConstants.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "MVVM/AccountPageViewModel.h"
#include "MVVM/ViewModel.h"

#define LOCTEXT_NAMESPACE "SAccountPage"

SAccountPage::SAccountPage()
    : ApiKeyValue(TEXT("")), bIsApiKeyValid(false), ApiKeyChangedHandle(), ValidationResultHandle(), AuthenticationResultHandle()
{
    auto ConfigResult = FConvaiDIContainerManager::Get().Resolve<IConfigurationService>();
    if (ConfigResult.IsSuccess())
    {
        ConfigService = ConfigResult.GetValue();
        TSharedPtr<IConfigurationService> ConfigServicePtr = ConfigService.Pin();
        if (ConfigServicePtr.IsValid())
        {
            ApiKeyValue = ConfigServicePtr->GetApiKey();
        }
        else
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("SAccountPage: ConfigurationService resolved but is invalid"));
        }
    }
    else
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("SAccountPage: failed to resolve ConfigurationService - %s"), *ConfigResult.GetError());
    }

    auto ValidationResult = FConvaiDIContainerManager::Get().Resolve<IApiValidationService>();
    if (ValidationResult.IsSuccess())
    {
        ValidationService = ValidationResult.GetValue();
    }
    else
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("SAccountPage: failed to resolve ApiValidationService - %s"), *ValidationResult.GetError());
    }

    AccountViewModel = FViewModelRegistry::Get().CreateViewModel<FAccountPageViewModel>();
    if (AccountViewModel.IsValid())
    {
        AccountViewModel->Initialize();
    }
}

void SAccountPage::Construct(const FArguments &InArgs)
{
    const float SpaceBelowTitle = ConvaiEditor::Constants::Layout::Spacing::SpaceBelowTitle;
    const float AccountSectionSpacing = ConvaiEditor::Constants::Layout::Spacing::AccountSectionSpacing;
    const float PaddingWindow = ConvaiEditor::Constants::Layout::Spacing::Window;
    const float ScrollBarThickness = ConvaiEditor::Constants::Layout::Components::ScrollBar::Thickness;
    const float ScrollBarVerticalPadding = ConvaiEditor::Constants::Layout::Components::ScrollBar::VerticalPadding;
    const float AccountHorizontalSpacing = ConvaiEditor::Constants::Layout::Spacing::AccountHorizontalSpacing;

    TSharedRef<SWidget> ContentWidget = SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, AccountSectionSpacing))[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(0.6f).Padding(FMargin(0.0f, 0.0f, AccountHorizontalSpacing, 0.0f))[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, SpaceBelowTitle))[SNew(STextBlock).Text(LOCTEXT("AccountDetails", "Account Details")).Font(FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountSectionTitle"))).ColorAndOpacity(FConvaiStyle::RequireColor(TEXT("Convai.Color.text.accountSection")))] + SVerticalBox::Slot().AutoHeight()[CreateAccountDetailsBox()]] + SHorizontalBox::Slot().FillWidth(0.4f)[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, SpaceBelowTitle))[SNew(STextBlock).Text(LOCTEXT("ApiKey", "API Key")).Font(FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountSectionTitle"))).ColorAndOpacity(FConvaiStyle::RequireColor(TEXT("Convai.Color.text.accountSection")))] + SVerticalBox::Slot().AutoHeight()[CreateApiKeyBox()]]] + SVerticalBox::Slot().AutoHeight()[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, SpaceBelowTitle))[SNew(STextBlock).Text(LOCTEXT("Usages", "Usages")).Font(FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountSectionTitle"))).ColorAndOpacity(FConvaiStyle::RequireColor(TEXT("Convai.Color.text.accountSection")))] + SVerticalBox::Slot().AutoHeight()[CreateUsagesBox()]];

    TSharedRef<SWidget> ScrollableContent = SNew(SScrollBox)
                                                .Style(&FConvaiStyle::GetScrollBoxStyle(true))
                                                .ScrollBarVisibility(EVisibility::Visible)
                                                .ScrollBarThickness(FVector2D(ScrollBarThickness, ScrollBarThickness))
                                                .ScrollBarPadding(FMargin(ScrollBarVerticalPadding, 0.0f)) +
                                            SScrollBox::Slot()
                                                .Padding(PaddingWindow)
                                                    [ContentWidget];

    ChildSlot
        [SNew(SOverlay) + SOverlay::Slot()[ScrollableContent] + SOverlay::Slot()[SAssignNew(LoadingIndicatorWidget, SLoadingIndicator).Size(ELoadingIndicatorSize::Large).Style(ELoadingIndicatorStyle::BrandSpinner).Message_Lambda([this]()
                                                                                                                                                                                                                                     { return AccountViewModel.IsValid() ? AccountViewModel->GetLoadingMessage() : FText::GetEmpty(); })
                                                                                     .ShowOverlay(true)
                                                                                     .Visibility_Lambda([this]()
                                                                                                        { return (AccountViewModel.IsValid() && AccountViewModel->IsLoading()) ? EVisibility::Visible : EVisibility::Collapsed; })]];

    TWeakPtr<SAccountPage> WeakSelf(SharedThis(this));

    // Config service bindings
    if (ConfigService.IsValid())
    {
        TSharedPtr<IConfigurationService> ConfigServicePtr = ConfigService.Pin();
        if (ConfigServicePtr.IsValid())
        {
            ApiKeyChangedHandle = ConfigServicePtr->OnApiKeyChanged().AddLambda([WeakSelf](const FString &NewApiKey)
                                                                                {
                if (TSharedPtr<SAccountPage> Page = WeakSelf.Pin())
                {
                    Page->HandleApiKeyChanged(NewApiKey);
                } });

            AuthenticationResultHandle = ConfigServicePtr->OnAuthenticationChanged().AddLambda([WeakSelf]()
                                                                                               {
                if (TSharedPtr<SAccountPage> Page = WeakSelf.Pin())
                {
                    Page->HandleAuthenticationChanged();
                } });
        }
    }

    // Validation service bindings
    if (ValidationService.IsValid())
    {
        TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
        if (ValidationServicePtr.IsValid())
        {
            ValidationResultHandle = ValidationServicePtr->OnApiKeyValidationResultDetailed().AddLambda([WeakSelf](const FApiValidationResult &Result)
                                                                                                        {
                if (TSharedPtr<SAccountPage> Page = WeakSelf.Pin())
                {
                    Page->HandleValidationResult(Result);
                } });
        }
    }

    // ViewModel bindings
    if (AccountViewModel.IsValid())
    {
        UsageChangedHandle = AccountViewModel->OnUsageChanged().AddLambda([WeakSelf]()
                                                                          {
            if (TSharedPtr<SAccountPage> Page = WeakSelf.Pin())
            {
                Page->RefreshAccountData();
            } });

        LoadingStateChangedHandle = AccountViewModel->OnLoadingStateChanged().AddLambda([WeakSelf](bool bIsLoading, const FText &Message)
                                                                                        {
            if (TSharedPtr<SAccountPage> Page = WeakSelf.Pin())
            {
                Page->HandleLoadingStateChanged(bIsLoading, Message);
            } });

        AccountViewModel->LoadAccountUsage(ApiKeyValue);
    }
}

SAccountPage::~SAccountPage()
{
    if (ConfigService.IsValid() && ApiKeyChangedHandle.IsValid())
    {
        TSharedPtr<IConfigurationService> ConfigServicePtr = ConfigService.Pin();
        if (ConfigServicePtr.IsValid())
        {
            ConfigServicePtr->OnApiKeyChanged().Remove(ApiKeyChangedHandle);
        }
    }

    if (ConfigService.IsValid() && AuthenticationResultHandle.IsValid())
    {
        TSharedPtr<IConfigurationService> ConfigServicePtr = ConfigService.Pin();
        if (ConfigServicePtr.IsValid())
        {
            ConfigServicePtr->OnAuthenticationChanged().Remove(AuthenticationResultHandle);
        }
    }

    if (ValidationService.IsValid() && ValidationResultHandle.IsValid())
    {
        TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
        if (ValidationServicePtr.IsValid())
        {
            ValidationServicePtr->OnApiKeyValidationResultDetailed().Remove(ValidationResultHandle);
        }
    }

    if (AccountViewModel.IsValid())
    {
        if (UsageChangedHandle.IsValid())
        {
            AccountViewModel->OnUsageChanged().Remove(UsageChangedHandle);
        }
        if (LoadingStateChangedHandle.IsValid())
        {
            AccountViewModel->OnLoadingStateChanged().Remove(LoadingStateChangedHandle);
        }
    }
}

void SAccountPage::HandleApiKeyChanged(const FString &NewApiKey)
{
    ApiKeyValue = NewApiKey;
    if (ApiKeyTextBox.IsValid())
    {
        ApiKeyTextBox->SetText(FText::FromString(ApiKeyValue));
    }

    // Trigger validation for the new API key
    TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
    if (ValidationServicePtr.IsValid())
    {
        ValidationServicePtr->ValidateApiKey(ApiKeyValue);
    }
}

void SAccountPage::HandleAuthenticationChanged()
{
    TSharedPtr<IConfigurationService> ConfigServicePtr = ConfigService.Pin();
    if (ConfigServicePtr.IsValid())
    {
        ApiKeyValue = ConfigServicePtr->GetApiKey();
        if (ApiKeyTextBox.IsValid())
        {
            ApiKeyTextBox->SetText(FText::FromString(ApiKeyValue));
        }
    }

    TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
    if (ValidationServicePtr.IsValid())
    {
        ValidationServicePtr->ValidateAuthentication();
    }
}

void SAccountPage::HandleValidationResult(const FApiValidationResult &Result)
{
    this->bIsApiKeyValid = Result.bIsValid;

    this->Invalidate(EInvalidateWidget::LayoutAndVolatility);
}

void SAccountPage::RefreshAccountData()
{
    this->Invalidate(EInvalidateWidget::LayoutAndVolatility);
}

void SAccountPage::HandleLoadingStateChanged(bool bIsLoading, const FText &Message)
{
    this->Invalidate(EInvalidateWidget::LayoutAndVolatility);
}

void SAccountPage::OnPageActivated()
{
    TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
    if (ValidationServicePtr.IsValid() && !ApiKeyValue.IsEmpty())
    {
        ValidationServicePtr->ValidateApiKey(ApiKeyValue);
    }
}

TSharedRef<SWidget> SAccountPage::CreateAccountDetailsBox()
{
    const FSlateFontInfo LabelFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountLabel"));
    const FSlateFontInfo ValueFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountValue"));
    const float BorderRadius = ConvaiEditor::Constants::Layout::Radius::StandardCard;
    const float BorderThickness = ConvaiEditor::Constants::Layout::Components::StandardCard::BorderThickness;
    const FLinearColor BackgroundColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.account.boxBackground"));
    const FLinearColor BorderColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.account.boxBorder"));
    const float PaddingHorizontal = ConvaiEditor::Constants::Layout::Spacing::AccountBox::Horizontal;
    const float PaddingVerticalOuter = ConvaiEditor::Constants::Layout::Spacing::AccountBox::VerticalOuter;
    const float PaddingVerticalInner = ConvaiEditor::Constants::Layout::Spacing::AccountBox::VerticalInner;
    const float HorizontalSpacing = ConvaiEditor::Constants::Layout::Spacing::AccountHorizontalSpacing;
    const FLinearColor TextColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.text.primary"));

    return SNew(SRoundedBox)
        .BorderRadius(BorderRadius)
        .BorderThickness(BorderThickness)
        .BackgroundColor(BackgroundColor)
        .BorderColor(BorderColor)
            [SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(FMargin(PaddingHorizontal, PaddingVerticalInner, PaddingHorizontal, PaddingVerticalInner))[SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, HorizontalSpacing, 0.0f))[SNew(STextBlock).Text(LOCTEXT("PlanLabel", "Plan:")).ColorAndOpacity(TextColor).Font(LabelFont)] + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[SNew(STextBlock).Text(this, &SAccountPage::GetPlanName).ColorAndOpacity(TextColor).Font(ValueFont)]]
             // Plan Expiry
             + SVerticalBox::Slot()
                   .AutoHeight()
                   .Padding(FMargin(PaddingHorizontal, PaddingVerticalInner, PaddingHorizontal, PaddingVerticalInner))
                       [SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, HorizontalSpacing, 0.0f))[SNew(STextBlock).Text(LOCTEXT("PlanExpiryLabel", "Plan Expiry:")).ColorAndOpacity(TextColor).Font(LabelFont)] + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[SNew(STextBlock).Text_Lambda([this]()
                                                                                                                                                                                                                                                                                                                                                                        { return AccountViewModel.IsValid() ? AccountViewModel->GetPlanExpiryText() : FText::GetEmpty(); })
                                                                                                                                                                                                                                                                                                                                               .ColorAndOpacity(TextColor)
                                                                                                                                                                                                                                                                                                                                               .Font(ValueFont)]]
             // Quota Renewal
             + SVerticalBox::Slot()
                   .AutoHeight()
                   .Padding(FMargin(PaddingHorizontal, PaddingVerticalInner, PaddingHorizontal, PaddingVerticalOuter))
                       [SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, HorizontalSpacing, 0.0f))[SNew(STextBlock).Text(LOCTEXT("QuotaRenewalLabel", "Quota Renewal:")).ColorAndOpacity(TextColor).Font(LabelFont)] + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[SNew(STextBlock).Text_Lambda([this]()
                                                                                                                                                                                                                                                                                                                                                                            { return AccountViewModel.IsValid() ? AccountViewModel->GetQuotaRenewalText() : FText::GetEmpty(); })
                                                                                                                                                                                                                                                                                                                                                   .ColorAndOpacity(TextColor)
                                                                                                                                                                                                                                                                                                                                                   .Font(ValueFont)]]];
}

TSharedRef<SWidget> SAccountPage::CreateApiKeyBox()
{
    return SNew(SConvaiApiKeyInputBox)
        .Text(this, &SAccountPage::GetApiKeyText)
        .OnTextChanged(this, &SAccountPage::OnApiKeyChanged)
        .OnTextCommitted(this, &SAccountPage::OnApiKeyCommitted)
        .IsPassword_Lambda([this]()
                           { return !bIsApiKeyVisible; })
        .OnTogglePassword(FOnClicked::CreateRaw(this, &SAccountPage::OnToggleApiKeyVisibility))
        .HintText(LOCTEXT("ApiKeyHint", "Paste your API key here"))
        .IsEnabled(true);
}

TSharedRef<SWidget> SAccountPage::CreateUsagesBox()
{
    const float BorderRadius = ConvaiEditor::Constants::Layout::Radius::StandardCard;
    const float BorderThickness = ConvaiEditor::Constants::Layout::Components::StandardCard::BorderThickness;
    const FLinearColor BackgroundColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.account.boxBackground"));
    const FLinearColor BorderColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.account.boxBorder"));
    const float PaddingHorizontal = ConvaiEditor::Constants::Layout::Spacing::AccountBox::Horizontal;
    const float PaddingVerticalOuter = ConvaiEditor::Constants::Layout::Spacing::AccountBox::VerticalOuter;
    const float PaddingVerticalInner = ConvaiEditor::Constants::Layout::Spacing::AccountBox::VerticalInner;

    return SNew(SRoundedBox)
        .BorderRadius(BorderRadius)
        .BorderThickness(BorderThickness)
        .BackgroundColor(BackgroundColor)
        .BorderColor(BorderColor)
            [SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(FMargin(PaddingHorizontal, PaddingVerticalOuter, PaddingHorizontal, PaddingVerticalInner))[CreateUsageProgressBar(LOCTEXT("InteractionUsage", "Interaction Usage"), TAttribute<float>::Create(TAttribute<float>::FGetter::CreateSP(this, &SAccountPage::GetInteractionUsagePercent)), TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SAccountPage::GetInteractionUsageText)))] + SVerticalBox::Slot().AutoHeight().Padding(FMargin(PaddingHorizontal, PaddingVerticalInner, PaddingHorizontal, PaddingVerticalInner))[CreateUsageProgressBar(LOCTEXT("ElevenlabsUsage", "Elevenlabs Usage"), TAttribute<float>::Create(TAttribute<float>::FGetter::CreateSP(this, &SAccountPage::GetElevenlabsUsagePercent)), TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SAccountPage::GetElevenlabsUsageText)))] + SVerticalBox::Slot().AutoHeight().Padding(FMargin(PaddingHorizontal, PaddingVerticalInner, PaddingHorizontal, PaddingVerticalInner))[CreateUsageProgressBar(LOCTEXT("CoreApiUsage", "Core API Usage"), TAttribute<float>::Create(TAttribute<float>::FGetter::CreateSP(this, &SAccountPage::GetCoreAPIUsagePercent)), TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SAccountPage::GetCoreAPIUsageText)))] + SVerticalBox::Slot().AutoHeight().Padding(FMargin(PaddingHorizontal, PaddingVerticalInner, PaddingHorizontal, PaddingVerticalOuter))[CreateUsageProgressBar(LOCTEXT("PixelStreamingUsage", "Pixel Streaming Usage"), TAttribute<float>::Create(TAttribute<float>::FGetter::CreateSP(this, &SAccountPage::GetPixelStreamingUsagePercent)), TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SAccountPage::GetPixelStreamingUsageText)))]];
}

TSharedRef<SWidget> SAccountPage::CreateUsageProgressBar(const FText &Label, TAttribute<float> Percent, TAttribute<FText> ValueText)
{
    const FSlateFontInfo LabelFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountLabel"));
    const FSlateFontInfo ValueFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountValue"));
    const float ProgressBarHeight = ConvaiEditor::Constants::Layout::Components::ProgressBar::AccountHeight;
    const float ProgressBarBorderRadius = ConvaiEditor::Constants::Layout::Radius::AccountProgressBar;
    const FLinearColor TextColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.text.primary"));
    const float HorizontalSpacing = ConvaiEditor::Constants::Layout::Spacing::AccountHorizontalSpacing;
    const FLinearColor TrackColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.account.progressTrack"));
    const FLinearColor FillColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.account.progressFill"));

    return SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))[SNew(STextBlock).Text(Label).ColorAndOpacity(TextColor).Font(LabelFont)] + SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1.0f).Padding(FMargin(0.0f, 0.0f, HorizontalSpacing, 0.0f)).VAlign(VAlign_Center)[SNew(SRoundedProgressBar).Percent(Percent).BarHeight(ProgressBarHeight).BorderRadius(ProgressBarBorderRadius).BackgroundColor(TrackColor).FillColor(FillColor)] + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(STextBlock).Text(ValueText).ColorAndOpacity(TextColor).Font(ValueFont)]];
}

FReply SAccountPage::OnToggleApiKeyVisibility()
{
    bIsApiKeyVisible = !bIsApiKeyVisible;

    return FReply::Handled();
}

void SAccountPage::OnApiKeyChanged(const FText &NewText)
{
    ApiKeyValue = NewText.ToString();

    TSharedPtr<IConfigurationService> ConfigServicePtr = ConfigService.Pin();
    if (ConfigServicePtr.IsValid())
    {
        ConfigServicePtr->SetApiKey(ApiKeyValue);
    }

    // Trigger validation for the new API key
    TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
    if (ValidationServicePtr.IsValid())
    {
        ValidationServicePtr->ValidateApiKey(ApiKeyValue);
    }
}

void SAccountPage::OnApiKeyCommitted(const FText &NewText, ETextCommit::Type CommitType)
{
    ApiKeyValue = NewText.ToString();

    TSharedPtr<IConfigurationService> ConfigServicePtr = ConfigService.Pin();
    if (ConfigServicePtr.IsValid())
    {
        ConfigServicePtr->SetApiKey(ApiKeyValue);
    }

    TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
    if (ValidationServicePtr.IsValid())
    {
        ValidationServicePtr->ValidateApiKey(ApiKeyValue, true);
    }
}

FText SAccountPage::GetApiKeyText() const
{
    TSharedPtr<IConfigurationService> ConfigServicePtr = ConfigService.Pin();
    if (ConfigServicePtr.IsValid())
    {
        FString CurrentApiKey = ConfigServicePtr->GetApiKey();
        return FText::FromString(CurrentApiKey);
    }

    return FText::FromString(ApiKeyValue);
}

bool SAccountPage::IsApiKeyValidating() const
{
    TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
    if (ValidationServicePtr.IsValid())
    {
        return ValidationServicePtr->IsValidatingApiKey();
    }
    return false;
}

TOptional<bool> SAccountPage::GetApiKeyValidationResult() const
{
    TSharedPtr<IApiValidationService> ValidationServicePtr = ValidationService.Pin();
    if (ValidationServicePtr.IsValid())
    {
        return ValidationServicePtr->GetLastApiKeyValidationResult(ApiKeyValue);
    }
    return TOptional<bool>();
}

FText SAccountPage::GetPlanName() const
{
    if (AccountViewModel.IsValid())
    {
        return FText::FromString(AccountViewModel->GetUsage().PlanName);
    }
    return FText::FromString(TEXT("-"));
}

FText SAccountPage::GetRenewDate() const
{
    if (AccountViewModel.IsValid())
    {
        return FText::FromString(AccountViewModel->GetUsage().RenewDate);
    }
    return FText::FromString(TEXT("-"));
}

float SAccountPage::GetInteractionUsageCurrent() const
{
    return AccountViewModel.IsValid() ? AccountViewModel->GetUsage().InteractionUsageCurrent : 0.0f;
}

float SAccountPage::GetInteractionUsageLimit() const
{
    return AccountViewModel.IsValid() ? AccountViewModel->GetUsage().InteractionUsageLimit : 1.0f;
}

float SAccountPage::GetElevenlabsUsageCurrent() const
{
    return AccountViewModel.IsValid() ? AccountViewModel->GetUsage().ElevenlabsUsageCurrent : 0.0f;
}

float SAccountPage::GetElevenlabsUsageLimit() const
{
    return AccountViewModel.IsValid() ? AccountViewModel->GetUsage().ElevenlabsUsageLimit : 1.0f;
}

float SAccountPage::GetCoreAPIUsageCurrent() const
{
    return AccountViewModel.IsValid() ? AccountViewModel->GetUsage().CoreAPIUsageCurrent : 0.0f;
}

float SAccountPage::GetCoreAPIUsageLimit() const
{
    return AccountViewModel.IsValid() ? AccountViewModel->GetUsage().CoreAPIUsageLimit : 1.0f;
}

float SAccountPage::GetPixelStreamingUsageCurrent() const
{
    return AccountViewModel.IsValid() ? AccountViewModel->GetUsage().PixelStreamingUsageCurrent : 0.0f;
}

float SAccountPage::GetPixelStreamingUsageLimit() const
{
    return AccountViewModel.IsValid() ? AccountViewModel->GetUsage().PixelStreamingUsageLimit : 1.0f;
}

float SAccountPage::GetInteractionUsagePercent() const
{
    float limit = GetInteractionUsageLimit();
    return (limit > 0.0f) ? FMath::Clamp(GetInteractionUsageCurrent() / limit, 0.0f, 1.0f) : 0.0f;
}

FText SAccountPage::GetInteractionUsageText() const
{
    return FText::Format(FText::FromString(TEXT("{0} / {1}")),
                         FText::AsNumber(FMath::RoundToInt(GetInteractionUsageCurrent())),
                         FText::AsNumber(FMath::RoundToInt(GetInteractionUsageLimit())));
}

float SAccountPage::GetElevenlabsUsagePercent() const
{
    float limit = GetElevenlabsUsageLimit();
    return (limit > 0.0f) ? FMath::Clamp(GetElevenlabsUsageCurrent() / limit, 0.0f, 1.0f) : 0.0f;
}

FText SAccountPage::GetElevenlabsUsageText() const
{
    return FText::Format(FText::FromString(TEXT("{0} / {1}")),
                         FText::AsNumber(FMath::RoundToInt(GetElevenlabsUsageCurrent())),
                         FText::AsNumber(FMath::RoundToInt(GetElevenlabsUsageLimit())));
}

float SAccountPage::GetCoreAPIUsagePercent() const
{
    float limit = GetCoreAPIUsageLimit();
    return (limit > 0.0f) ? FMath::Clamp(GetCoreAPIUsageCurrent() / limit, 0.0f, 1.0f) : 0.0f;
}

FText SAccountPage::GetCoreAPIUsageText() const
{
    return FText::Format(FText::FromString(TEXT("{0} / {1}")),
                         FText::AsNumber(FMath::RoundToInt(GetCoreAPIUsageCurrent())),
                         FText::AsNumber(FMath::RoundToInt(GetCoreAPIUsageLimit())));
}

float SAccountPage::GetPixelStreamingUsagePercent() const
{
    float limit = GetPixelStreamingUsageLimit();
    return (limit > 0.0f) ? FMath::Clamp(GetPixelStreamingUsageCurrent() / limit, 0.0f, 1.0f) : 0.0f;
}

FText SAccountPage::GetPixelStreamingUsageText() const
{
    return FText::Format(FText::FromString(TEXT("{0} / {1}")),
                         FText::AsNumber(FMath::RoundToInt(GetPixelStreamingUsageCurrent())),
                         FText::AsNumber(FMath::RoundToInt(GetPixelStreamingUsageLimit())));
}

FText SAccountPage::GetUserName() const
{
    if (AccountViewModel.IsValid())
    {
        return FText::FromString(AccountViewModel->GetUsage().UserName);
    }
    return FText::FromString(TEXT("-"));
}

#undef LOCTEXT_NAMESPACE
