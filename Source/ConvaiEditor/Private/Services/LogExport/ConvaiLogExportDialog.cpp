/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiLogExportDialog.cpp
 *
 * Implementation of log export dialog UI.
 */

#include "Services/LogExport/ConvaiLogExportDialog.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "EditorStyleSet.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
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

#define LOCTEXT_NAMESPACE "ConvaiLogExportDialog"

// FConvaiIssueReport implementation
TSharedPtr<FJsonObject> FConvaiIssueReport::ToJson() const
{
    TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

    JsonObject->SetStringField(TEXT("Description"), Description);
    JsonObject->SetStringField(TEXT("Category"), CategoryToString(Category));
    JsonObject->SetStringField(TEXT("Severity"), SeverityToString(Severity));
    JsonObject->SetStringField(TEXT("TimeStarted"), TimeStarted);
    JsonObject->SetBoolField(TEXT("IsReproducible"), bIsReproducible);
    JsonObject->SetStringField(TEXT("ReproductionSteps"), ReproductionSteps);
    JsonObject->SetStringField(TEXT("ReportedAt"), FDateTime::UtcNow().ToIso8601());

    return JsonObject;
}

FString FConvaiIssueReport::CategoryToString(EConvaiIssueCategory Category)
{
    switch (Category)
    {
    case EConvaiIssueCategory::ConnectionIssue:
        return TEXT("Connection Issue");
    case EConvaiIssueCategory::CrashOnStartup:
        return TEXT("Crash on Startup");
    case EConvaiIssueCategory::AudioVoiceIssue:
        return TEXT("Audio/Voice Issue");
    case EConvaiIssueCategory::CharacterNotResponding:
        return TEXT("Character Not Responding");
    case EConvaiIssueCategory::SettingsConfig:
        return TEXT("Settings/Configuration");
    case EConvaiIssueCategory::OtherBug:
        return TEXT("Other Bug");
    case EConvaiIssueCategory::FeatureRequest:
        return TEXT("Feature Request");
    default:
        return TEXT("Not Specified");
    }
}

FString FConvaiIssueReport::SeverityToString(EConvaiIssueSeverity Severity)
{
    switch (Severity)
    {
    case EConvaiIssueSeverity::Critical:
        return TEXT("Critical");
    case EConvaiIssueSeverity::High:
        return TEXT("High");
    case EConvaiIssueSeverity::Medium:
        return TEXT("Medium");
    case EConvaiIssueSeverity::Low:
        return TEXT("Low");
    default:
        return TEXT("Medium");
    }
}

// SConvaiLogExportDialog implementation
void SConvaiLogExportDialog::Construct(const FArguments &InArgs)
{
    InitializeOptions();

    ChildSlot
        [BuildDialogContent()];
}

void SConvaiLogExportDialog::InitializeOptions()
{
    CategoryOptions.Add(MakeShared<FString>(TEXT("Can't connect to Convai servers")));
    CategoryOptions.Add(MakeShared<FString>(TEXT("Plugin crashes on startup")));
    CategoryOptions.Add(MakeShared<FString>(TEXT("Audio/Voice issues")));
    CategoryOptions.Add(MakeShared<FString>(TEXT("Character not responding")));
    CategoryOptions.Add(MakeShared<FString>(TEXT("Settings/Configuration problem")));
    CategoryOptions.Add(MakeShared<FString>(TEXT("Other bug")));
    CategoryOptions.Add(MakeShared<FString>(TEXT("Feature request")));

    SeverityOptions.Add(MakeShared<FString>(TEXT("Critical - Can't use plugin")));
    SeverityOptions.Add(MakeShared<FString>(TEXT("High - Major functionality broken")));
    SeverityOptions.Add(MakeShared<FString>(TEXT("Medium - Some features don't work")));
    SeverityOptions.Add(MakeShared<FString>(TEXT("Low - Minor issue")));

    TimeStartedOptions.Add(MakeShared<FString>(TEXT("Just now")));
    TimeStartedOptions.Add(MakeShared<FString>(TEXT("Today")));
    TimeStartedOptions.Add(MakeShared<FString>(TEXT("Yesterday")));
    TimeStartedOptions.Add(MakeShared<FString>(TEXT("This week")));
    TimeStartedOptions.Add(MakeShared<FString>(TEXT("Longer ago")));
}

