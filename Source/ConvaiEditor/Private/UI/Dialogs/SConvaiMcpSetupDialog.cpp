/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiMcpSetupDialog.cpp
 */

#include "UI/Dialogs/SConvaiMcpSetupDialog.h"

#if CONVAI_MCP_SUPPORTED

#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/MessageDialog.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/CoreStyle.h"
#include "UI/Widgets/SRoundedBox.h"
#include "UnrealEdMisc.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "SConvaiMcpSetupDialog"

void SConvaiMcpSetupDialog::Construct(const FArguments& InArgs)
{
	ParentWindow = InArgs._ParentWindow;

	AgentOptions.Add(MakeShared<EConvaiMcpAgent>(EConvaiMcpAgent::ClaudeCode));
	AgentOptions.Add(MakeShared<EConvaiMcpAgent>(EConvaiMcpAgent::Cursor));
	AgentOptions.Add(MakeShared<EConvaiMcpAgent>(EConvaiMcpAgent::VSCode));
	AgentOptions.Add(MakeShared<EConvaiMcpAgent>(EConvaiMcpAgent::Gemini));
	AgentOptions.Add(MakeShared<EConvaiMcpAgent>(EConvaiMcpAgent::Codex));
	bHasCurrentAgent = FConvaiMcpSetupService::TryGetPreferredAgent(CurrentAgent);
	bSetupPending = FConvaiMcpSetupService::IsSetupPending();
	if (bHasCurrentAgent)
	{
		SelectedAgent = CurrentAgent;
	}
	TSharedPtr<EConvaiMcpAgent> InitiallySelectedAgent = AgentOptions[0];
	for (const TSharedPtr<EConvaiMcpAgent>& Option : AgentOptions)
	{
		if (Option.IsValid() && *Option == SelectedAgent)
		{
			InitiallySelectedAgent = Option;
			break;
		}
	}
	bConfigureTerminal = FConvaiMcpSetupService::IsTerminalConfiguredForAgent(SelectedAgent);
	bLaunchCodexWithFullPermissions = FConvaiMcpSetupService::IsTerminalConfiguredWithFullPermissions(SelectedAgent);
	RefreshSelectedCli();

	ContextWriteModeOptions.Add(MakeShared<EConvaiContextWriteMode>(EConvaiContextWriteMode::Append));
	ContextWriteModeOptions.Add(MakeShared<EConvaiContextWriteMode>(EConvaiContextWriteMode::Replace));

	const FLinearColor SurfaceColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.dialog.surfaceBg"));
	const FLinearColor BorderColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.dialog.borderAccent"));
	const FLinearColor PrimaryTextColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.dialog.textPrimary"));
	const FLinearColor SecondaryTextColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.dialog.textSecondary"));
	const FLinearColor HintTextColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.dialog.textHint"));
	const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 22);
	const FSlateFontInfo SectionFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 14);
	const FSlateFontInfo BodyFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 12);
	const FSlateFontInfo CaptionFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10);

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
		.BorderBackgroundColor(FConvaiStyle::RequireColor(TEXT("Convai.Color.surface.window")))
		.Padding(0.0f)
		[
			SNew(SScrollBox)

			+ SScrollBox::Slot()
			[
				SNew(SBox)
				.WidthOverride(600.0f)
				.Padding(24.0f)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Title", "Connect an AI assistant"))
						.Font(TitleFont)
						.ColorAndOpacity(PrimaryTextColor)
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Blurb", "Choose the assistant you use for AI coding. Convai will connect it to this Unreal project so it can use editor tools."))
						.Font(BodyFont)
						.ColorAndOpacity(SecondaryTextColor)
						.AutoWrapText(true)
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 20)
					[
						SNew(STextBlock)
						.Text(this, &SConvaiMcpSetupDialog::GetCurrentAgentText)
						.Font(CaptionFont)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
					[
						SNew(SRoundedBox)
						.BorderRadius(10.0f)
						.BackgroundColor(SurfaceColor)
						.BorderColor(BorderColor.CopyWithNewOpacity(0.55f))
						.BorderThickness(1.0f)
						.ContentPadding(FMargin(16.0f))
						[
							SNew(SVerticalBox)

							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ChooseAssistant", "1. Choose your assistant"))
								.Font(SectionFont)
								.ColorAndOpacity(PrimaryTextColor)
							]

							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SComboBox<TSharedPtr<EConvaiMcpAgent>>)
								.OptionsSource(&AgentOptions)
								.InitiallySelectedItem(InitiallySelectedAgent)
								.ContentPadding(FMargin(10.0f, 6.0f))
								.OnGenerateWidget_Lambda([PrimaryTextColor](TSharedPtr<EConvaiMcpAgent> Item)
								{
									return SNew(STextBlock)
										.Text(Item.IsValid() ? FConvaiMcpSetupService::GetAgentDisplayName(*Item) : FText::GetEmpty())
										.ColorAndOpacity(PrimaryTextColor);
								})
								.OnSelectionChanged_Lambda([this](TSharedPtr<EConvaiMcpAgent> Item, ESelectInfo::Type)
								{
									if (Item.IsValid())
									{
										SelectedAgent = *Item;
										bConfigureTerminal = FConvaiMcpSetupService::IsTerminalConfiguredForAgent(SelectedAgent);
										bLaunchCodexWithFullPermissions = FConvaiMcpSetupService::IsTerminalConfiguredWithFullPermissions(SelectedAgent);
										RefreshSelectedCli();
									}
								})
								[
									SNew(STextBlock)
									.Text_Lambda([this]() { return FConvaiMcpSetupService::GetAgentDisplayName(SelectedAgent); })
									.ColorAndOpacity(PrimaryTextColor)
								]
							]

							+ SVerticalBox::Slot().AutoHeight().Padding(2, 7, 0, 12)
							[
								SNew(STextBlock)
								.Text(this, &SConvaiMcpSetupDialog::GetAgentDescription)
								.Font(CaptionFont)
								.ColorAndOpacity(HintTextColor)
								.AutoWrapText(true)
							]

							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SHorizontalBox)

								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0, 1, 8, 0)
								[
									SNew(STextBlock)
									.Text(FText::FromString(FString::Chr(0x25CF)))
									.Font(BodyFont)
									.ColorAndOpacity(this, &SConvaiMcpSetupDialog::GetCliStatusColor)
								]

								+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
								[
									SNew(SVerticalBox)

									+ SVerticalBox::Slot().AutoHeight()
									[
										SNew(STextBlock)
										.Text(this, &SConvaiMcpSetupDialog::GetCliStatusTitle)
										.ToolTipText(this, &SConvaiMcpSetupDialog::GetCliStatusToolTip)
										.Font(SectionFont)
										.ColorAndOpacity(this, &SConvaiMcpSetupDialog::GetCliStatusColor)
									]

									+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
									[
										SNew(STextBlock)
										.Text(this, &SConvaiMcpSetupDialog::GetCliStatusDetail)
										.ToolTipText(this, &SConvaiMcpSetupDialog::GetCliStatusToolTip)
										.Font(CaptionFont)
										.ColorAndOpacity(SecondaryTextColor)
										.AutoWrapText(true)
									]
								]

								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 0, 0)
								[
									SNew(SButton)
									.ButtonStyle(FConvaiStyle::Get(), TEXT("Convai.Button.Secondary"))
									.TextStyle(FConvaiStyle::Get(), TEXT("Convai.Button.Secondary.Text"))
									.Text(this, &SConvaiMcpSetupDialog::GetInstallButtonText)
									.Visibility(this, &SConvaiMcpSetupDialog::GetInstallButtonVisibility)
									.ToolTipText(LOCTEXT("InstallGuideTooltip", "Open the official installation guide in your browser."))
									.OnClicked(this, &SConvaiMcpSetupDialog::OnOpenInstallPage)
								]

								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
								[
									SNew(SButton)
									.ButtonStyle(FConvaiStyle::Get(), TEXT("Convai.Button.Secondary"))
									.TextStyle(FConvaiStyle::Get(), TEXT("Convai.Button.Secondary.Text"))
									.Text(LOCTEXT("RecheckCli", "Recheck"))
									.Visibility(this, &SConvaiMcpSetupDialog::GetRecheckButtonVisibility)
									.ToolTipText(LOCTEXT("RecheckCliTooltip", "Scan for the selected command-line assistant again."))
									.OnClicked(this, &SConvaiMcpSetupDialog::OnRecheckCli)
								]
							]
						]
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
					[
						SNew(SRoundedBox)
						.BorderRadius(10.0f)
						.BackgroundColor(SurfaceColor)
						.BorderColor(BorderColor.CopyWithNewOpacity(0.35f))
						.BorderThickness(1.0f)
						.ContentPadding(FMargin(16.0f))
						[
							SNew(SVerticalBox)

							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("SetupOptions", "2. Choose how it works"))
								.Font(SectionFont)
								.ColorAndOpacity(PrimaryTextColor)
							]

							+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 8)
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([this]() { return bAutoStartServer ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bAutoStartServer = State == ECheckBoxState::Checked; })
								[
									SNew(SVerticalBox)
									+ SVerticalBox::Slot().AutoHeight()
									[
										SNew(STextBlock)
										.Text(LOCTEXT("AutoStart", "Connect automatically when this project opens"))
										.Font(BodyFont)
										.ColorAndOpacity(PrimaryTextColor)
									]
									+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
									[
										SNew(STextBlock)
										.Text(LOCTEXT("AutoStartDetail", "Recommended. Keeps Unreal ready for your assistant."))
										.Font(CaptionFont)
										.ColorAndOpacity(HintTextColor)
									]
								]
							]

							+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 8)
							[
								SNew(SCheckBox)
								.Visibility(this, &SConvaiMcpSetupDialog::GetTerminalOptionVisibility)
								.IsChecked_Lambda([this]() { return bConfigureTerminal ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
								{
									bConfigureTerminal = State == ECheckBoxState::Checked;
									if (!bConfigureTerminal)
									{
										bLaunchCodexWithFullPermissions = false;
									}
								})
								[
									SNew(SVerticalBox)
									+ SVerticalBox::Slot().AutoHeight()
									[
										SNew(STextBlock)
										.Text_Lambda([this]()
										{
											return FText::Format(
												LOCTEXT("PrepareTerminal", "Prepare Unreal's terminal for {0}"),
												FConvaiMcpSetupService::GetAgentDisplayName(SelectedAgent));
										})
										.Font(BodyFont)
										.ColorAndOpacity(PrimaryTextColor)
									]
									+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
									[
										SNew(STextBlock)
										.Text(LOCTEXT("PrepareTerminalDetail", "Opening a terminal starts in this project and launches your assistant."))
										.Font(CaptionFont)
										.ColorAndOpacity(HintTextColor)
										.AutoWrapText(true)
									]
								]
							]

							+ SVerticalBox::Slot().AutoHeight().Padding(24, 0, 0, 8)
							[
								SNew(STextBlock)
								.Visibility(this, &SConvaiMcpSetupDialog::GetTerminalConflictVisibility)
								.Text(LOCTEXT("TermConflict", "Unreal already has terminal startup commands. Convai will update its managed commands and leave unrelated commands in place."))
								.Font(CaptionFont)
								.ColorAndOpacity(FLinearColor(0.95f, 0.68f, 0.24f))
								.AutoWrapText(true)
							]

							+ SVerticalBox::Slot().AutoHeight().Padding(24, 0, 0, 12)
							[
								SNew(SRoundedBox)
								.Visibility(this, &SConvaiMcpSetupDialog::GetFullPermissionsVisibility)
								.BorderRadius(8.0f)
								.BackgroundColor(FLinearColor(0.20f, 0.055f, 0.035f, 0.95f))
								.BorderColor(FLinearColor(0.95f, 0.30f, 0.18f, 0.85f))
								.BorderThickness(1.0f)
								.ContentPadding(FMargin(12.0f))
								[
									SNew(SCheckBox)
									.IsChecked_Lambda([this]()
									{
										return bLaunchCodexWithFullPermissions ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
									})
									.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
									{
										bLaunchCodexWithFullPermissions = State == ECheckBoxState::Checked;
										if (bLaunchCodexWithFullPermissions)
										{
											// Full permissions are a terminal launch mode. Make the
											// dependency explicit instead of hiding this option until
											// the user discovers the separate terminal checkbox.
											bConfigureTerminal = true;
										}
									})
									.ToolTipText(LOCTEXT("FullPermissionsTooltip", "Launches Codex with --dangerously-bypass-approvals-and-sandbox in Unreal's interactive terminal. Auto Tagger continues using its separate bounded process."))
									[
										SNew(SVerticalBox)
										+ SVerticalBox::Slot().AutoHeight()
										[
											SNew(STextBlock)
											.Text(LOCTEXT("FullPermissionsLabel", "Full access in Unreal's terminal"))
											.Font(BodyFont)
											.ColorAndOpacity(FLinearColor(1.0f, 0.78f, 0.68f))
										]
										+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
										[
											SNew(STextBlock)
											.Text(LOCTEXT("FullPermissionsWarning", "Dangerous: Codex runs without approval prompts or a sandbox and can read, change, delete, or send any file available to your OS account. Auto Tagger remains isolated."))
											.Font(CaptionFont)
											.ColorAndOpacity(FLinearColor(1.0f, 0.58f, 0.48f))
											.AutoWrapText(true)
										]
									]
								]
							]

							+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 6)
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([this]() { return bAddPrimer ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bAddPrimer = State == ECheckBoxState::Checked; })
								[
									SNew(SVerticalBox)
									+ SVerticalBox::Slot().AutoHeight()
									[
										SNew(STextBlock)
										.Text(this, &SConvaiMcpSetupDialog::GetAddInstructionsLabel)
										.Font(BodyFont)
										.ColorAndOpacity(PrimaryTextColor)
									]
									+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
									[
										SNew(STextBlock)
										.Text(LOCTEXT("PrimerDetail", "Adds a small managed guide for Convai characters, actions, and scene tools."))
										.Font(CaptionFont)
										.ColorAndOpacity(HintTextColor)
										.AutoWrapText(true)
									]
								]
							]

							+ SVerticalBox::Slot().AutoHeight().Padding(24, 0, 0, 4)
							[
								SNew(STextBlock)
								.Visibility(this, &SConvaiMcpSetupDialog::GetContextExistsVisibility)
								.Text(this, &SConvaiMcpSetupDialog::GetContextExistsWarningText)
								.Font(CaptionFont)
								.ColorAndOpacity(FLinearColor(0.95f, 0.68f, 0.24f))
								.AutoWrapText(true)
							]

							+ SVerticalBox::Slot().AutoHeight().Padding(24, 0, 0, 0)
							[
								SNew(SComboBox<TSharedPtr<EConvaiContextWriteMode>>)
								.Visibility(this, &SConvaiMcpSetupDialog::GetContextExistsVisibility)
								.OptionsSource(&ContextWriteModeOptions)
								.InitiallySelectedItem(ContextWriteModeOptions[0])
								.ContentPadding(FMargin(8.0f, 4.0f))
								.OnGenerateWidget_Lambda([](TSharedPtr<EConvaiContextWriteMode> Item)
								{
									return SNew(STextBlock).Text(Item.IsValid() && *Item == EConvaiContextWriteMode::Replace
										? LOCTEXT("ModeReplace", "Replace the entire file")
										: LOCTEXT("ModeAppend", "Keep existing content (recommended)"));
								})
								.OnSelectionChanged_Lambda([this](TSharedPtr<EConvaiContextWriteMode> Item, ESelectInfo::Type)
								{
									if (Item.IsValid())
									{
										ContextWriteMode = *Item;
									}
								})
								[
									SNew(STextBlock)
									.Text_Lambda([this]()
									{
										return ContextWriteMode == EConvaiContextWriteMode::Replace
											? LOCTEXT("ModeReplace", "Replace the entire file")
											: LOCTEXT("ModeAppend", "Keep existing content (recommended)");
									})
								]
							]
						]
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(2, 0, 2, 18)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("NextStep", "Convai will enable the required Unreal plugins and create this assistant's project connection. Unreal will restart only if needed."))
						.Font(CaptionFont)
						.ColorAndOpacity(SecondaryTextColor)
						.AutoWrapText(true)
					]

					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[
						SNew(SUniformGridPanel)
						.SlotPadding(FMargin(8, 0, 0, 0))

						+ SUniformGridPanel::Slot(0, 0)
						[
							SNew(SBox)
							.MinDesiredWidth(112.0f)
							.HeightOverride(38.0f)
							[
								SNew(SButton)
								.ButtonStyle(FConvaiStyle::Get(), TEXT("Convai.Button.Secondary"))
								.TextStyle(FConvaiStyle::Get(), TEXT("Convai.Button.Secondary.Text"))
								.HAlign(HAlign_Center)
								.VAlign(VAlign_Center)
								.Text(LOCTEXT("Cancel", "Cancel"))
								.OnClicked(this, &SConvaiMcpSetupDialog::OnCancel)
							]
						]

						+ SUniformGridPanel::Slot(1, 0)
						[
							SNew(SBox)
							.MinDesiredWidth(168.0f)
							.HeightOverride(38.0f)
							[
								SNew(SButton)
								.ButtonStyle(FConvaiStyle::Get(), TEXT("Convai.Button.Primary"))
								.TextStyle(FConvaiStyle::Get(), TEXT("Convai.Button.Primary.Text"))
								.HAlign(HAlign_Center)
								.VAlign(VAlign_Center)
								.Text(this, &SConvaiMcpSetupDialog::GetConfirmButtonText)
								.IsEnabled(this, &SConvaiMcpSetupDialog::CanConfirm)
								.ToolTipText_Lambda([this]()
								{
									return CanConfirm()
										? FText::GetEmpty()
										: LOCTEXT("InstallBeforeContinue", "Install the selected command-line assistant before continuing.");
								})
								.OnClicked(this, &SConvaiMcpSetupDialog::OnConfirm)
							]
						]
					]
				]
			]
		]
	];
}

