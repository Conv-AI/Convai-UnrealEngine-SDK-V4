/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiLogExportDialog.h
 *
 * Dialog widget for log export.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "ConvaiLogExporter.h"

enum class EConvaiIssueCategory : uint8
{
    /** Can't connect to Convai servers */
    ConnectionIssue,
    /** Plugin crashes on startup */
    CrashOnStartup,
    /** Audio/Voice problems */
    AudioVoiceIssue,
    /** Character behavior issues */
    CharacterNotResponding,
    /** Settings/Configuration problems */
    SettingsConfig,
    /** Other bug */
    OtherBug,
    /** Feature request */
    FeatureRequest,
    /** No category selected */
    None
};

/**
 * Issue severity levels
 */
enum class EConvaiIssueSeverity : uint8
{
    /** Blocking, can't use the plugin */
    Critical,
    /** Major functionality broken */
    High,
    /** Some features don't work */
    Medium,
    /** Minor issue or question */
    Low
};

/**
 * Structure to hold user's issue report
 */
struct FConvaiIssueReport
{
    /** User's description of the issue */
    FString Description;

    /** Selected issue category */
    EConvaiIssueCategory Category = EConvaiIssueCategory::None;

    /** Issue severity */
    EConvaiIssueSeverity Severity = EConvaiIssueSeverity::Medium;

    /** When the issue started */
    FString TimeStarted;

    /** Whether the issue is reproducible */
    bool bIsReproducible = false;

    /** Steps to reproduce (if applicable) */
    FString ReproductionSteps;

    FConvaiIssueReport() = default;

    /** Convert to JSON for export */
    TSharedPtr<FJsonObject> ToJson() const;

    /** Get category as string */
    static FString CategoryToString(EConvaiIssueCategory Category);

    /** Get severity as string */
    static FString SeverityToString(EConvaiIssueSeverity Severity);
};

/**
 * Dialog widget for collecting user's issue description before exporting logs.
 */
class SConvaiLogExportDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SConvaiLogExportDialog) {}
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);

    /**
     * Show the dialog and get user input
     * @param OutReport Filled with user's input if dialog is confirmed
     * @return True if user clicked Export, false if cancelled
     */
    static bool ShowDialog(FConvaiIssueReport &OutReport);

private:
    /** User's issue description text */
    TSharedPtr<class SMultiLineEditableTextBox> DescriptionTextBox;

    /** Selected category combo box */
    TSharedPtr<class SComboBox<TSharedPtr<FString>>> CategoryComboBox;

    /** Selected severity combo box */
    TSharedPtr<class SComboBox<TSharedPtr<FString>>> SeverityComboBox;

    /** When issue started combo box */
    TSharedPtr<class SComboBox<TSharedPtr<FString>>> TimeStartedComboBox;

    /** Is reproducible checkbox */
    TSharedPtr<class SCheckBox> ReproducibleCheckBox;

    /** Reproduction steps text */
    TSharedPtr<class SMultiLineEditableTextBox> ReproStepsTextBox;

    /** Close button reference for hover state checks */
    TSharedPtr<class SButton> CloseButton;

    /** Stored user report */
    FConvaiIssueReport UserReport;

    /** Dialog result (true = export, false = cancel) */
    bool bDialogResult = false;

    /** Category options */
    TArray<TSharedPtr<FString>> CategoryOptions;

    /** Severity options */
    TArray<TSharedPtr<FString>> SeverityOptions;

    /** Time started options */
    TArray<TSharedPtr<FString>> TimeStartedOptions;

    /** Current selections (indices) */
    int32 SelectedCategoryIndex = 0;
    int32 SelectedSeverityIndex = 1; // Default to Medium
    int32 SelectedTimeStartedIndex = 0;

    /** Build the dialog content */
    TSharedRef<SWidget> BuildDialogContent();

    /** Validate form inputs */
    bool ValidateInputs(FString &OutErrorMessage);

    /** Handle Export button clicked */
    FReply OnExportClicked();

    /** Handle Cancel button clicked */
    FReply OnCancelClicked();

    /** Generate text for category combo box */
    TSharedRef<SWidget> OnGenerateCategoryWidget(TSharedPtr<FString> Item);

    /** Generate text for severity combo box */
    TSharedRef<SWidget> OnGenerateSeverityWidget(TSharedPtr<FString> Item);

    /** Generate text for time started combo box */
    TSharedRef<SWidget> OnGenerateTimeStartedWidget(TSharedPtr<FString> Item);

    /** Get currently selected category text */
    FText GetSelectedCategoryText() const;

    /** Get currently selected severity text */
    FText GetSelectedSeverityText() const;

    /** Get currently selected time started text */
    FText GetSelectedTimeStartedText() const;

    /** Handle category selection changed */
    void OnCategorySelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);

    /** Handle severity selection changed */
    void OnSeveritySelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);

    /** Handle time started selection changed */
    void OnTimeStartedSelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);

    /** Initialize combo box options */
    void InitializeOptions();

    /** Convert index to enum */
    EConvaiIssueCategory IndexToCategory(int32 Index) const;
    EConvaiIssueSeverity IndexToSeverity(int32 Index) const;
};
