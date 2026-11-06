/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiMcpSetupDialog.h
 *
 * Modal dialog for "Set Up AI Coding (MCP)": pick an agent + opt-in checkboxes
 * (auto-start server, configure terminal, add primer). UE 5.8+ only.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/Mcp/ConvaiMcpSetup.h"

#if CONVAI_MCP_SUPPORTED

#include "Widgets/SCompoundWidget.h"

class SWindow;
class SCheckBox;

/**
 * Blocking helper: shows the modal setup dialog. Returns true and fills
 * OutOptions if the user clicked "Enable & Restart"; false if cancelled.
 */
class CONVAIEDITOR_API SConvaiMcpSetupDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SConvaiMcpSetupDialog) {}
		SLATE_ARGUMENT(TWeakPtr<SWindow>, ParentWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows the dialog modally. Returns true if confirmed. */
	static bool ShowDialog(FConvaiMcpSetupOptions& OutOptions);

	/** Shared end-to-end setup entry point used by both the Convai shell and Auto Tagger. */
	static bool RunSetupFlow();

private:
	TWeakPtr<SWindow> ParentWindow;

	EConvaiMcpAgent SelectedAgent = EConvaiMcpAgent::ClaudeCode;
	bool bAutoStartServer = true;
	bool bConfigureTerminal = false;
	bool bLaunchCodexWithFullPermissions = false;
	bool bAddPrimer = false;
	EConvaiContextWriteMode ContextWriteMode = EConvaiContextWriteMode::Append;
	bool bConfirmed = false;
	bool bHasCurrentAgent = false;
	bool bSetupPending = false;
	bool bSelectedCliFound = false;
	bool bCliPathChangedSinceDisplay = false;
	EConvaiMcpAgent CurrentAgent = EConvaiMcpAgent::ClaudeCode;
	FString SelectedCliExecutablePath;

	TArray<TSharedPtr<EConvaiMcpAgent>> AgentOptions;
	TArray<TSharedPtr<EConvaiContextWriteMode>> ContextWriteModeOptions;

	bool IsTerminalOptionEnabled() const;   // CLI agents only
	EVisibility GetTerminalOptionVisibility() const;
	EVisibility GetTerminalConflictVisibility() const;
	EVisibility GetFullPermissionsVisibility() const;

	/** Visible only when the instructions checkbox is checked AND the context file already exists. */
	EVisibility GetContextExistsVisibility() const;
	FText GetContextExistsWarningText() const;
	FText GetAddInstructionsLabel() const;
	FText GetCurrentAgentText() const;
	FText GetAgentDescription() const;
	FText GetCliStatusTitle() const;
	FText GetCliStatusDetail() const;
	FText GetCliStatusToolTip() const;
	FSlateColor GetCliStatusColor() const;
	EVisibility GetInstallButtonVisibility() const;
	EVisibility GetRecheckButtonVisibility() const;
	FText GetInstallButtonText() const;
	FText GetConfirmButtonText() const;
	bool CanConfirm() const;
	FReply OnOpenInstallPage();
	FReply OnRecheckCli();
	void RefreshSelectedCli();

	FReply OnConfirm();
	FReply OnCancel();
	void CloseParent();
};

#endif // CONVAI_MCP_SUPPORTED
