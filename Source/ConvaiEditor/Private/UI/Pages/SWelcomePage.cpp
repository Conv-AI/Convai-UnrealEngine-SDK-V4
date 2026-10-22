/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SWelcomePage.cpp
 *
 * Implementation of the welcome page.
 */

#include "UI/Pages/SWelcomePage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Images/SImage.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/ConvaiStyleResources.h"
#include "Styling/SlateTypes.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateBrush.h"
#include "UI/Widgets/SConvaiApiKeyInputBox.h"
#include "UI/Widgets/SWelcomeBanner.h"
#include "UI/Utility/ConvaiWidgetFactory.h"
#include "Utility/ConvaiConstants.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/OAuth/IOAuthAuthenticationService.h"
#include "MVVM/ViewModel.h"

#define LOCTEXT_NAMESPACE "SWelcomePage"

void SWelcomePage::Construct(const FArguments &InArgs)
{
    ViewModel = FViewModelRegistry::Get().CreateViewModel<FWelcomePageViewModel>();
    if (ViewModel.IsValid())
    {
        ViewModel->Initialize();
    }

    const float WindowPadding = ConvaiEditor::Constants::Layout::Spacing::Window;
    const float ContentPadding = ConvaiEditor::Constants::Layout::Spacing::Content;
    const float BorderRadius = ConvaiEditor::Constants::Layout::Radius::StandardCard;
    const FLinearColor BackgroundColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.windowBackground"));
    const FLinearColor ContentBackgroundColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.surface.window"));

    auto BgBrushResult = FConvaiStyleResources::Get().GetOrCreateColorBrush(FName("Welcome.BgBrush"), BackgroundColor);
    const FSlateBrush *BgBrush = BgBrushResult.IsSuccess() ? BgBrushResult.GetValue().Get() : FConvaiStyle::GetTransparentBrush();

    auto ContentBgBrushResult = FConvaiStyleResources::Get().GetOrCreateColorBrush(FName("Welcome.ContentBgBrush"), ContentBackgroundColor);
    const FSlateBrush *ContentBgBrush = ContentBgBrushResult.IsSuccess() ? ContentBgBrushResult.GetValue().Get() : FConvaiStyle::GetTransparentBrush();

    ChildSlot
        [SNew(SBorder)
             .BorderImage(BgBrush)
             .Padding(FMargin(0.0f, 0.0f, 0.0f, WindowPadding))
                 [SNew(SBox)
                      .WidthOverride(600.0f)
                      .HeightOverride(700.0f)
                          [SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[SNew(SBox).WidthOverride(600.0f).HeightOverride(350.0f)[SNew(SWelcomeBanner)]] + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.0f, 16.0f, 0.0f, 0.0f))[SNew(SBox).WidthOverride(560.0f)[SNew(SBorder).BorderImage(ContentBgBrush).Padding(ContentPadding)[CreateMainContent()]]]]]];
}

TSharedRef<SWidget> SWelcomePage::CreateMainContent()
{
    return SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility(this, &SWelcomePage::GetWelcomeStepVisibility)[CreateWelcomeStep()]] + SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility(this, &SWelcomePage::GetApiKeyStepVisibility)[CreateApiKeyStep()]];
}

TSharedRef<SWidget> SWelcomePage::CreateWelcomeStep()
{
    FSlateFontInfo TitleFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountSectionTitle"));
    TitleFont.Size = 36;
    FSlateFontInfo BodyFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountValue"));
    BodyFont.Size = 20;
    const FLinearColor TextColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.text.primary"));
    const float Spacing = ConvaiEditor::Constants::Layout::Spacing::AccountSectionSpacing;

    return SNew(SVerticalBox)
           // Title
           + SVerticalBox::Slot()
                 .AutoHeight()
                 .Padding(FMargin(0.0f, 0.0f, 0.0f, Spacing))
                 .HAlign(HAlign_Center)
                     [SNew(STextBlock)
                          .Text(LOCTEXT("WelcomeTitle", "Welcome to Convai"))
                          .Font(TitleFont)
                          .ColorAndOpacity(TextColor)
                          .Justification(ETextJustify::Center)]
           // Description
           + SVerticalBox::Slot()
                 .AutoHeight()
                 .Padding(FMargin(0.0f, 0.0f, 0.0f, Spacing))
                 .HAlign(HAlign_Center)
                     [SNew(STextBlock)
                          .Text(LOCTEXT("WelcomeDescription",
                                        "This powerful plugin enables you to create interactive AI characters and conversations in your Unreal Engine projects."))
                          .Font(BodyFont)
                          .ColorAndOpacity(TextColor)
                          .Justification(ETextJustify::Center)
                          .AutoWrapText(true)]
           // Continue Button
           + SVerticalBox::Slot()
                 .AutoHeight()
                 .Padding(FMargin(0.0f, Spacing, 0.0f, 0.0f))
                 .HAlign(HAlign_Center)
                     [CreateButton(
                         LOCTEXT("ConnectButton", "Connect Convai Account"),
                         FOnClicked::CreateRaw(this, &SWelcomePage::OnConnectClicked),
                         true)];
}