TSharedRef<SWidget> SConvaiLogExportDialog::BuildDialogContent()
{
    FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle("Bold", 20);
    FSlateFontInfo LabelFont = FCoreStyle::GetDefaultFontStyle("Bold", 12);
    FSlateFontInfo BodyFont = FCoreStyle::GetDefaultFontStyle("Regular", 12);
    FSlateFontInfo ButtonFont = FCoreStyle::GetDefaultFontStyle("Bold", 14);

    using namespace ConvaiEditor::Constants::Layout::Icons;
    const FVector2D WindowControlIconSize = Minimize;
    static FEditableTextBoxStyle InputTextBoxStyle;
    static FComboBoxStyle ComboBoxStyle;
    static FButtonStyle SubmitButtonStyle;
    static FButtonStyle CancelButtonStyle;
    static bool bStylesInitialized = false;

    if (!bStylesInitialized)
    {
        FLinearColor InputBg = FConvaiStyle::RequireColor("Convai.Color.component.dialog.inputBg");
        FLinearColor AccentGreen = FConvaiStyle::RequireColor("Convai.Color.component.dialog.accentGreen");
        FLinearColor AccentGreenBright = FConvaiStyle::RequireColor("Convai.Color.component.dialog.accentGreenBright");
        FLinearColor TextPrimary = FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary");
        FLinearColor TextHint = FConvaiStyle::RequireColor("Convai.Color.component.dialog.textHint");
        FLinearColor SurfaceBg = FConvaiStyle::RequireColor("Convai.Color.component.dialog.surfaceBg");
        FLinearColor ButtonSecondary = FConvaiStyle::RequireColor("Convai.Color.component.dialog.buttonSecondary");
        InputTextBoxStyle = FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
        InputTextBoxStyle
            .SetBackgroundImageNormal(FSlateRoundedBoxBrush(InputBg, 8.0f, AccentGreen, 1.0f))
            .SetBackgroundImageHovered(FSlateRoundedBoxBrush(InputBg, 8.0f, AccentGreenBright, 1.0f))
            .SetBackgroundImageFocused(FSlateRoundedBoxBrush(InputBg, 8.0f, AccentGreenBright, 2.0f))
            .SetBackgroundImageReadOnly(FSlateRoundedBoxBrush(InputBg, 8.0f, TextHint, 1.0f))
            .SetForegroundColor(TextPrimary)
            .SetPadding(FMargin(12, 10));

        ComboBoxStyle = FCoreStyle::Get().GetWidgetStyle<FComboBoxStyle>("ComboBox");
        ComboBoxStyle
            .SetComboButtonStyle(FComboButtonStyle()
                                    .SetButtonStyle(FButtonStyle()
                                                        .SetNormal(FSlateRoundedBoxBrush(InputBg, 8.0f, AccentGreen, 1.0f))
                                                        .SetHovered(FSlateRoundedBoxBrush(InputBg, 8.0f, AccentGreenBright, 1.0f))
                                                        .SetPressed(FSlateRoundedBoxBrush(InputBg, 8.0f, AccentGreenBright, 2.0f))
                                                        .SetNormalPadding(FMargin(12, 10))
                                                        .SetPressedPadding(FMargin(12, 10)))
                                    .SetDownArrowImage(*CONVAI_EDITOR_STYLE::GetBrush("ComboButton.Arrow"))
                                     .SetMenuBorderBrush(FSlateRoundedBoxBrush(SurfaceBg, 8.0f, AccentGreen, 2.0f))
                                     .SetMenuBorderPadding(FMargin(2)));

        SubmitButtonStyle = FButtonStyle()
                                .SetNormal(FSlateRoundedBoxBrush(AccentGreen, 8.0f))
                                .SetHovered(FSlateRoundedBoxBrush(AccentGreenBright, 8.0f))
                                .SetPressed(FSlateRoundedBoxBrush(AccentGreen.Desaturate(0.2f), 8.0f))
                                .SetNormalPadding(FMargin(24, 12))
                                .SetPressedPadding(FMargin(24, 12));

        CancelButtonStyle = FButtonStyle()
                                .SetNormal(FSlateRoundedBoxBrush(ButtonSecondary, 8.0f))
                                .SetHovered(FSlateRoundedBoxBrush(ButtonSecondary * 1.2f, 8.0f))
                                .SetPressed(FSlateRoundedBoxBrush(ButtonSecondary * 0.8f, 8.0f))
                                .SetNormalPadding(FMargin(24, 12))
                                .SetPressedPadding(FMargin(24, 12));

        bStylesInitialized = true;
    }

    FLinearColor WindowBg = FConvaiStyle::RequireColor("Convai.Color.component.dialog.windowBg");
    FLinearColor BorderAccent = FConvaiStyle::RequireColor("Convai.Color.component.dialog.borderAccent");
    static FSlateRoundedBoxBrush WindowBorderBrush = FSlateRoundedBoxBrush(WindowBg, 12.0f, BorderAccent, 2.0f);

    return SNew(SBorder)
        .BorderImage(&WindowBorderBrush)
        .Padding(24.0f)
            [SNew(SBox)
                 .MinDesiredWidth(650.0f)
                 .MaxDesiredHeight(750.0f)
                     [SNew(SVerticalBox)

                      + SVerticalBox::Slot()
                            .AutoHeight()
                            .Padding(0, 0, 0, 16)
                                [SNew(SHorizontalBox)

                                 + SHorizontalBox::Slot()
                                       .FillWidth(1.0f)
                                       .VAlign(VAlign_Center)
                                           [SNew(STextBlock)
                                                .Text(LOCTEXT("DialogTitle", "Contact Convai Support"))
                                                .Font(TitleFont)
                                                .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))
                                                .Justification(ETextJustify::Center)] +
                                 SHorizontalBox::Slot()
                                     .AutoWidth()
                                     .VAlign(VAlign_Top)
                                     .Padding(FMargin(4, 0, 0, 0))
                                         [SNew(SBorder)
                                              .BorderImage_Lambda([this]()
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
                                                       .OnClicked(this, &SConvaiLogExportDialog::OnCancelClicked)
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
                                [SNew(SVerticalBox)

                                 + SVerticalBox::Slot()
                                       .AutoHeight()
                                       .Padding(0, 0, 0, 16)
                                           [SNew(SVerticalBox)

                                            + SVerticalBox::Slot()
                                                  .AutoHeight()
                                                      [SNew(STextBlock)
                                                           .Text(LOCTEXT("CategoryLabel", "What type of issue are you reporting?"))
                                                           .Font(LabelFont)
                                                           .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))]

                                            + SVerticalBox::Slot()
                                                  .AutoHeight()
                                                  .Padding(0, 6, 0, 0)
                                                      [SAssignNew(CategoryComboBox, SComboBox<TSharedPtr<FString>>)
                                                           .ComboBoxStyle(&ComboBoxStyle)
                                                           .OptionsSource(&CategoryOptions)
                                                           .OnGenerateWidget(this, &SConvaiLogExportDialog::OnGenerateCategoryWidget)
                                                           .OnSelectionChanged(this, &SConvaiLogExportDialog::OnCategorySelectionChanged)
                                                               [SNew(STextBlock)
                                                                    .Text(this, &SConvaiLogExportDialog::GetSelectedCategoryText)
                                                                    .Font(BodyFont)
                                                                    .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))]]] +
                                 SVerticalBox::Slot()
                                     .AutoHeight()
                                     .Padding(0, 0, 0, 16)
                                         [SNew(SVerticalBox)

                                          + SVerticalBox::Slot()
                                                .AutoHeight()
                                                    [SNew(STextBlock)
                                                         .Text(LOCTEXT("SeverityLabel", "How severe is this issue?"))
                                                         .Font(LabelFont)
                                                         .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))]

                                          + SVerticalBox::Slot()
                                                .AutoHeight()
                                                .Padding(0, 6, 0, 0)
                                                    [SAssignNew(SeverityComboBox, SComboBox<TSharedPtr<FString>>)
                                                         .ComboBoxStyle(&ComboBoxStyle)
                                                         .OptionsSource(&SeverityOptions)
                                                         .OnGenerateWidget(this, &SConvaiLogExportDialog::OnGenerateSeverityWidget)
                                                         .OnSelectionChanged(this, &SConvaiLogExportDialog::OnSeveritySelectionChanged)
                                                             [SNew(STextBlock)
                                                                  .Text(this, &SConvaiLogExportDialog::GetSelectedSeverityText)
                                                                  .Font(BodyFont)
                                                                  .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))]]]

                                 // When started
                                 + SVerticalBox::Slot()
                                       .AutoHeight()
                                       .Padding(0, 0, 0, 16)
                                           [SNew(SVerticalBox)

                                            + SVerticalBox::Slot()
                                                  .AutoHeight()
                                                      [SNew(STextBlock)
                                                           .Text(LOCTEXT("TimeStartedLabel", "When did this issue start?"))
                                                           .Font(LabelFont)
                                                           .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))]

                                            + SVerticalBox::Slot()
                                                  .AutoHeight()
                                                  .Padding(0, 6, 0, 0)
                                                      [SAssignNew(TimeStartedComboBox, SComboBox<TSharedPtr<FString>>)
                                                           .ComboBoxStyle(&ComboBoxStyle)
                                                           .OptionsSource(&TimeStartedOptions)
                                                           .OnGenerateWidget(this, &SConvaiLogExportDialog::OnGenerateTimeStartedWidget)
                                                           .OnSelectionChanged(this, &SConvaiLogExportDialog::OnTimeStartedSelectionChanged)
                                                               [SNew(STextBlock)
                                                                    .Text(this, &SConvaiLogExportDialog::GetSelectedTimeStartedText)
                                                                    .Font(BodyFont)
                                                                    .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))]]]

                                 // Description
                                 + SVerticalBox::Slot()
                                       .AutoHeight()
                                       .Padding(0, 0, 0, 16)
                                           [SNew(SVerticalBox)

                                            + SVerticalBox::Slot()
                                                  .AutoHeight()
                                                      [SNew(STextBlock)
                                                           .Text(LOCTEXT("DescriptionLabel", "Please describe the issue in detail:"))
                                                           .Font(LabelFont)
                                                           .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))]

                                            + SVerticalBox::Slot()
                                                  .AutoHeight()
                                                  .Padding(0, 6, 0, 0)
                                                      [SNew(SBox)
                                                           .HeightOverride(120.0f)
                                                               [SAssignNew(DescriptionTextBox, SMultiLineEditableTextBox)
                                                                    .Style(&InputTextBoxStyle)
                                                                    .HintText(LOCTEXT("DescriptionHint", "Example: Character voice cuts off after 5 seconds. Started happening today after updating to UE 5.3.2..."))
                                                                    .Font(BodyFont)
                                                                    .ForegroundColor(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))
                                                                    .AutoWrapText(true)
                                                                    .AllowMultiLine(true)]]]

                                 // Is Reproducible
                                 + SVerticalBox::Slot()
                                       .AutoHeight()
                                      .Padding(0, 0, 0, 16)
                                          [SAssignNew(ReproducibleCheckBox, SCheckBox)
                                               .Style(CONVAI_EDITOR_STYLE::Get(), "Checkbox")
                                                .Content()
                                                    [SNew(STextBlock)
                                                         .Text(LOCTEXT("ReproducibleLabel", "I can reproduce this issue consistently"))
                                                         .Font(BodyFont)
                                                         .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))]]

                                 // Reproduction Steps
                                 + SVerticalBox::Slot()
                                       .AutoHeight()
                                       .Padding(0, 0, 0, 16)
                                           [SNew(SVerticalBox)

                                            + SVerticalBox::Slot()
                                                  .AutoHeight()
                                                      [SNew(STextBlock)
                                                           .Text(LOCTEXT("ReproStepsLabel", "Steps to reproduce:"))
                                                           .Font(LabelFont)
                                                           .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))]

                                            + SVerticalBox::Slot()
                                                  .AutoHeight()
                                                  .Padding(0, 6, 0, 0)
                                                      [SNew(SBox)
                                                           .HeightOverride(100.0f)
                                                               [SAssignNew(ReproStepsTextBox, SMultiLineEditableTextBox)
                                                                    .Style(&InputTextBoxStyle)
                                                                    .HintText(LOCTEXT("ReproStepsHint", "1. Open Convai Editor\n2. Select character X\n3. Click Test Voice\n4. Issue occurs..."))
                                                                    .Font(BodyFont)
                                                                    .ForegroundColor(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))
                                                                    .AutoWrapText(true)
                                                                    .AllowMultiLine(true)]]]

                                 // Buttons
                                 + SVerticalBox::Slot()
                                       .AutoHeight()
                                       .Padding(0, 20, 0, 0)
                                           [SNew(SHorizontalBox)

                                            + SHorizontalBox::Slot()
                                                  .FillWidth(1.0f)
                                                  .Padding(0, 0, 8, 0)
                                                      [SNew(SButton)
                                                           .ButtonStyle(&CancelButtonStyle)
                                                           .HAlign(HAlign_Center)
                                                           .VAlign(VAlign_Center)
                                                           .OnClicked(this, &SConvaiLogExportDialog::OnCancelClicked)
                                                               [SNew(STextBlock)
                                                                    .Text(LOCTEXT("CancelButton", "Cancel"))
                                                                    .Font(ButtonFont)
                                                                    .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))]]

                                            + SHorizontalBox::Slot()
                                                  .FillWidth(1.0f)
                                                  .Padding(8, 0, 0, 0)
                                                      [SNew(SButton)
                                                           .ButtonStyle(&SubmitButtonStyle)
                                                           .HAlign(HAlign_Center)
                                                           .VAlign(VAlign_Center)
                                                           .OnClicked(this, &SConvaiLogExportDialog::OnExportClicked)
                                                               [SNew(STextBlock)
                                                                    .Text(LOCTEXT("SubmitButton", "Submit to Support"))
                                                                    .Font(ButtonFont)
                                                                    .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.windowBg"))]]]]]];
}

bool SConvaiLogExportDialog::ShowDialog(FConvaiIssueReport &OutReport)
{
    TSharedRef<SConvaiLogExportDialog> Dialog = SNew(SConvaiLogExportDialog);

    TSharedRef<SWindow> Window = SNew(SWindow)
                                     .Title(LOCTEXT("WindowTitle", "Contact Convai Support"))
                                     .ClientSize(FVector2D(750, 760))
                                     .SupportsMaximize(false)
                                     .SupportsMinimize(false)
                                     .SizingRule(ESizingRule::FixedSize)
                                     .IsTopmostWindow(true)
                                     .UseOSWindowBorder(false)
                                     .CreateTitleBar(false)
                                     .HasCloseButton(true);

    // Wrap dialog content with draggable background
    Window->SetContent(
        SNew(SDraggableBackground)
            .ParentWindow(Window)
                [Dialog]);

    FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());

    if (Dialog->bDialogResult)
    {
        OutReport = Dialog->UserReport;
        return true;
    }

    return false;
}