bool SConvaiMcpSetupDialog::IsTerminalOptionEnabled() const
{
	return FConvaiMcpSetupService::IsCliAgent(SelectedAgent);
}

EVisibility SConvaiMcpSetupDialog::GetTerminalOptionVisibility() const
{
	return IsTerminalOptionEnabled() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SConvaiMcpSetupDialog::GetTerminalConflictVisibility() const
{
	const bool bShow = bConfigureTerminal && IsTerminalOptionEnabled() && FConvaiMcpSetupService::HasExistingTerminalStartupCommands();
	return bShow ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SConvaiMcpSetupDialog::GetFullPermissionsVisibility() const
{
	return SelectedAgent == EConvaiMcpAgent::Codex
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FText SConvaiMcpSetupDialog::GetAddInstructionsLabel() const
{
	return FText::Format(
		LOCTEXT("AddInstructionsFmt", "Teach {0} how to use Convai"),
		FConvaiMcpSetupService::GetAgentDisplayName(SelectedAgent));
}

FText SConvaiMcpSetupDialog::GetCurrentAgentText() const
{
	if (!bHasCurrentAgent)
	{
		return LOCTEXT("NoCurrentAgent", "This project is not connected yet.");
	}
	if (bSetupPending)
	{
		return FText::Format(
			LOCTEXT("PendingCurrentAgent", "Setup is pending for {0}; it is not connected until setup applies successfully."),
			FConvaiMcpSetupService::GetAgentDisplayName(CurrentAgent));
	}
	return FText::Format(
		LOCTEXT("CurrentAgent", "Current project assistant: {0}"),
		FConvaiMcpSetupService::GetAgentDisplayName(CurrentAgent));
}

FText SConvaiMcpSetupDialog::GetAgentDescription() const
{
	switch (SelectedAgent)
	{
	case EConvaiMcpAgent::ClaudeCode:
		return LOCTEXT("ClaudeDescription", "Anthropic's command-line coding assistant.");
	case EConvaiMcpAgent::Cursor:
		return LOCTEXT("CursorDescription", "The Cursor desktop code editor.");
	case EConvaiMcpAgent::VSCode:
		return LOCTEXT("VSCodeDescription", "Visual Studio Code with an AI coding extension.");
	case EConvaiMcpAgent::Gemini:
		return LOCTEXT("GeminiDescription", "Google's command-line coding assistant.");
	case EConvaiMcpAgent::Codex:
		return LOCTEXT("CodexDescription", "OpenAI's command-line coding assistant.");
	default:
		return FText::GetEmpty();
	}
}

FText SConvaiMcpSetupDialog::GetCliStatusTitle() const
{
	if (!FConvaiMcpSetupService::IsCliAgent(SelectedAgent))
	{
		return LOCTEXT("DesktopReady", "Ready to connect");
	}
	if (bCliPathChangedSinceDisplay)
	{
		return LOCTEXT("CliPathChanged", "CLI path changed - review");
	}
	if (bSelectedCliFound)
	{
		return FText::Format(
			LOCTEXT("CliDetected", "{0} detected"),
			FConvaiMcpSetupService::GetAgentDisplayName(SelectedAgent));
	}
	return LOCTEXT("CliMissing", "Install required");
}

FText SConvaiMcpSetupDialog::GetCliStatusDetail() const
{
	if (!FConvaiMcpSetupService::IsCliAgent(SelectedAgent))
	{
		return LOCTEXT("DesktopReadyDetail", "This desktop editor does not need a command-line check.");
	}
	if (bSelectedCliFound)
	{
		if (bCliPathChangedSinceDisplay)
		{
			return LOCTEXT("CliChangedDetail", "The detected CLI changed. Review it, then connect again.");
		}
		return LOCTEXT("CliReadyDetail", "Installed and ready to connect.");
	}
	return FText::Format(
		LOCTEXT("CliMissingDetail", "Install {0}, then return to this window."),
		FConvaiMcpSetupService::GetAgentDisplayName(SelectedAgent));
}

FText SConvaiMcpSetupDialog::GetCliStatusToolTip() const
{
	if (!FConvaiMcpSetupService::IsCliAgent(SelectedAgent))
	{
		return FText::Format(
			LOCTEXT("DesktopReadyTooltip", "Convai will create this project's connection for {0}."),
			FConvaiMcpSetupService::GetAgentDisplayName(SelectedAgent));
	}
	if (bSelectedCliFound)
	{
		return FText::Format(
			LOCTEXT("CliReadyTooltip", "Connect approves this exact executable path for {0}:\n{1}"),
			FConvaiMcpSetupService::GetAgentDisplayName(SelectedAgent),
			FText::FromString(SelectedCliExecutablePath));
	}
	return FText::Format(
		LOCTEXT("CliMissingTooltip", "Convai could not find the '{0}' command on your system PATH."),
		FText::FromString(FConvaiMcpSetupService::GetCliCommand(SelectedAgent)));
}

FSlateColor SConvaiMcpSetupDialog::GetCliStatusColor() const
{
	if (!FConvaiMcpSetupService::IsCliAgent(SelectedAgent))
	{
		return FSlateColor(FConvaiStyle::RequireColor(TEXT("Convai.Color.component.dialog.accentGreen")));
	}
	return bSelectedCliFound && !bCliPathChangedSinceDisplay
		? FSlateColor(FLinearColor(0.22f, 0.78f, 0.42f))
		: FSlateColor(FLinearColor(0.95f, 0.60f, 0.20f));
}

EVisibility SConvaiMcpSetupDialog::GetInstallButtonVisibility() const
{
	return FConvaiMcpSetupService::IsCliAgent(SelectedAgent)
		&& !bSelectedCliFound
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SConvaiMcpSetupDialog::GetRecheckButtonVisibility() const
{
	return FConvaiMcpSetupService::IsCliAgent(SelectedAgent)
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FText SConvaiMcpSetupDialog::GetInstallButtonText() const
{
	return FText::Format(
		LOCTEXT("InstallAgent", "Install {0}"),
		FConvaiMcpSetupService::GetAgentDisplayName(SelectedAgent));
}

FText SConvaiMcpSetupDialog::GetConfirmButtonText() const
{
	return FConvaiMcpSetupService::AreRequiredPluginsEnabled()
		? LOCTEXT("ConnectAssistant", "Connect assistant")
		: LOCTEXT("EnableAndRestart", "Enable & restart");
}

bool SConvaiMcpSetupDialog::CanConfirm() const
{
	if (!FConvaiMcpSetupService::IsCliAgent(SelectedAgent))
	{
		return true;
	}
	return bSelectedCliFound;
}

FReply SConvaiMcpSetupDialog::OnOpenInstallPage()
{
	const FString Url = FConvaiMcpSetupService::GetAgentInstallUrl(SelectedAgent);
	if (!Url.IsEmpty())
	{
		FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
	}
	return FReply::Handled();
}

FReply SConvaiMcpSetupDialog::OnRecheckCli()
{
	RefreshSelectedCli();
	return FReply::Handled();
}

void SConvaiMcpSetupDialog::RefreshSelectedCli()
{
	bCliPathChangedSinceDisplay = false;
	SelectedCliExecutablePath.Reset();
	bSelectedCliFound = FConvaiMcpSetupService::IsCliAgent(SelectedAgent)
		&& FConvaiMcpSetupService::FindCliExecutable(SelectedAgent, SelectedCliExecutablePath);
}

EVisibility SConvaiMcpSetupDialog::GetContextExistsVisibility() const
{
	const bool bShow = bAddPrimer && FConvaiMcpSetupService::DoesAgentContextFileExist(SelectedAgent);
	return bShow ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SConvaiMcpSetupDialog::GetContextExistsWarningText() const
{
	return FText::Format(
		LOCTEXT("ContextExistsFmt", "{0} already exists. Choose how to add the Convai instructions:"),
		FText::FromString(FConvaiMcpSetupService::GetAgentContextFile(SelectedAgent)));
}

FReply SConvaiMcpSetupDialog::OnConfirm()
{
	if (FConvaiMcpSetupService::IsCliAgent(SelectedAgent))
	{
		FString CurrentExecutablePath;
		if (!FConvaiMcpSetupService::FindCliExecutable(SelectedAgent, CurrentExecutablePath))
		{
			SelectedCliExecutablePath.Reset();
			bSelectedCliFound = false;
			bCliPathChangedSinceDisplay = false;
			return FReply::Handled();
		}
#if PLATFORM_WINDOWS
		const bool bSamePath = CurrentExecutablePath.Equals(SelectedCliExecutablePath, ESearchCase::IgnoreCase);
#else
		const bool bSamePath = CurrentExecutablePath.Equals(SelectedCliExecutablePath, ESearchCase::CaseSensitive);
#endif
		if (!bSelectedCliFound || !bSamePath)
		{
			SelectedCliExecutablePath = MoveTemp(CurrentExecutablePath);
			bSelectedCliFound = true;
			bCliPathChangedSinceDisplay = true;
			return FReply::Handled();
		}
	}
	if (bLaunchCodexWithFullPermissions)
	{
		// Keep the destructive-capability confirmation separate from the checkbox click.
		const EAppReturnType::Type Choice = FMessageDialog::Open(
			EAppMsgType::YesNo,
			LOCTEXT(
				"ConfirmFullPermissions",
				"Enable full permissions for Codex?\n\nThis disables Codex approval prompts and its sandbox when launched from Unreal's terminal. Codex may read, modify, delete, or transmit any files your OS account can access.\n\nAuto Tagger does not use this permission setting. Continue only if you trust this project and the instructions you give Codex."));
		if (Choice != EAppReturnType::Yes)
		{
			return FReply::Handled();
		}
	}
	bConfirmed = true;
	CloseParent();
	return FReply::Handled();
}

FReply SConvaiMcpSetupDialog::OnCancel()
{
	bConfirmed = false;
	CloseParent();
	return FReply::Handled();
}

void SConvaiMcpSetupDialog::CloseParent()
{
	if (TSharedPtr<SWindow> Window = ParentWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
}

bool SConvaiMcpSetupDialog::ShowDialog(FConvaiMcpSetupOptions& OutOptions)
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WindowTitle", "Convai - Connect AI Assistant"))
		.ClientSize(FVector2D(680.0f, 660.0f))
		.SizingRule(ESizingRule::FixedSize)
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedRef<SConvaiMcpSetupDialog> Dialog = SNew(SConvaiMcpSetupDialog).ParentWindow(Window);
	Window->SetContent(Dialog);

	GEditor->EditorAddModalWindow(Window);

	if (Dialog->bConfirmed)
	{
		OutOptions.Agent = Dialog->SelectedAgent;
		OutOptions.bAutoStartServer = Dialog->bAutoStartServer;
		OutOptions.bConfigureTerminal = Dialog->bConfigureTerminal && FConvaiMcpSetupService::IsCliAgent(Dialog->SelectedAgent);
		OutOptions.bLaunchCodexWithFullPermissions = OutOptions.bConfigureTerminal
			&& Dialog->SelectedAgent == EConvaiMcpAgent::Codex
			&& Dialog->bLaunchCodexWithFullPermissions;
		OutOptions.bAddPrimer = Dialog->bAddPrimer;
		OutOptions.ContextWriteMode = Dialog->ContextWriteMode;
		OutOptions.ConfirmedCliExecutablePath = Dialog->SelectedCliExecutablePath;
		return true;
	}
	return false;
}

bool SConvaiMcpSetupDialog::RunSetupFlow()
{
	FConvaiMcpSetupOptions Options;
	if (!ShowDialog(Options))
	{
		return false;
	}

	FString Error;
	bool bRestartRequired = true;
	if (!FConvaiMcpSetupService::EnableAndScheduleSetup(Options, Error, &bRestartRequired))
	{
		FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("Convai MCP setup failed:\n%s"), *Error)));
		Info.ExpireDuration = 8.0f;
		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(SNotificationItem::CS_Fail);
		}
		return false;
	}

	if (!bRestartRequired)
	{
		return true;
	}
	const EAppReturnType::Type Choice = FMessageDialog::Open(
		EAppMsgType::YesNo,
		LOCTEXT("McpRestartPrompt", "The MCP, Toolsets and Terminal plugins were enabled. The editor must restart to finish setup.\n\nRestart now?"));
	if (Choice == EAppReturnType::Yes)
	{
		FUnrealEdMisc::Get().RestartEditor(false);
	}
	return true;
}

#undef LOCTEXT_NAMESPACE

#endif // CONVAI_MCP_SUPPORTED
