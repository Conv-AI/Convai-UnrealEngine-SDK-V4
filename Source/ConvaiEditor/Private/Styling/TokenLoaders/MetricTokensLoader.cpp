/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * MetricTokensLoader.cpp
 *
 * Implementation of metric token loading from constants.
 */

#include "Styling/TokenLoaders/MetricTokensLoader.h"
#include "Utility/ConvaiConstants.h"
#include "Logging/ConvaiEditorThemeLog.h"

void FMetricTokensLoader::Load(const TSharedPtr<FJsonObject> &Tokens, TSharedPtr<FSlateStyleSet> Style)
{
    if (!Style.IsValid())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("MetricTokensLoader: invalid Style parameter"));
        return;
    }

    LoadSizeTokensFromConstants(Style);
    LoadSpacingTokensFromConstants(Style);
    LoadRadiusTokensFromConstants(Style);
}

void FMetricTokensLoader::LoadSizeTokensFromConstants(TSharedPtr<FSlateStyleSet> Style)
{
    using namespace ConvaiEditor::Constants::Layout;

    Style->Set(FName(TEXT("Convai.Size.window")), Window::DefaultSize);
    Style->Set(FName(TEXT("Convai.Size.logo")), Window::LogoSize);
    Style->Set(FName(TEXT("Convai.Size.homePageSidebarWidth")), Window::HomePageSidebarWidth);

    Style->Set(FName(TEXT("Convai.Size.sampleCard.dimensions")), Components::SampleCard::Dimensions);
    Style->Set(FName(TEXT("Convai.Size.sampleCard.imageHeight")), Components::SampleCard::ImageHeight);
    Style->Set(FName(TEXT("Convai.Size.sampleCard.titleHeight")), Components::SampleCard::TitleHeight);
    Style->Set(FName(TEXT("Convai.Size.sampleCard.tagHeight")), Components::SampleCard::TagHeight);
    Style->Set(FName(TEXT("Convai.Size.sampleCard.gradientHeight")), Components::SampleCard::GradientHeight);
    Style->Set(FName(TEXT("Convai.Size.sampleCard.border")), Components::SampleCard::BorderThickness);

    Style->Set(FName(TEXT("Convai.Size.homePageCard.dimensions")), Components::HomePageCard::Dimensions);
    Style->Set(FName(TEXT("Convai.Size.homePageCard.imageHeight")), Components::HomePageCard::ImageHeight);
    Style->Set(FName(TEXT("Convai.Size.homePageCard.titleHeight")), Components::HomePageCard::TitleHeight);
    Style->Set(FName(TEXT("Convai.Size.homePageCard.contentPadding")), Components::HomePageCard::ContentPadding);
    Style->Set(FName(TEXT("Convai.Size.homePageCard.border")), Components::HomePageCard::BorderThickness);

    Style->Set(FName(TEXT("Convai.Size.standardCard.border")), Components::StandardCard::BorderThickness);

    Style->Set(FName(TEXT("Convai.Size.scrollBarThickness")), Components::ScrollBar::Thickness);

    Style->Set(FName(TEXT("Convai.Size.progressBarHeight")), Components::ProgressBar::Height);
    Style->Set(FName(TEXT("Convai.Size.accountProgressBarHeight")), Components::ProgressBar::AccountHeight);

    Style->Set(FName(TEXT("Convai.Size.windowControl.buttonSize")), Components::WindowControlPanel::ButtonSize);
    Style->Set(FName(TEXT("Convai.Size.windowControl.buttonWidth")), Components::WindowControlPanel::ButtonWidth);
    Style->Set(FName(TEXT("Convai.Size.windowControl.buttonHeight")), Components::WindowControlPanel::ButtonHeight);

    Style->Set(FName(TEXT("Convai.Size.separatorThickness")), Components::Separator::Thickness);
    Style->Set(FName(TEXT("Convai.Radius.separator")), Radius::Separator);

    Style->Set(FName(TEXT("Convai.Size.samplesPageOuterPadding")), Components::SamplesPage::OuterPadding);

    Style->Set(FName(TEXT("Convai.Size.cardCornerSmoothingFactor")), Radius::CardCornerSmoothing);

    Style->Set(FName(TEXT("Convai.Size.icon.home")), Icons::Home);
    Style->Set(FName(TEXT("Convai.Size.icon.settings")), Icons::Settings);
    Style->Set(FName(TEXT("Convai.Size.icon.visibilityToggle")), Icons::VisibilityToggle);
    Style->Set(FName(TEXT("Convai.Size.icon.logo")), Icons::Logo);
    Style->Set(FName(TEXT("Convai.Size.icon.ConvaiLogo")), Icons::ConvaiLogo);
    Style->Set(FName(TEXT("Convai.Size.icon.Gradient_1x256")), Icons::Gradient_1x256);
    Style->Set(FName(TEXT("Convai.Size.icon.Documentation")), Icons::Documentation);
    Style->Set(FName(TEXT("Convai.Size.icon.YoutubeTutorials")), Icons::YoutubeTutorials);
    Style->Set(FName(TEXT("Convai.Size.icon.ConvaiDeveloperForum")), Icons::ConvaiDeveloperForum);
    Style->Set(FName(TEXT("Convai.Size.icon.actions")), Icons::Actions);
    Style->Set(FName(TEXT("Convai.Size.icon.narrativeDesign")), Icons::NarrativeDesign);
    Style->Set(FName(TEXT("Convai.Size.icon.longTermMemory")), Icons::LongTermMemory);
    Style->Set(FName(TEXT("Convai.Size.icon.openExternally")), Icons::OpenExternally);
    Style->Set(FName(TEXT("Convai.Size.icon.toggle")), Icons::Toggle);
    Style->Set(FName(TEXT("Convai.Size.icon.ExternalLink")), Icons::ExternalLink);
    Style->Set(FName(TEXT("Convai.Size.icon.SignOut")), Icons::SignOut);
    Style->Set(FName(TEXT("Convai.Size.icon.WelcomeBanner")), Icons::WelcomeBanner);
    Style->Set(FName(TEXT("Convai.Size.icon.16")), Icons::Icon16);
    Style->Set(FName(TEXT("Convai.Size.icon.40")), Icons::Icon40);
}

