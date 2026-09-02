/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiMcpSetupDialog.cpp
 */

#include "UI/Dialogs/SConvaiMcpSetupDialog.h"

#if CONVAI_MCP_SUPPORTED

#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
#include "Styling/AppStyle.h"
#define CONVAI_MCP_STYLE FAppStyle
#else
#include "EditorStyleSet.h"
#define CONVAI_MCP_STYLE FEditorStyle
#endif

#define LOCTEXT_NAMESPACE "SConvaiMcpSetupDialog"

void SConvaiMcpSetupDialog::Construct(const FArguments& InArgs)
{
	ParentWindow = InArgs._ParentWindow;

	AgentOptions.Add(MakeShared<EConvaiMcpAgent>(EConvaiMcpAgent::ClaudeCode));
	AgentOptions.Add(MakeShared<EConvaiMcpAgent>(EConvaiMcpAgent::Cursor));
	AgentOptions.Add(MakeShared<EConvaiMcpAgent>(EConvaiMcpAgent::VSCode));
	AgentOptions.Add(MakeShared<EConvaiMcpAgent>(EConvaiMcpAgent::Gemini));
	AgentOptions.Add(MakeShared<EConvaiMcpAgent>(EConvaiMcpAgent::Codex));

	ContextWriteModeOptions.Add(MakeShared<EConvaiContextWriteMode>(EConvaiContextWriteMode::Append));
	ContextWriteModeOptions.Add(MakeShared<EConvaiContextWriteMode>(EConvaiContextWriteMode::Replace));

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(500.0f)
		.Padding(16.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Title", "Set up this project for AI coding (MCP)"))
				.Font(CONVAI_MCP_STYLE::Get().GetFontStyle("HeadingExtraSmall"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("Blurb", "Enables the Unreal MCP, Toolsets and Terminal plugins and configures your AI agent to drive this project. The editor will restart to finish."))
			]

			// Agent picker
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock).Text(LOCTEXT("AgentLabel", "AI agent:"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(SComboBox<TSharedPtr<EConvaiMcpAgent>>)
				.OptionsSource(&AgentOptions)
				.InitiallySelectedItem(AgentOptions[0])
				.OnGenerateWidget_Lambda([](TSharedPtr<EConvaiMcpAgent> Item)
				{
					return SNew(STextBlock).Text(FConvaiMcpSetupService::GetAgentDisplayName(*Item));
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<EConvaiMcpAgent> Item, ESelectInfo::Type)
				{
					if (Item.IsValid())
					{
						SelectedAgent = *Item;
						if (!IsTerminalOptionEnabled())
						{
							bConfigureTerminal = false;
						}
					}
				})
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FConvaiMcpSetupService::GetAgentDisplayName(SelectedAgent); })
				]
			]

			// Auto-start server
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() { return bAutoStartServer ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bAutoStartServer = (S == ECheckBoxState::Checked); })
				[
					SNew(STextBlock)
					.WrapTextAt(440.0f)
					.Text(LOCTEXT("AutoStart", "Auto-start the MCP server when the editor launches"))
				]
			]

			// Configure terminal (CLI agents only)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SCheckBox)
				.IsEnabled_Lambda([this]() { return IsTerminalOptionEnabled(); })
				.IsChecked_Lambda([this]() { return (bConfigureTerminal && IsTerminalOptionEnabled()) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bConfigureTerminal = (S == ECheckBoxState::Checked); })
				[
					SNew(STextBlock)
					.WrapTextAt(440.0f)
					.Text_Lambda([this]()
					{
						if (!IsTerminalOptionEnabled())
						{
							return LOCTEXT("ConfigTermGui", "Configure the Terminal (unavailable: this agent is a GUI app, not a CLI)");
						}
						return FText::Format(
							LOCTEXT("ConfigTermFmt", "Set up the Terminal plugin so opening a terminal starts in this project and runs {0}"),
							FText::FromString(FConvaiMcpSetupService::GetCliCommand(SelectedAgent)));
					})
				]
			]

			// Terminal conflict warning
			+ SVerticalBox::Slot().AutoHeight().Padding(24, 0, 0, 2)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Visibility_Lambda([this]() { return GetTerminalConflictVisibility(); })
				.ColorAndOpacity(FLinearColor(1.0f, 0.66f, 0.0f))
				.Text(LOCTEXT("TermConflict", "You already have terminal startup commands; a Convai block will be added (your existing commands are preserved)."))
			]

			// Add Convai instructions (independent of the terminal option)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 2)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() { return bAddPrimer ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bAddPrimer = (S == ECheckBoxState::Checked); })
				[
					SNew(STextBlock)
					.WrapTextAt(440.0f)
					.Text_Lambda([this]() { return GetAddInstructionsLabel(); })
				]
			]

			// Existing-context-file warning (only when checked AND the file exists)
			+ SVerticalBox::Slot().AutoHeight().Padding(24, 0, 0, 2)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Visibility_Lambda([this]() { return GetContextExistsVisibility(); })
				.ColorAndOpacity(FLinearColor(1.0f, 0.66f, 0.0f))
				.Text_Lambda([this]() { return GetContextExistsWarningText(); })
			]

			// Append / Replace selector (only when the file exists)
			+ SVerticalBox::Slot().AutoHeight().Padding(24, 0, 0, 12)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this]() { return GetContextExistsVisibility(); })

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SComboBox<TSharedPtr<EConvaiContextWriteMode>>)
					.OptionsSource(&ContextWriteModeOptions)
					.InitiallySelectedItem(ContextWriteModeOptions[0])
					.OnGenerateWidget_Lambda([](TSharedPtr<EConvaiContextWriteMode> Item)
					{
						return SNew(STextBlock).Text(*Item == EConvaiContextWriteMode::Append
							? LOCTEXT("ModeAppend", "Append (keep existing content)")
							: LOCTEXT("ModeReplace", "Replace (overwrite the file)"));
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<EConvaiContextWriteMode> Item, ESelectInfo::Type)
					{
						if (Item.IsValid()) { ContextWriteMode = *Item; }
					})
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return ContextWriteMode == EConvaiContextWriteMode::Append
								? LOCTEXT("ModeAppend", "Append (keep existing content)")
								: LOCTEXT("ModeReplace", "Replace (overwrite the file)");
						})
					]
				]
			]

			// Codex auto-approve (only shown when the selected agent is Codex)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SCheckBox)
				.Visibility_Lambda([this]() { return GetCodexAutoApproveVisibility(); })
				.IsChecked_Lambda([this]() { return bCodexAutoApprove ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bCodexAutoApprove = (S == ECheckBoxState::Checked); })
				[
					SNew(STextBlock)
					.WrapTextAt(440.0f)
					.Text(LOCTEXT("CodexAutoApprove", "Let Codex run unattended with no approval prompts (bypasses Codex's sandbox)"))
				]
			]

			// Warning shown only when Codex auto-approve is enabled
			+ SVerticalBox::Slot().AutoHeight().Padding(24, 0, 0, 2)
			[
				SNew(STextBlock)
				.WrapTextAt(440.0f)
				.Visibility_Lambda([this]()
				{
					return (GetCodexAutoApproveVisibility() == EVisibility::Visible && bCodexAutoApprove)
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				.ColorAndOpacity(FLinearColor(1.0f, 0.66f, 0.0f))
				.Text(LOCTEXT("CodexAutoApproveWarn", "Warning: this runs 'codex --dangerously-bypass-approvals-and-sandbox' - Codex executes every command with NO approval prompt and NO sandbox (full access to your machine). Only enable if you trust what the agent will run in this project."))
			]

			// Buttons
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 6, 0, 0)
			[
				SNew(SUniformGridPanel).SlotPadding(FMargin(6, 0, 0, 0))
				+ SUniformGridPanel::Slot(0, 0)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(LOCTEXT("Cancel", "Cancel"))
					.OnClicked(this, &SConvaiMcpSetupDialog::OnCancel)
				]
				+ SUniformGridPanel::Slot(1, 0)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.ButtonStyle(CONVAI_MCP_STYLE::Get(), "PrimaryButton")
					.Text(LOCTEXT("Enable", "Enable & Restart"))
					.OnClicked(this, &SConvaiMcpSetupDialog::OnConfirm)
				]
			]
		]
	];
}