FReply SConvaiLogExportDialog::OnExportClicked()
{
    // Collect user input
    UserReport.Description = DescriptionTextBox->GetText().ToString();
    UserReport.Category = IndexToCategory(SelectedCategoryIndex);
    UserReport.Severity = IndexToSeverity(SelectedSeverityIndex);
    UserReport.TimeStarted = *TimeStartedOptions[SelectedTimeStartedIndex];
    UserReport.bIsReproducible = ReproducibleCheckBox->IsChecked();
    UserReport.ReproductionSteps = ReproStepsTextBox->GetText().ToString();

    // Validate inputs
    FString ErrorMessage;
    if (!ValidateInputs(ErrorMessage))
    {
        // Show error notification
        FNotificationInfo Info(FText::FromString(ErrorMessage));
        Info.bFireAndForget = true;
        Info.FadeInDuration = 0.2f;
        Info.FadeOutDuration = 0.5f;
        Info.ExpireDuration = 10.0f;

        TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
        if (NotificationItem.IsValid())
        {
            NotificationItem->SetCompletionState(SNotificationItem::CS_Fail);
        }

        return FReply::Handled();
    }

    bDialogResult = true;

    // Close window
    if (FSlateApplication::IsInitialized())
    {
        TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().FindWidgetWindow(AsShared());
        if (ParentWindow.IsValid())
        {
            ParentWindow->RequestDestroyWindow();
        }
    }

    return FReply::Handled();
}