void FMetricTokensLoader::LoadSpacingTokensFromConstants(TSharedPtr<FSlateStyleSet> Style)
{
    using namespace ConvaiEditor::Constants::Layout::Spacing;

    Style->Set(FName(TEXT("Convai.Spacing.nav")), Nav);
    Style->Set(FName(TEXT("Convai.Spacing.dropdownY")), DropdownY);
    Style->Set(FName(TEXT("Convai.Spacing.window")), Window);
    Style->Set(FName(TEXT("Convai.Spacing.content")), Content);
    Style->Set(FName(TEXT("Convai.Spacing.sampleCardPadding")), SampleCardPadding);
    Style->Set(FName(TEXT("Convai.Spacing.sampleCardSpacing")), SampleCardSpacing);
    Style->Set(FName(TEXT("Convai.Spacing.homePageCardSpacing")), HomePageCardSpacing);
    Style->Set(FName(TEXT("Convai.Spacing.homePageSidebarSpacing")), HomePageSidebarSpacing);
    Style->Set(FName(TEXT("Convai.Spacing.scrollBarVerticalPadding")), ConvaiEditor::Constants::Layout::Components::ScrollBar::VerticalPadding);
    Style->Set(FName(TEXT("Convai.Spacing.accountSection")), AccountSection);
    Style->Set(FName(TEXT("Convai.Spacing.spaceBelowTitle")), SpaceBelowTitle);
    Style->Set(FName(TEXT("Convai.Spacing.accountHorizontal")), AccountHorizontal);
    Style->Set(FName(TEXT("Convai.Spacing.headerPaddingTop")), HeaderPaddingTop);
    Style->Set(FName(TEXT("Convai.Spacing.headerPaddingBottom")), HeaderPaddingBottom);
    Style->Set(FName(TEXT("Convai.Spacing.paddingWindow")), PaddingWindow);
    Style->Set(FName(TEXT("Convai.Spacing.paddingContent")), PaddingContent);
    Style->Set(FName(TEXT("Convai.Spacing.accountSectionSpacing")), AccountSectionSpacing);
    Style->Set(FName(TEXT("Convai.Spacing.accountHorizontalSpacing")), AccountHorizontalSpacing);
    Style->Set(FName(TEXT("Convai.Spacing.accountBoxPaddingHorizontal")), AccountBox::Horizontal);
    Style->Set(FName(TEXT("Convai.Spacing.accountBoxPaddingVerticalOuter")), AccountBox::VerticalOuter);
    Style->Set(FName(TEXT("Convai.Spacing.accountBoxPaddingVerticalInner")), AccountBox::VerticalInner);
    Style->Set(FName(TEXT("Convai.Spacing.apiKeyIconUniformPadding")), ApiKeyIconUniformPadding);

    Style->Set(FName(TEXT("Convai.Spacing.button.padding")), Button::Padding);

    Style->Set(FName(TEXT("Convai.Spacing.windowControl.settingsButtonPaddingHorizontal")), ConvaiEditor::Constants::Layout::Components::WindowControlPanel::SettingsButtonPaddingHorizontal);
    Style->Set(FName(TEXT("Convai.Spacing.windowControl.settingsButtonPaddingVertical")), ConvaiEditor::Constants::Layout::Components::WindowControlPanel::SettingsButtonPaddingVertical);
    Style->Set(FName(TEXT("Convai.Spacing.windowControl.controlButtonPaddingHorizontal")), ConvaiEditor::Constants::Layout::Components::WindowControlPanel::ControlButtonPaddingHorizontal);
    Style->Set(FName(TEXT("Convai.Spacing.windowControl.controlButtonPaddingVertical")), ConvaiEditor::Constants::Layout::Components::WindowControlPanel::ControlButtonPaddingVertical);
    Style->Set(FName(TEXT("Convai.Spacing.windowControl.iconSpacing")), ConvaiEditor::Constants::Layout::Components::WindowControlPanel::IconSpacing);
    Style->Set(FName(TEXT("Convai.Spacing.windowControl.dividerSideMargin")), ConvaiEditor::Constants::Layout::Components::WindowControlPanel::DividerSideMargin);
    Style->Set(FName(TEXT("Convai.Spacing.windowControl.dividerVerticalMargin")), ConvaiEditor::Constants::Layout::Components::WindowControlPanel::DividerVerticalMargin);

    Style->Set(FName(TEXT("Convai.Spacing.accountBoxPadding.horizontal")), AccountBox::Horizontal);
    Style->Set(FName(TEXT("Convai.Spacing.accountBoxPadding.verticalOuter")), AccountBox::VerticalOuter);
    Style->Set(FName(TEXT("Convai.Spacing.accountBoxPadding.verticalInner")), AccountBox::VerticalInner);
    Style->Set(FName(TEXT("Convai.Spacing.apiKeyIconUniform")), ApiKeyIconUniformPadding);
}

void FMetricTokensLoader::LoadRadiusTokensFromConstants(TSharedPtr<FSlateStyleSet> Style)
{
    using namespace ConvaiEditor::Constants::Layout::Radius;

    Style->Set(FName(TEXT("Convai.Radius.contentContainer")), ContentContainer);
    Style->Set(FName(TEXT("Convai.Radius.dropdown")), Dropdown);
    Style->Set(FName(TEXT("Convai.Radius.separator")), Separator);
    Style->Set(FName(TEXT("Convai.Radius.sampleCard")), SampleCard);
    Style->Set(FName(TEXT("Convai.Radius.sampleCardMask")), SampleCardMask);
    Style->Set(FName(TEXT("Convai.Radius.homePageCard")), HomePageCard);
    Style->Set(FName(TEXT("Convai.Radius.sampleCardTag")), SampleCardTag);
    Style->Set(FName(TEXT("Convai.Radius.standardCard")), StandardCard);
    Style->Set(FName(TEXT("Convai.Radius.accountProgressBar")), AccountProgressBar);
    Style->Set(FName(TEXT("Convai.Radius.cardCornerSmoothing")), CardCornerSmoothing);
}