bool SConvaiMcpSetupDialog::IsTerminalOptionEnabled() const
{
	return FConvaiMcpSetupService::IsCliAgent(SelectedAgent);
}

EVisibility SConvaiMcpSetupDialog::GetTerminalConflictVisibility() const
{
	const bool bShow = bConfigureTerminal && IsTerminalOptionEnabled() && FConvaiMcpSetupService::HasExistingTerminalStartupCommands();
	return bShow ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SConvaiMcpSetupDialog::GetCodexAutoApproveVisibility() const
{
	return SelectedAgent == EConvaiMcpAgent::Codex ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SConvaiMcpSetupDialog::GetAddInstructionsLabel() const
{
	return FText::Format(
		LOCTEXT("AddInstructionsFmt", "Add Convai instructions to the agent's context file ({0})"),
		FText::FromString(FConvaiMcpSetupService::GetAgentContextFile(SelectedAgent)));
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
		.Title(LOCTEXT("WindowTitle", "Convai — Set Up AI Coding (MCP)"))
		.SizingRule(ESizingRule::Autosized)
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
		OutOptions.bAddPrimer = Dialog->bAddPrimer;
		OutOptions.ContextWriteMode = Dialog->ContextWriteMode;
		OutOptions.bCodexAutoApprove = Dialog->bCodexAutoApprove && (Dialog->SelectedAgent == EConvaiMcpAgent::Codex);
		return true;
	}
	return false;
}

#undef LOCTEXT_NAMESPACE

#endif // CONVAI_MCP_SUPPORTED