FReply SConvaiLogExportDialog::OnCancelClicked()
{
    bDialogResult = false;

    // Close window
    if (FSlateApplication::IsInitialized())
    {
        TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().FindWidgetWindow(AsShared());
        if (ParentWindow.IsValid())
        {
            ParentWindow->RequestDestroyWindow();
        }
    }

    return FReply::Handled();
}

bool SConvaiLogExportDialog::ValidateInputs(FString &OutErrorMessage)
{
    const int32 MinDescriptionLength = 20;
    const int32 MinReproStepsLength = 10;

    FString Description = DescriptionTextBox->GetText().ToString().TrimStartAndEnd();
    if (Description.IsEmpty())
    {
        OutErrorMessage = TEXT("Please provide a description of the issue.");
        return false;
    }
    if (Description.Len() < MinDescriptionLength)
    {
        OutErrorMessage = FString::Printf(TEXT("Description is too short. Please provide at least %d characters."), MinDescriptionLength);
        return false;
    }

    if (UserReport.Category == EConvaiIssueCategory::None)
    {
        OutErrorMessage = TEXT("Please select an issue category.");
        return false;
    }

    FString ReproSteps = ReproStepsTextBox->GetText().ToString().TrimStartAndEnd();
    if (ReproSteps.IsEmpty())
    {
        OutErrorMessage = TEXT("Please provide steps to reproduce the issue.");
        return false;
    }
    if (ReproSteps.Len() < MinReproStepsLength)
    {
        OutErrorMessage = FString::Printf(TEXT("Reproduction steps are too short. Please provide at least %d characters."), MinReproStepsLength);
        return false;
    }

    return true;
}

