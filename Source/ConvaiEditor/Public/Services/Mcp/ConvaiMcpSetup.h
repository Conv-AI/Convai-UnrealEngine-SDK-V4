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

	/**
	 * Launch Codex from Unreal's interactive terminal without approval prompts or
	 * sandboxing. Explicit dangerous opt-in; never used by Auto Tagger processes.
	 */
	bool bLaunchCodexWithFullPermissions = false;

	/** Merge the Convai primer into the agent's auto-loaded context file. */
	bool bAddPrimer = false;

	/** When the context file already exists, append our block or replace the whole file. */
	EConvaiContextWriteMode ContextWriteMode = EConvaiContextWriteMode::Append;

	/**
	 * Exact CLI path displayed and confirmed in the setup dialog. This is consumed
	 * at click time and deliberately is not copied into the restart transaction;
	 * supported vision-CLI approval is persisted separately with its executable identity.
	 */
	FString ConfirmedCliExecutablePath;

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

	/** True when the agent has a bounded, non-interactive image workflow for editor automation. */
	static bool SupportsVisionCli(EConvaiMcpAgent Agent);

	/** The CLI executable name for a CLI agent (e.g. "claude"). Empty for GUI agents. */
	static FString GetCliCommand(EConvaiMcpAgent Agent);

	/** Official install/setup page for the selected client. */
	static FString GetAgentInstallUrl(EConvaiMcpAgent Agent);

	/**
	 * Resolve the selected CLI from PATH. Codex and Claude Code npm shims are
	 * resolved to their package-native executables; the mutable shim itself is never
	 * returned. Project-local executables and reparse-point paths are ignored.
	 */
	static bool FindCliExecutable(EConvaiMcpAgent Agent, FString& OutExecutablePath);

	/**
	 * Stream and hash an approved native CLI executable. The returned value is a
	 * versioned cryptographic identity (currently `blake3:<64 hex digits>`).
	 */
	static bool ComputeCliExecutableIdentity(
		const FString& ExecutablePath,
		FString& OutIdentity,
		FString& OutError);

	/** Durable project preference written after setup scheduling succeeds. */
	static bool TryGetPreferredAgent(EConvaiMcpAgent& OutAgent);

	/**
	 * Whether the preferred agent is ready for the Auto Tagger's CLI image
	 * workflow. When requested, returns the identity that must be re-hashed and
	 * compared immediately before process creation.
	 */
	static bool IsPreferredVisionCliReady(
		EConvaiMcpAgent& OutAgent,
		FString& OutExecutablePath,
		FText& OutDetail,
		FString* OutApprovedIdentity = nullptr);

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

	/** True when the current Terminal StartupCommands contain Convai's complete block for this agent. */
	static bool IsTerminalConfiguredForAgent(EConvaiMcpAgent Agent);

	/** True only when the managed Codex terminal command uses the explicit full-permissions flag. */
	static bool IsTerminalConfiguredWithFullPermissions(EConvaiMcpAgent Agent);

	/**
	 * Phase 1: enable ModelContextProtocol, AllToolsets, Terminal in the project
	 * descriptor, persist the selected agent, and apply immediately when the modules
	 * are already loaded. Otherwise the options remain pending for the next restart.
	 */
	static bool EnableAndScheduleSetup(
		const FConvaiMcpSetupOptions& Options,
		FString& OutError,
		bool* bOutRestartRequired = nullptr);

	/** True if a pending setup record is present. */
	static bool IsSetupPending();

	/**
	 * Phase 2: if a pending record exists and the MCP/Terminal modules are loaded,
	 * finish configuration and clear the record. If the modules are NOT loaded the
	 * record is left in place (sticky) so a later successful launch finishes it.
	 * Safe to call unconditionally from OnEditorInitialized.
	 */
	static bool ApplyPendingSetupIfAny(FString* OutError = nullptr);

private:
	static bool ReadPending(FConvaiMcpSetupOptions& OutOptions, FString* OutError = nullptr);
	static bool WritePending(const FConvaiMcpSetupOptions& Options, FString& OutError);
	static bool ClearPending(FString& OutError);

	/** Apply the resolved options now (modules assumed loaded). */
	static bool ApplyConfiguration(const FConvaiMcpSetupOptions& Options, FString& OutError);

	static bool ReconcileTerminalStartupCommands(const FConvaiMcpSetupOptions& Options, FString& OutError);
	static bool WriteAgentPrimer(EConvaiMcpAgent Agent, EConvaiContextWriteMode WriteMode, FString& OutError);
	static void PersistAutoStartServer(bool bEnabled);
	static void StartServerOnce();
};

#endif // CONVAI_MCP_SUPPORTED
