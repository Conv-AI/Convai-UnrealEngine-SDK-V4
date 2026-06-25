/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiMcpSetup.h
 *
 * Shared contract for the "Set Up AI Coding (MCP)" feature: version gate,
 * supported-agent enum, the user's chosen options, and the static service that
 * enables the engine MCP/Terminal plugins and finishes configuration after a
 * restart.
 *
 * IMPORTANT: This feature targets UE 5.8+ ONLY (the engine MCP/Terminal/Toolset
 * plugins do not exist before 5.8). Everything MCP-specific is compiled behind
 * CONVAI_MCP_SUPPORTED so the plugin still builds cleanly on 5.0-5.7. The code
 * takes NO build dependency on the MCP/Terminal modules: plugins are enabled via
 * IProjectManager, settings are written via GConfig, and the server / client
 * config are driven through the plugin's console commands (which only exist once
 * the plugin is enabled and loaded, i.e. after the restart).
 */

#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"

// 5.8+ only. UE_VERSION_OLDER_THAN handles the 6.0 edge case correctly
// (unlike the legacy `MAJOR>=5 && MINOR>=N` idiom used elsewhere).
#define CONVAI_MCP_SUPPORTED (!UE_VERSION_OLDER_THAN(5, 8, 0))

#if CONVAI_MCP_SUPPORTED

/** AI coding clients supported by the engine's Unreal MCP integration. */
enum class EConvaiMcpAgent : uint8
{
	ClaudeCode,
	Cursor,
	VSCode,
	Gemini,
	Codex
};

/** How to add the Convai instructions when the agent's context file already exists. */
enum class EConvaiContextWriteMode : uint8
{
	Append,  // Preserve existing content; upsert our sentinel-delimited block.
	Replace  // Overwrite the file with ONLY our content (still sentinel-wrapped).
};

/** Options captured from the setup dialog and persisted across the restart. */
struct FConvaiMcpSetupOptions
{
	EConvaiMcpAgent Agent = EConvaiMcpAgent::ClaudeCode;

	/** Persist UModelContextProtocolSettings.bAutoStartServer=true for future launches. */
	bool bAutoStartServer = true;

	/** Add a sentinel-delimited Convai block to the Terminal StartupCommands (CLI agents only). */
	bool bConfigureTerminal = false;

	/** Merge the Convai primer into the agent's auto-loaded context file. */
	bool bAddPrimer = false;

	/** When the context file already exists, append our block or replace the whole file. */
	EConvaiContextWriteMode ContextWriteMode = EConvaiContextWriteMode::Append;

	/** Codex only: launch with `--ask-for-approval never --sandbox workspace-write` so it runs
	 *  commands without prompting (sandboxed to the project). Ignored for non-Codex agents. */
	bool bCodexAutoApprove = false;
};

/**
 * Static service that drives the two-phase MCP setup.
 *
 * Phase 1 (click-time, EnableAndScheduleSetup): enable the required engine
 * plugins in the .uproject and persist the chosen options as a pending record.
 * The caller then restarts the editor.
 *
 * Phase 2 (next launch, ApplyPendingSetupIfAny — call from OnEditorInitialized):
 * once the MCP/Terminal modules are loaded, write the client config, optionally
 * configure the terminal + primer, persist auto-start, always start the server
 * once this session, then clear the pending record. Does NOT open a terminal.
 */
class CONVAIEDITOR_API FConvaiMcpSetupService
{
public:
	/** True on engines where the feature is available (5.8+). Compile-time. */
	static bool IsSupported() { return true; }

	/** Display label for an agent (e.g. "Claude Code"). */
	static FText GetAgentDisplayName(EConvaiMcpAgent Agent);

	/** Token passed to ModelContextProtocol.GenerateClientConfig (e.g. "ClaudeCode"). */
	static FString GetClientConfigToken(EConvaiMcpAgent Agent);

	/** CLI agents (Claude Code, Codex, Gemini) can be launched in a terminal; GUI ones (Cursor, VS Code) cannot. */
	static bool IsCliAgent(EConvaiMcpAgent Agent);

	/** The CLI executable name for a CLI agent (e.g. "claude"). Empty for GUI agents. */
	static FString GetCliCommand(EConvaiMcpAgent Agent);

	/** Project-root-relative context file an agent auto-loads (e.g. "CLAUDE.md"). */
	static FString GetAgentContextFile(EConvaiMcpAgent Agent);

	/** Absolute path to the agent's context file (ProjectDir + GetAgentContextFile). */
	static FString GetAgentContextFileAbsolutePath(EConvaiMcpAgent Agent);

	/** True if the agent's context file already exists on disk. */
	static bool DoesAgentContextFileExist(EConvaiMcpAgent Agent);

	/** True if all of ModelContextProtocol / AllToolsets / Terminal are enabled in the project. */
	static bool AreRequiredPluginsEnabled();

	/** Are the user's Terminal StartupCommands non-empty (used to warn about conflicts)? */
	static bool HasExistingTerminalStartupCommands();

	/**
	 * Phase 1: enable ModelContextProtocol, AllToolsets, Terminal in the project
	 * descriptor and persist the pending options. Returns false (with OutError)
	 * if plugin enabling fails. The caller restarts the editor afterward.
	 */
	static bool EnableAndScheduleSetup(const FConvaiMcpSetupOptions& Options, FString& OutError);

	/** True if a pending setup record is present. */
	static bool IsSetupPending();

	/**
	 * Phase 2: if a pending record exists and the MCP/Terminal modules are loaded,
	 * finish configuration and clear the record. If the modules are NOT loaded the
	 * record is left in place (sticky) so a later successful launch finishes it.
	 * Safe to call unconditionally from OnEditorInitialized.
	 */
	static void ApplyPendingSetupIfAny();

private:
	static bool ReadPending(FConvaiMcpSetupOptions& OutOptions);
	static void WritePending(const FConvaiMcpSetupOptions& Options);
	static void ClearPending();

	/** Apply the resolved options now (modules assumed loaded). */
	static void ApplyConfiguration(const FConvaiMcpSetupOptions& Options);

	static bool ConfigureTerminalStartupCommands(const FConvaiMcpSetupOptions& Options, FString& OutError);
	static bool WriteAgentPrimer(EConvaiMcpAgent Agent, EConvaiContextWriteMode WriteMode, FString& OutError);
	static void PersistAutoStartServer(bool bEnabled);
	static void StartServerOnce();
};

#endif // CONVAI_MCP_SUPPORTED