TSharedRef<SWidget> SConvaiLogExportDialog::OnGenerateCategoryWidget(TSharedPtr<FString> Item)
{
    FLinearColor CategoryColor = FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary");
    int32 Index = CategoryOptions.IndexOfByKey(Item);

    switch (Index)
    {
    case 0:
        CategoryColor = FConvaiStyle::RequireColor("Convai.Color.component.form.categoryConnection");
        break;
    case 1:
        CategoryColor = FConvaiStyle::RequireColor("Convai.Color.component.form.categoryCrash");
        break;
    case 2:
        CategoryColor = FConvaiStyle::RequireColor("Convai.Color.component.form.categoryAudio");
        break;
    case 3:
        CategoryColor = FConvaiStyle::RequireColor("Convai.Color.component.form.categoryCharacter");
        break;
    case 4:
        CategoryColor = FConvaiStyle::RequireColor("Convai.Color.component.form.categorySettings");
        break;
    case 5:
        CategoryColor = FConvaiStyle::RequireColor("Convai.Color.component.form.categoryBug");
        break;
    case 6:
        CategoryColor = FConvaiStyle::RequireColor("Convai.Color.component.form.categoryFeature");
        break;
    default:
        CategoryColor = FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary");
        break;
    }

    return SNew(SBox)
        .Padding(FMargin(12, 8))
            [SNew(STextBlock)
                 .Text(FText::FromString(*Item))
                 .Font(FCoreStyle::GetDefaultFontStyle("Regular", 13))
                 .ColorAndOpacity(CategoryColor)];
}