TSharedRef<SWidget> SWelcomePage::CreateApiKeyStep()
{
    const FSlateFontInfo TitleFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountSectionTitle"));
    const FLinearColor TextColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.text.primary"));
    const float Spacing = ConvaiEditor::Constants::Layout::Spacing::AccountSectionSpacing;

    return SNew(SVerticalBox)
           // Title
           + SVerticalBox::Slot()
                 .AutoHeight()
                 .Padding(FMargin(0.0f, 0.0f, 0.0f, Spacing))
                 .HAlign(HAlign_Center)
                     [SNew(STextBlock)
                          .Text(LOCTEXT("ApiKeyTitle", "Enter Your API Key"))
                          .Font(TitleFont)
                          .ColorAndOpacity(TextColor)
                          .Justification(ETextJustify::Center)]
           // Description
           + SVerticalBox::Slot()
                 .AutoHeight()
                 .Padding(FMargin(0.0f, 0.0f, 0.0f, Spacing))
                 .HAlign(HAlign_Center)
                     [SNew(STextBlock)
                          .Text(LOCTEXT("ApiKeyDescription",
                                        "Please enter your Convai API key to enable the plugin's features. "
                                        "You can find your API key in your Convai dashboard."))
                          .Font(FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountValue")))
                          .ColorAndOpacity(TextColor)
                          .Justification(ETextJustify::Center)
                          .AutoWrapText(true)]
           // API Key Input
           + SVerticalBox::Slot()
                 .AutoHeight()
                 .Padding(FMargin(0.0f, Spacing, 0.0f, Spacing))
                     [CreateApiKeyInput()]
           // Error Message
           + SVerticalBox::Slot()
                 .AutoHeight()
                 .Padding(FMargin(0.0f, 0.0f, 0.0f, Spacing))
                     [SNew(SBox)
                          .Visibility(this, &SWelcomePage::GetErrorMessageVisibility)
                              [CreateErrorMessage()]]
           // Buttons
           + SVerticalBox::Slot()
                 .AutoHeight()
                 .Padding(FMargin(0.0f, Spacing, 0.0f, 0.0f))
                 .HAlign(HAlign_Center)
                     [SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, Spacing * 0.5f, 0.0f))[CreateButton(LOCTEXT("CloseButton", "Close"), FOnClicked::CreateRaw(this, &SWelcomePage::OnCloseClicked), false)] + SHorizontalBox::Slot().AutoWidth().Padding(FMargin(Spacing * 0.5f, 0.0f, 0.0f, 0.0f))[CreateButton(LOCTEXT("ValidateButton", "Validate & Continue"), FOnClicked::CreateRaw(this, &SWelcomePage::OnValidateClicked), false)]];
}

TSharedRef<SWidget> SWelcomePage::CreateApiKeyInput()
{
    return SNew(SConvaiApiKeyInputBox)
        .Text(this, &SWelcomePage::GetApiKeyText)
        .OnTextChanged(this, &SWelcomePage::OnApiKeyTextChanged)
        .OnTextCommitted(this, &SWelcomePage::OnApiKeyTextCommitted)
        .IsPassword_Lambda([this]()
                           { return !ViewModel->IsApiKeyVisible.Get(); })
        .OnTogglePassword(FOnClicked::CreateRaw(this, &SWelcomePage::OnToggleApiKeyVisibilityClicked))
        .HintText(LOCTEXT("ApiKeyHint", "Paste your API key here"))
        .IsEnabled(true);
}

