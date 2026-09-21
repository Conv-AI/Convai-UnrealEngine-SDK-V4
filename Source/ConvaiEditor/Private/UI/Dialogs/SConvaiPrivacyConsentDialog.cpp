/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiPrivacyConsentDialog.cpp
 *
 * Implementation of the privacy consent dialog.
 */

#include "UI/Dialogs/SConvaiPrivacyConsentDialog.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "EditorStyleSet.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateTypes.h"
#include "Styling/ConvaiStyle.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Utility/ConvaiConstants.h"
#include "UI/Shell/SDraggableBackground.h"

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
#include "Styling/AppStyle.h"
#define CONVAI_EDITOR_STYLE FAppStyle
#else
#define CONVAI_EDITOR_STYLE FEditorStyle
#endif

#define LOCTEXT_NAMESPACE "ConvaiPrivacyConsent"

void SConvaiPrivacyConsentDialog::Construct(const FArguments &InArgs)
{
}

bool SConvaiPrivacyConsentDialog::ShowConsentDialog(bool bIsForExport)
{
    bool bUserAccepted = false;

    FString TitleText = bIsForExport ? TEXT("Export Logs - Privacy Notice") : TEXT("Contact Support - Privacy Notice");

    TSharedRef<SWindow> Window = SNew(SWindow)
                                     .Title(FText::FromString(TitleText))
                                     .ClientSize(FVector2D(750, 720))
                                     .SupportsMaximize(false)
                                     .SupportsMinimize(false)
                                     .SizingRule(ESizingRule::FixedSize)
                                     .IsTopmostWindow(true)
                                     .UseOSWindowBorder(false)
                                     .CreateTitleBar(false)
                                     .HasCloseButton(true);

    static FSlateRoundedBoxBrush WindowBorderBrush = FSlateRoundedBoxBrush(
        FConvaiStyle::RequireColor("Convai.Color.component.dialog.windowBg"),
        12.0f,
        FConvaiStyle::RequireColor("Convai.Color.component.dialog.borderAccent"),
        2.0f);

    FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle("Bold", 20);
    FSlateFontInfo BodyFont = FCoreStyle::GetDefaultFontStyle("Regular", 13);
    FSlateFontInfo ButtonFont = FCoreStyle::GetDefaultFontStyle("Bold", 14);

    using namespace ConvaiEditor::Constants::Layout::Icons;
    const FVector2D WindowControlIconSize = Minimize;

    TSharedPtr<SButton> CloseButton;

    static FButtonStyle AcceptButtonStyle;
    static FButtonStyle DeclineButtonStyle;
    static bool bStylesInitialized = false;

    if (!bStylesInitialized)
    {
        FLinearColor AccentGreen = FConvaiStyle::RequireColor("Convai.Color.component.dialog.accentGreen");
        FLinearColor AccentGreenBright = FConvaiStyle::RequireColor("Convai.Color.component.dialog.accentGreenBright");
        FLinearColor ButtonSecondary = FConvaiStyle::RequireColor("Convai.Color.component.dialog.buttonSecondary");

        AcceptButtonStyle = FButtonStyle()
                                .SetNormal(FSlateRoundedBoxBrush(AccentGreen, 8.0f))
                                .SetHovered(FSlateRoundedBoxBrush(AccentGreenBright, 8.0f))
                                .SetPressed(FSlateRoundedBoxBrush(AccentGreen.Desaturate(0.2f), 8.0f))
                                .SetNormalPadding(FMargin(16, 10))
                                .SetPressedPadding(FMargin(16, 10));

        DeclineButtonStyle = FButtonStyle()
                                 .SetNormal(FSlateRoundedBoxBrush(ButtonSecondary, 8.0f))
                                 .SetHovered(FSlateRoundedBoxBrush(ButtonSecondary * 1.2f, 8.0f))
                                 .SetPressed(FSlateRoundedBoxBrush(ButtonSecondary * 0.8f, 8.0f))
                                 .SetNormalPadding(FMargin(16, 10))
                                 .SetPressedPadding(FMargin(16, 10));

        bStylesInitialized = true;
    }

    Window->SetContent(
        SNew(SDraggableBackground)
            .ParentWindow(Window)
                [SNew(SBorder)
                     .BorderImage(&WindowBorderBrush)
                     .Padding(24.0f)
                         [SNew(SVerticalBox)

                          + SVerticalBox::Slot()
                                .AutoHeight()
                                .Padding(0, 0, 0, 12)
                                    [SNew(SHorizontalBox)

                                     + SHorizontalBox::Slot()
                                           .FillWidth(1.0f)
                                           .VAlign(VAlign_Center)
                                               [SNew(STextBlock)
                                                    .Text(LOCTEXT("PrivacyTitle", "Data Collection Notice"))
                                                    .Font(TitleFont)
                                                    .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))
                                                    .Justification(ETextJustify::Center)]

                                     + SHorizontalBox::Slot()
                                           .AutoWidth()
                                           .VAlign(VAlign_Top)
                                           .Padding(FMargin(4, 0, 0, 0))
                                               [SNew(SBorder)
                                                    .BorderImage_Lambda([&CloseButton]()
                                                                        {
        const ISlateStyle &ST = FConvaiStyle::Get();
        if (CloseButton.IsValid() && CloseButton->IsPressed())
            return ST.GetBrush("Convai.ColorBrush.windowControl.close.background.active");
        if (CloseButton.IsValid() && CloseButton->IsHovered())
            return ST.GetBrush("Convai.ColorBrush.windowControl.close.background.hover");
        return ST.GetBrush("Convai.ColorBrush.windowControl.close.background.normal"); })
                                                    .Padding(0)
                                                        [SAssignNew(CloseButton, SButton)
                                                             .ButtonStyle(CONVAI_EDITOR_STYLE::Get(), "NoBorder")
                                                             .ContentPadding(FMargin(0))
                                                             .ToolTipText(LOCTEXT("CloseTooltip", "Close"))
                                                             .OnClicked_Lambda([&Window, &bUserAccepted]() -> FReply
                                                                               {
        bUserAccepted = false;
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().RequestDestroyWindow(Window);
        }
        return FReply::Handled(); })
                                                             .HAlign(HAlign_Fill)
                                                             .VAlign(VAlign_Fill)
                                                                 [SNew(SBox)
                                                                      .HAlign(HAlign_Center)
                                                                      .VAlign(VAlign_Center)
                                                                          [SNew(SImage)
                                                                               .Image(FConvaiStyle::Get().GetBrush("Convai.Icon.Close"))
                                                                               .DesiredSizeOverride(WindowControlIconSize)
                                                                               .ColorAndOpacity(FConvaiStyle::Get().GetColor("Convai.Color.windowControl.close.normal"))]]]]]

                          + SVerticalBox::Slot()
                                .AutoHeight()
                                .Padding(0, 0, 0, 16)
                                    [SNew(STextBlock)
                                         .Text(LOCTEXT("PrivacyDescription",
                                                       "You are about to export diagnostic information to help resolve technical issues.\n"
                                                       "Below is a summary of the data that will be collected:"))
                                         .Font(BodyFont)
                                         .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textSecondary"))
                                         .AutoWrapText(true)]

                          + SVerticalBox::Slot()
                                .AutoHeight()
                                .Padding(0, 0, 0, 16)
                                    [SNew(SBorder)
                                         .BorderImage(new FSlateRoundedBoxBrush(FConvaiStyle::RequireColor("Convai.Color.component.dialog.surfaceBg"), 10.0f))
                                         .Padding(16.0f)
                                             [SNew(STextBlock)
                                                  .Text(LOCTEXT("DataDetails",
                                                                "System Information:\n"
                                                                "• Operating System (OS)\n"
                                                                "• CPU, GPU, RAM specifications\n"
                                                                "• Screen resolution and display settings\n"
                                                                "• Locale and language settings\n\n"
                                                                "Project Information:\n"
                                                                "• Unreal Engine version\n"
                                                                "• Convai Plugin version and settings\n"
                                                                "• Project name and configuration\n"
                                                                "• Installed plugins list\n\n"
                                                                "Log Files (Last 24 Hours):\n"
                                                                "• Convai plugin logs\n"
                                                                "• Unreal Engine logs\n"
                                                                "• Crash reports (if any)\n"
                                                                "• Plugin configuration files\n\n"
                                                                "Performance & Network:\n"
                                                                "• FPS and memory usage statistics\n"
                                                                "• Network adapter info\n"
                                                                "• Session uptime"))
                                                  .Font(BodyFont)
                                                  .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))
                                                  .AutoWrapText(true)]]

                          + SVerticalBox::Slot()
                                .AutoHeight()
                                .Padding(0, 0, 0, 20)
                                    [SNew(STextBlock)
                                         .Text(LOCTEXT("ConsentQuestion", "By clicking 'Accept', you consent to the collection of this diagnostic data."))
                                         .Font(BodyFont)
                                         .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textSecondary"))
                                         .AutoWrapText(true)
                                         .Justification(ETextJustify::Center)]

                          + SVerticalBox::Slot()
                                .AutoHeight()
                                    [SNew(SHorizontalBox)

                                     + SHorizontalBox::Slot()
                                           .FillWidth(1.0f)
                                           .Padding(0, 0, 8, 0)
                                               [SNew(SButton)
                                                    .ButtonStyle(&DeclineButtonStyle)
                                                    .HAlign(HAlign_Center)
                                                    .VAlign(VAlign_Center)
                                                    .OnClicked_Lambda([&Window, &bUserAccepted]() -> FReply
                                                                      {
        bUserAccepted = false;
        FSlateApplication::Get().RequestDestroyWindow(Window);
        return FReply::Handled(); })
                                                        [SNew(STextBlock)
                                                             .Text(LOCTEXT("Decline", "Decline"))
                                                             .Font(ButtonFont)
                                                             .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))
                                                             .Justification(ETextJustify::Center)]]

                                     + SHorizontalBox::Slot()
                                           .FillWidth(1.0f)
                                           .Padding(8, 0, 0, 0)
                                               [SNew(SButton)
                                                    .ButtonStyle(&AcceptButtonStyle)
                                                    .HAlign(HAlign_Center)
                                                    .VAlign(VAlign_Center)
                                                    .OnClicked_Lambda([&Window, &bUserAccepted]() -> FReply
                                                                      {
        bUserAccepted = true;
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().RequestDestroyWindow(Window);
        }
        return FReply::Handled(); })
                                                        [SNew(STextBlock)
                                                             .Text(LOCTEXT("Accept", "Accept"))
                                                             .Font(ButtonFont)
                                                             .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.windowBg"))
                                                             .Justification(ETextJustify::Center)]]]]]);

    FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());

    return bUserAccepted;
}

#undef LOCTEXT_NAMESPACE