TSharedRef<SWidget> SConvaiLogExportDialog::OnGenerateSeverityWidget(TSharedPtr<FString> Item)
{
    FLinearColor SeverityColor = FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary");
    int32 Index = SeverityOptions.IndexOfByKey(Item);

    switch (Index)
    {
    case 0:
        SeverityColor = FConvaiStyle::RequireColor("Convai.Color.component.form.severityCritical");
        break;
    case 1:
        SeverityColor = FConvaiStyle::RequireColor("Convai.Color.component.form.severityHigh");
        break;
    case 2:
        SeverityColor = FConvaiStyle::RequireColor("Convai.Color.component.form.severityMedium");
        break;
    case 3:
        SeverityColor = FConvaiStyle::RequireColor("Convai.Color.component.form.severityLow");
        break;
    default:
        SeverityColor = FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary");
        break;
    }

    return SNew(SBox)
        .Padding(FMargin(12, 8))
            [SNew(STextBlock)
                 .Text(FText::FromString(*Item))
                 .Font(FCoreStyle::GetDefaultFontStyle("Regular", 13))
                 .ColorAndOpacity(SeverityColor)];
}

TSharedRef<SWidget> SConvaiLogExportDialog::OnGenerateTimeStartedWidget(TSharedPtr<FString> Item)
{
    return SNew(SBox)
        .Padding(FMargin(12, 8))
            [SNew(STextBlock)
                 .Text(FText::FromString(*Item))
                 .Font(FCoreStyle::GetDefaultFontStyle("Regular", 13))
                 .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.component.dialog.textPrimary"))];
}

FText SConvaiLogExportDialog::GetSelectedCategoryText() const
{
    if (CategoryOptions.IsValidIndex(SelectedCategoryIndex))
    {
        return FText::FromString(*CategoryOptions[SelectedCategoryIndex]);
    }
    return LOCTEXT("SelectCategory", "Select a category...");
}

FText SConvaiLogExportDialog::GetSelectedSeverityText() const
{
    if (SeverityOptions.IsValidIndex(SelectedSeverityIndex))
    {
        return FText::FromString(*SeverityOptions[SelectedSeverityIndex]);
    }
    return LOCTEXT("SelectSeverity", "Select severity...");
}

FText SConvaiLogExportDialog::GetSelectedTimeStartedText() const
{
    if (TimeStartedOptions.IsValidIndex(SelectedTimeStartedIndex))
    {
        return FText::FromString(*TimeStartedOptions[SelectedTimeStartedIndex]);
    }
    return LOCTEXT("SelectTime", "Select when...");
}

void SConvaiLogExportDialog::OnCategorySelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
    SelectedCategoryIndex = CategoryOptions.IndexOfByKey(NewSelection);
}