TSharedRef<SWidget> SWelcomePage::CreateErrorMessage()
{
    const FLinearColor ErrorBgColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.surface.window"));
    auto ErrorBgBrushResult = FConvaiStyleResources::Get().GetOrCreateColorBrush(FName("Welcome.ErrorBgBrush"), ErrorBgColor);
    const FSlateBrush *ErrorBgBrush = ErrorBgBrushResult.IsSuccess() ? ErrorBgBrushResult.GetValue().Get() : FConvaiStyle::GetTransparentBrush();

    return SNew(SBorder)
        .BorderImage(ErrorBgBrush)
        .Padding(FMargin(8.0f))
            [SNew(STextBlock)
                 .Text(this, &SWelcomePage::GetErrorMessageText)
                 .Font(FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountValue")))
                 .ColorAndOpacity(FConvaiStyle::RequireColor(TEXT("Convai.Color.Error")))
                 .AutoWrapText(true)];
}

TSharedRef<SWidget> SWelcomePage::CreateButton(const FText &Text, const FOnClicked &OnClicked, bool bIsPrimary)
{
    const FString StylePrefix = bIsPrimary ? TEXT("Convai.Button.Primary") : TEXT("Convai.Button.Secondary");

    TAttribute<bool> EnabledAttr = bIsPrimary ? TAttribute<bool>::Create(TAttribute<bool>::FGetter::CreateRaw(this, &SWelcomePage::IsContinueButtonEnabled)) : TAttribute<bool>::Create(TAttribute<bool>::FGetter::CreateRaw(this, &SWelcomePage::IsValidateButtonEnabled));

    FSlateFontInfo BtnFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountValue"));
    BtnFont.Size = 20;

    return SNew(SBox)
        .WidthOverride(340.0f)
        .HeightOverride(45.0f)
            [SNew(SButton)
                 .ButtonStyle(FConvaiStyle::Get(), *StylePrefix)
                 .TextStyle(FConvaiStyle::Get(), *(StylePrefix + TEXT(".Text")))
                 .OnClicked(OnClicked)
                 .IsEnabled(EnabledAttr)
                 .HAlign(HAlign_Center)
                 .VAlign(VAlign_Center)
                     [SNew(STextBlock)
                          .Text(Text)
                          .Font(BtnFont)
                          .ColorAndOpacity(FConvaiStyle::RequireColor(TEXT("Convai.Color.component.button.primary.text")))
                          .Justification(ETextJustify::Center)]];
}

FReply SWelcomePage::OnContinueClicked()
{
    if (ViewModel.IsValid())
    {
        ViewModel->ContinueToApiKey();
    }
    return FReply::Handled();
}

FReply SWelcomePage::OnValidateClicked()
{
    if (ViewModel.IsValid())
    {
        ViewModel->ValidateApiKey();
    }
    return FReply::Handled();
}

void SWelcomePage::OnApiKeyTextChanged(const FText &NewText)
{
    if (ViewModel.IsValid())
    {
        ViewModel->OnApiKeyTextChanged(NewText.ToString());
    }
}

void SWelcomePage::OnApiKeyTextCommitted(const FText &NewText, ETextCommit::Type CommitType)
{
    if (ViewModel.IsValid())
    {
        ViewModel->OnApiKeyTextChanged(NewText.ToString());

        if (CommitType == ETextCommit::OnEnter)
        {
            ViewModel->ValidateApiKey();
        }
    }
}

FReply SWelcomePage::OnToggleApiKeyVisibilityClicked()
{
    if (ViewModel.IsValid())
    {
        ViewModel->ToggleApiKeyVisibility();
    }
    return FReply::Handled();
}

FReply SWelcomePage::OnCloseClicked()
{
    if (ViewModel.IsValid())
    {
        ViewModel->CloseWelcome();
    }
    return FReply::Handled();
}

FReply SWelcomePage::OnConnectClicked()
{
    auto AuthManagerResult = FConvaiDIContainerManager::Get().Resolve<IAuthWindowManager>();
    if (AuthManagerResult.IsSuccess())
    {
        TSharedPtr<IAuthWindowManager> AuthManager = AuthManagerResult.GetValue();
        AuthManager->StartAuthFlow();
    }
    else
    {
        if (!AuthService.IsValid())
        {
            auto Result = FConvaiDIContainerManager::Get().Resolve<IOAuthAuthenticationService>();
            if (Result.IsSuccess())
            {
                AuthService = Result.GetValue();
            }
        }

        if (AuthService.IsValid())
        {
            if (!AuthSuccessHandle.IsValid())
            {
                AuthSuccessHandle = AuthService->OnAuthSuccess().AddSP(SharedThis(this), &SWelcomePage::HandleAuthSuccess);
            }
            if (!AuthFailureHandle.IsValid())
            {
                AuthFailureHandle = AuthService->OnAuthFailure().AddSP(SharedThis(this), &SWelcomePage::HandleAuthFailure);
            }

            AuthService->StartLogin();
        }
        else
        {
            if (ViewModel.IsValid())
            {
                ViewModel->ErrorMessage.Set(TEXT("Authentication service not available"));
            }
        }
    }

    return FReply::Handled();
}

void SWelcomePage::HandleAuthSuccess()
{
    if (ViewModel.IsValid())
    {
        ViewModel->CloseWelcome();
    }
}

void SWelcomePage::HandleAuthFailure(const FString &Error)
{
    if (ViewModel.IsValid())
    {
        ViewModel->ErrorMessage.Set(Error);
    }
}

EVisibility SWelcomePage::GetWelcomeStepVisibility() const
{
    return ViewModel.IsValid() && ViewModel->IsWelcomeStep() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SWelcomePage::GetApiKeyStepVisibility() const
{
    return ViewModel.IsValid() && ViewModel->IsApiKeyStep() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SWelcomePage::GetErrorMessageVisibility() const
{
    return ViewModel.IsValid() && ViewModel->HasError() ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SWelcomePage::GetApiKeyText() const
{
    return ViewModel.IsValid() ? FText::FromString(ViewModel->ApiKeyText.Get()) : FText::GetEmpty();
}

FText SWelcomePage::GetErrorMessageText() const
{
    return ViewModel.IsValid() ? FText::FromString(ViewModel->ErrorMessage.Get()) : FText::GetEmpty();
}

bool SWelcomePage::IsContinueButtonEnabled() const
{
    return ViewModel.IsValid() && ViewModel->CanContinue();
}

bool SWelcomePage::IsValidateButtonEnabled() const
{
    return ViewModel.IsValid() && ViewModel->CanValidate();
}

#undef LOCTEXT_NAMESPACE