void SConvaiLogExportDialog::OnSeveritySelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
    SelectedSeverityIndex = SeverityOptions.IndexOfByKey(NewSelection);
}

void SConvaiLogExportDialog::OnTimeStartedSelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
    SelectedTimeStartedIndex = TimeStartedOptions.IndexOfByKey(NewSelection);
}

EConvaiIssueCategory SConvaiLogExportDialog::IndexToCategory(int32 Index) const
{
    switch (Index)
    {
    case 0:
        return EConvaiIssueCategory::ConnectionIssue;
    case 1:
        return EConvaiIssueCategory::CrashOnStartup;
    case 2:
        return EConvaiIssueCategory::AudioVoiceIssue;
    case 3:
        return EConvaiIssueCategory::CharacterNotResponding;
    case 4:
        return EConvaiIssueCategory::SettingsConfig;
    case 5:
        return EConvaiIssueCategory::OtherBug;
    case 6:
        return EConvaiIssueCategory::FeatureRequest;
    default:
        return EConvaiIssueCategory::None;
    }
}

EConvaiIssueSeverity SConvaiLogExportDialog::IndexToSeverity(int32 Index) const
{
    switch (Index)
    {
    case 0:
        return EConvaiIssueSeverity::Critical;
    case 1:
        return EConvaiIssueSeverity::High;
    case 2:
        return EConvaiIssueSeverity::Medium;
    case 3:
        return EConvaiIssueSeverity::Low;
    default:
        return EConvaiIssueSeverity::Medium;
    }
}

#undef LOCTEXT_NAMESPACE
