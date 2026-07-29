/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiMcpSetupService.cpp
 *
 * Implementation of the two-phase "Set Up AI Coding (MCP)" flow. See
 * ConvaiMcpSetup.h. All MCP-specific code is gated behind CONVAI_MCP_SUPPORTED
 * (5.8+); on older engines this translation unit is empty.
 */

#include "Services/Mcp/ConvaiMcpSetup.h"

#if CONVAI_MCP_SUPPORTED

#include "ConvaiEditor.h" // LogConvaiEditor
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Modules/ModuleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "HAL/PlatformFileManager.h"
#include "Engine/Engine.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "ConvaiMcpSetup"

namespace ConvaiMcp
{
	// Pending-record store: project-local EditorPerProjectUserSettings ini.
	static const TCHAR* PendingSection = TEXT("ConvaiMcpSetup");

	// Engine MCP/Terminal settings sections. Pinned to UE 5.8 — if a future
	// engine renames these, update here (only reached under CONVAI_MCP_SUPPORTED).
	static const TCHAR* McpSettingsSection = TEXT("/Script/ModelContextProtocolEngine.ModelContextProtocolSettings");
	static const TCHAR* TerminalSettingsSection = TEXT("/Script/Terminal.TerminalSettings");

	static const TCHAR* RequiredPlugins[] = { TEXT("ModelContextProtocol"), TEXT("AllToolsets"), TEXT("Terminal") };

	// Legacy terminal sentinel lines we used to emit around our managed block. We no
	// longer write these (they were noise in the terminal), but we still recognize and
	// strip any pre-existing ones so an old block gets cleaned up on the next apply.
	static const TCHAR* LegacyTermSentinelBegin = TEXT(":: >>> Convai MCP setup");
	static const TCHAR* LegacyTermSentinelEnd = TEXT(":: <<< Convai MCP setup");

	// Primer sentinels still delimit our managed block inside the agent context file.
	static const TCHAR* PrimerSentinelBegin = TEXT("<!-- >>> Convai primer (managed by Convai; edits between sentinels are overwritten) -->");
	static const TCHAR* PrimerSentinelEnd = TEXT("<!-- <<< Convai primer -->");

	// The exact Codex launch line we emit when auto-approve is enabled. Kept here so the
	// dedup matcher and the line we actually write stay in sync.
	static FString CodexAutoApproveLaunchLine()
	{
		// `--ask-for-approval never` alone still leaves Codex prompting (folder-trust + escalation),
		// which defeats the auto-approve opt-in. This flag skips ALL confirmation prompts so the agent
		// runs unattended. The user explicitly opts into this via the dialog checkbox.
		return TEXT("codex --dangerously-bypass-approvals-and-sandbox");
	}

	// Older auto-approve launch line we used to emit; recognized so re-running setup cleans it up
	// instead of leaving a stale duplicate alongside the new line.
	static const TCHAR* LegacyCodexAutoApproveLaunchLine = TEXT("codex --ask-for-approval never --sandbox workspace-write");

	// True if Line is one of OUR managed terminal StartupCommands entries for ProjDir:
	// the TERM line, our exact project cd, an EXACT managed CLI launch line we emit, or a
	// leftover legacy sentinel. Matched EXACTLY (never a prefix) so a user's own commands
	// like "codex login" or "codex --model ..." are preserved, never silently removed.
	static bool IsManagedTerminalLine(const FString& Line, const FString& ProjDir)
	{
		if (Line.Equals(TEXT("set TERM=xterm-256color")))
		{
			return true;
		}
		if (Line.Equals(FString::Printf(TEXT("cd /d \"%s\""), *ProjDir)))
		{
			return true;
		}
		// Only the exact launch lines we ourselves write (plain CLI, or the flagged Codex variant).
		if (Line.Equals(TEXT("claude")) || Line.Equals(TEXT("codex")) || Line.Equals(TEXT("gemini"))
			|| Line.Equals(CodexAutoApproveLaunchLine()) || Line.Equals(LegacyCodexAutoApproveLaunchLine))
		{
			return true;
		}
		if (Line.Equals(LegacyTermSentinelBegin) || Line.Equals(LegacyTermSentinelEnd))
		{
			return true;
		}
		return false;
	}

	static FString AbsoluteProjectDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	}

	// Reload a settings class's live CDO from config so the CURRENT session picks up
	// the values we just wrote via GConfig (otherwise the writes only take effect on a
	// *subsequent* restart — the "terminal commands not added on restart" bug).
	static void ReloadSettingsCdo(const TCHAR* ClassPath)
	{
		if (UClass* C = FindObject<UClass>(nullptr, ClassPath))
		{
			if (UObject* CDO = C->GetDefaultObject())
			{
				CDO->ReloadConfig();
			}
		}
	}

	static void Toast(const FText& Message, bool bSuccess)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 8.0f;
		Info.bFireAndForget = true;
		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	/** Replace (or append) a sentinel-delimited block inside Body. Returns the new text. */
	static FString UpsertSentinelBlock(const FString& Body, const FString& Begin, const FString& End, const FString& BlockBody)
	{
		const FString NewBlock = Begin + LINE_TERMINATOR + BlockBody + LINE_TERMINATOR + End;

		int32 BeginIdx = Body.Find(Begin, ESearchCase::CaseSensitive);
		if (BeginIdx != INDEX_NONE)
		{
			int32 EndIdx = Body.Find(End, ESearchCase::CaseSensitive, ESearchDir::FromStart, BeginIdx);
			if (EndIdx != INDEX_NONE)
			{
				const int32 EndStop = EndIdx + End.Len();
				FString Result = Body.Left(BeginIdx) + NewBlock + Body.Mid(EndStop);
				return Result;
			}
		}

		// Append (preserving any existing user content).
		FString Result = Body;
		if (!Result.IsEmpty() && !Result.EndsWith(LINE_TERMINATOR))
		{
			Result += LINE_TERMINATOR;
		}
		if (!Result.IsEmpty())
		{
			Result += LINE_TERMINATOR;
		}
		Result += NewBlock + LINE_TERMINATOR;
		return Result;
	}
}

FText FConvaiMcpSetupService::GetAgentDisplayName(EConvaiMcpAgent Agent)
{
	switch (Agent)
	{
	case EConvaiMcpAgent::ClaudeCode: return LOCTEXT("AgentClaude", "Claude Code");
	case EConvaiMcpAgent::Cursor:     return LOCTEXT("AgentCursor", "Cursor");
	case EConvaiMcpAgent::VSCode:     return LOCTEXT("AgentVSCode", "VS Code");
	case EConvaiMcpAgent::Gemini:     return LOCTEXT("AgentGemini", "Gemini");
	case EConvaiMcpAgent::Codex:      return LOCTEXT("AgentCodex", "Codex");
	default:                          return LOCTEXT("AgentUnknown", "Unknown");
	}
}

FString FConvaiMcpSetupService::GetClientConfigToken(EConvaiMcpAgent Agent)
{
	switch (Agent)
	{
	case EConvaiMcpAgent::ClaudeCode: return TEXT("ClaudeCode");
	case EConvaiMcpAgent::Cursor:     return TEXT("Cursor");
	case EConvaiMcpAgent::VSCode:     return TEXT("VSCode");
	case EConvaiMcpAgent::Gemini:     return TEXT("Gemini");
	case EConvaiMcpAgent::Codex:      return TEXT("Codex");
	default:                          return TEXT("ClaudeCode");
	}
}

bool FConvaiMcpSetupService::IsCliAgent(EConvaiMcpAgent Agent)
{
	return Agent == EConvaiMcpAgent::ClaudeCode || Agent == EConvaiMcpAgent::Codex || Agent == EConvaiMcpAgent::Gemini;
}

FString FConvaiMcpSetupService::GetCliCommand(EConvaiMcpAgent Agent)
{
	switch (Agent)
	{
	case EConvaiMcpAgent::ClaudeCode: return TEXT("claude");
	case EConvaiMcpAgent::Codex:      return TEXT("codex");
	case EConvaiMcpAgent::Gemini:     return TEXT("gemini");
	default:                          return FString();
	}
}

FString FConvaiMcpSetupService::GetAgentContextFile(EConvaiMcpAgent Agent)
{
	switch (Agent)
	{
	case EConvaiMcpAgent::ClaudeCode: return TEXT("CLAUDE.md");
	case EConvaiMcpAgent::Codex:      return TEXT("AGENTS.md");
	case EConvaiMcpAgent::Gemini:     return TEXT("GEMINI.md");
	case EConvaiMcpAgent::Cursor:     return TEXT(".cursor/rules/convai.mdc");
	case EConvaiMcpAgent::VSCode:     return TEXT(".github/copilot-instructions.md");
	default:                          return TEXT("CLAUDE.md");
	}
}

FString FConvaiMcpSetupService::GetAgentContextFileAbsolutePath(EConvaiMcpAgent Agent)
{
	return FPaths::Combine(ConvaiMcp::AbsoluteProjectDir(), GetAgentContextFile(Agent));
}

bool FConvaiMcpSetupService::DoesAgentContextFileExist(EConvaiMcpAgent Agent)
{
	return FPaths::FileExists(GetAgentContextFileAbsolutePath(Agent));
}

bool FConvaiMcpSetupService::AreRequiredPluginsEnabled()
{
	for (const TCHAR* Name : ConvaiMcp::RequiredPlugins)
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
		if (!Plugin.IsValid() || !Plugin->IsEnabled())
		{
			return false;
		}
	}
	return true;
}

bool FConvaiMcpSetupService::HasExistingTerminalStartupCommands()
{
	if (!GConfig)
	{
		return false;
	}
	TArray<FString> Existing;
	GConfig->GetArray(ConvaiMcp::TerminalSettingsSection, TEXT("StartupCommands"), Existing, GEditorPerProjectIni);

	FString ProjDir = ConvaiMcp::AbsoluteProjectDir();
	FPaths::MakePlatformFilename(ProjDir);
	ProjDir.RemoveFromEnd(TEXT("\\"));

	// A conflict is any non-empty entry that ISN'T one of our managed lines (TERM,
	// our project cd, a known CLI command, or a leftover ":: " sentinel).
	for (const FString& Line : Existing)
	{
		if (Line.TrimStartAndEnd().IsEmpty())
		{
			continue;
		}
		if (!ConvaiMcp::IsManagedTerminalLine(Line, ProjDir))
		{
			return true;
		}
	}
	return false;
}

bool FConvaiMcpSetupService::EnableAndScheduleSetup(const FConvaiMcpSetupOptions& Options, FString& OutError)
{
	IProjectManager& PM = IProjectManager::Get();

	for (const TCHAR* Name : ConvaiMcp::RequiredPlugins)
	{
		FText FailReason;
		if (!PM.SetPluginEnabled(FString(Name), true, FailReason))
		{
			OutError = FString::Printf(TEXT("Failed to enable plugin '%s': %s"), Name, *FailReason.ToString());
			return false;
		}
	}

	if (PM.IsCurrentProjectDirty())
	{
		FText FailReason;
		if (!PM.SaveCurrentProjectToDisk(FailReason))
		{
			OutError = FString::Printf(TEXT("Failed to save project descriptor: %s"), *FailReason.ToString());
			return false;
		}
	}

	WritePending(Options);
	UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiMcp: plugins enabled, setup scheduled for next restart (agent=%s)."), *GetClientConfigToken(Options.Agent));
	return true;
}

bool FConvaiMcpSetupService::IsSetupPending()
{
	FConvaiMcpSetupOptions Unused;
	return ReadPending(Unused);
}

void FConvaiMcpSetupService::WritePending(const FConvaiMcpSetupOptions& Options)
{
	if (!GConfig)
	{
		return;
	}
	GConfig->SetBool(ConvaiMcp::PendingSection, TEXT("bPending"), true, GEditorPerProjectIni);
	GConfig->SetInt(ConvaiMcp::PendingSection, TEXT("Agent"), (int32)Options.Agent, GEditorPerProjectIni);
	GConfig->SetBool(ConvaiMcp::PendingSection, TEXT("bAutoStartServer"), Options.bAutoStartServer, GEditorPerProjectIni);
	GConfig->SetBool(ConvaiMcp::PendingSection, TEXT("bConfigureTerminal"), Options.bConfigureTerminal, GEditorPerProjectIni);
	GConfig->SetBool(ConvaiMcp::PendingSection, TEXT("bAddPrimer"), Options.bAddPrimer, GEditorPerProjectIni);
	GConfig->SetInt(ConvaiMcp::PendingSection, TEXT("ContextWriteMode"), (int32)Options.ContextWriteMode, GEditorPerProjectIni);
	GConfig->SetBool(ConvaiMcp::PendingSection, TEXT("bCodexAutoApprove"), Options.bCodexAutoApprove, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

bool FConvaiMcpSetupService::ReadPending(FConvaiMcpSetupOptions& OutOptions)
{
	if (!GConfig)
	{
		return false;
	}
	bool bPending = false;
	if (!GConfig->GetBool(ConvaiMcp::PendingSection, TEXT("bPending"), bPending, GEditorPerProjectIni) || !bPending)
	{
		return false;
	}
	int32 AgentInt = (int32)EConvaiMcpAgent::ClaudeCode;
	GConfig->GetInt(ConvaiMcp::PendingSection, TEXT("Agent"), AgentInt, GEditorPerProjectIni);
	OutOptions.Agent = (EConvaiMcpAgent)FMath::Clamp(AgentInt, 0, (int32)EConvaiMcpAgent::Codex);
	GConfig->GetBool(ConvaiMcp::PendingSection, TEXT("bAutoStartServer"), OutOptions.bAutoStartServer, GEditorPerProjectIni);
	GConfig->GetBool(ConvaiMcp::PendingSection, TEXT("bConfigureTerminal"), OutOptions.bConfigureTerminal, GEditorPerProjectIni);
	GConfig->GetBool(ConvaiMcp::PendingSection, TEXT("bAddPrimer"), OutOptions.bAddPrimer, GEditorPerProjectIni);
	int32 WriteModeInt = (int32)EConvaiContextWriteMode::Append;
	GConfig->GetInt(ConvaiMcp::PendingSection, TEXT("ContextWriteMode"), WriteModeInt, GEditorPerProjectIni);
	OutOptions.ContextWriteMode = (EConvaiContextWriteMode)FMath::Clamp(WriteModeInt, 0, (int32)EConvaiContextWriteMode::Replace);
	GConfig->GetBool(ConvaiMcp::PendingSection, TEXT("bCodexAutoApprove"), OutOptions.bCodexAutoApprove, GEditorPerProjectIni);
	return true;
}

void FConvaiMcpSetupService::ClearPending()
{
	if (!GConfig)
	{
		return;
	}
	GConfig->EmptySection(ConvaiMcp::PendingSection, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void FConvaiMcpSetupService::ApplyPendingSetupIfAny()
{
	FConvaiMcpSetupOptions Options;
	if (!ReadPending(Options))
	{
		return;
	}

	// Guard: the MCP/Terminal modules must be loaded before we touch their
	// settings / console commands. If not, leave the record sticky for a later launch.
	const bool bMcpLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("ModelContextProtocolEngine"));
	const bool bTerminalLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("Terminal"));
	if (!bMcpLoaded)
	{
		UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: setup pending but ModelContextProtocol not loaded; leaving pending for a later restart."));
		ConvaiMcp::Toast(LOCTEXT("McpNotLoaded", "Convai MCP setup is pending but the MCP plugin isn't loaded yet. It will finish on the next restart."), false);
		return;
	}

	ApplyConfiguration(Options);
	ClearPending();
}

void FConvaiMcpSetupService::ApplyConfiguration(const FConvaiMcpSetupOptions& Options)
{
	FString Summary;

	// (a) Always: write the chosen agent's MCP client config (project root on installed engines).
	// Codex client config is WRITE-ONCE: the engine refuses GenerateClientConfig if
	// <ProjectDir>/.codex/config.toml already exists, and the Exec gives no success signal.
	// So for Codex, detect an existing config first and inform rather than claim success.
	bool bCodexConfigExisted = false;
	if (Options.Agent == EConvaiMcpAgent::Codex)
	{
		const FString CodexConfigPath = FPaths::Combine(ConvaiMcp::AbsoluteProjectDir(), TEXT(".codex"), TEXT("config.toml"));
		bCodexConfigExisted = FPaths::FileExists(CodexConfigPath);
	}

	if (bCodexConfigExisted)
	{
		Summary += TEXT("Codex config already existed - left untouched; add the unreal MCP server to it manually (http://127.0.0.1:8000/mcp). ");
	}
	else
	{
		if (GEngine)
		{
			const FString Cmd = FString::Printf(TEXT("ModelContextProtocol.GenerateClientConfig %s"), *GetClientConfigToken(Options.Agent));
			GEngine->Exec(nullptr, *Cmd);
		}
		Summary += FString::Printf(TEXT("Client config for %s written. "), *GetAgentDisplayName(Options.Agent).ToString());
	}

	// (b) Terminal (CLI agents only). Skip (don't fail) if the Terminal module isn't loaded.
	if (Options.bConfigureTerminal && IsCliAgent(Options.Agent))
	{
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("Terminal")))
		{
			Summary += TEXT("Terminal not loaded - skipped terminal setup. ");
			UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: Terminal module not loaded; skipping terminal startup config."));
		}
		else
		{
			FString Err;
			if (ConfigureTerminalStartupCommands(Options, Err))
			{
				Summary += TEXT("Terminal configured. ");
			}
			else
			{
				UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: terminal config failed: %s"), *Err);
			}
		}
	}

	// (c) Convai instructions context file.
	if (Options.bAddPrimer)
	{
		FString Err;
		if (WriteAgentPrimer(Options.Agent, Options.ContextWriteMode, Err))
		{
			Summary += TEXT("Convai instructions added. ");
		}
		else
		{
			UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: primer write failed: %s"), *Err);
		}
	}

	// (d) Persist the chosen auto-start value for future launches (true OR false, so
	// unchecking on a re-run disables a previously-enabled auto-start).
	PersistAutoStartServer(Options.bAutoStartServer);

	// Always max out the Terminal scrollback (the Terminal plugin is always enabled here).
	// 1000000 is the engine's ClampMax; the default of 131072 is too small for long agent runs.
	// Written before the ReloadSettingsCdo below so it applies live this session.
	if (GConfig)
	{
		GConfig->SetInt(ConvaiMcp::TerminalSettingsSection, TEXT("ScrollbackLimit"), 1000000, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	// Reload the live settings CDOs so THIS session uses the values we just wrote to
	// GConfig (Terminal StartupCommands + MCP bAutoStartServer). Without this the writes
	// only take effect after another restart (the terminal-commands-on-restart bug).
	ConvaiMcp::ReloadSettingsCdo(ConvaiMcp::TerminalSettingsSection);
	ConvaiMcp::ReloadSettingsCdo(ConvaiMcp::McpSettingsSection);

	// (e) Always start the server once this session (independent of the checkbox).
	StartServerOnce();

	// Summary toast incl. the exact command to run the agent in a terminal.
	FString HowTo;
	if (IsCliAgent(Options.Agent))
	{
		HowTo = FString::Printf(TEXT("Open a terminal in the project folder and run `%s`."), *GetCliCommand(Options.Agent));
	}
	else
	{
		HowTo = FString::Printf(TEXT("Open %s on this project folder; it will pick up the MCP config."), *GetAgentDisplayName(Options.Agent).ToString());
	}
	ConvaiMcp::Toast(FText::FromString(FString::Printf(TEXT("Convai MCP ready. %s\n%s"), *Summary, *HowTo)), true);
	UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiMcp: setup applied (%s)"), *Summary);
}

bool FConvaiMcpSetupService::ConfigureTerminalStartupCommands(const FConvaiMcpSetupOptions& Options, FString& OutError)
{
	const EConvaiMcpAgent Agent = Options.Agent;
	if (!GConfig)
	{
		OutError = TEXT("GConfig unavailable");
		return false;
	}

	TArray<FString> Commands;
	GConfig->GetArray(ConvaiMcp::TerminalSettingsSection, TEXT("StartupCommands"), Commands, GEditorPerProjectIni);

	FString ProjDir = ConvaiMcp::AbsoluteProjectDir();
	FPaths::MakePlatformFilename(ProjDir);
	ProjDir.RemoveFromEnd(TEXT("\\"));

	// Idempotency: drop any of OUR managed lines (the TERM line, our exact project cd,
	// a known CLI command, or a leftover ":: " sentinel from an older version) while
	// preserving every other user command in order. Then append a fresh block.
	TArray<FString> Cleaned;
	Cleaned.Reserve(Commands.Num());
	for (const FString& Line : Commands)
	{
		if (!ConvaiMcp::IsManagedTerminalLine(Line, ProjDir))
		{
			Cleaned.Add(Line);
		}
	}

	// Append the functional commands only — no comment sentinels (Windows/cmd.exe).
	Cleaned.Add(TEXT("set TERM=xterm-256color"));
	Cleaned.Add(FString::Printf(TEXT("cd /d \"%s\""), *ProjDir));

	// Codex with auto-approve runs without prompting, sandboxed to the workspace.
	FString LaunchLine = GetCliCommand(Agent);
	if (Agent == EConvaiMcpAgent::Codex && Options.bCodexAutoApprove)
	{
		LaunchLine = ConvaiMcp::CodexAutoApproveLaunchLine();
	}
	Cleaned.Add(LaunchLine);

	GConfig->SetArray(ConvaiMcp::TerminalSettingsSection, TEXT("StartupCommands"), Cleaned, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
	return true;
}

bool FConvaiMcpSetupService::WriteAgentPrimer(EConvaiMcpAgent Agent, EConvaiContextWriteMode WriteMode, FString& OutError)
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Convai"));
	if (!Plugin.IsValid())
	{
		OutError = TEXT("Convai plugin not found");
		return false;
	}

	const FString SourcePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("AgentPrimer"), TEXT("ConvaiAgentPrimer.md"));
	FString PrimerBody;
	if (!FFileHelper::LoadFileToString(PrimerBody, *SourcePath))
	{
		OutError = FString::Printf(TEXT("Could not read primer source at %s"), *SourcePath);
		return false;
	}

	const FString TargetPath = FPaths::Combine(ConvaiMcp::AbsoluteProjectDir(), GetAgentContextFile(Agent));

	// Ensure parent dir exists (.cursor/rules, .github).
	const FString TargetDir = FPaths::GetPath(TargetPath);
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*TargetDir))
	{
		PF.CreateDirectoryTree(*TargetDir);
	}

	// Read the existing context file if present. Distinguish missing (ok -> empty) from
	// unreadable (e.g. locked): if it exists but can't be read, bail instead of clobbering.
	// This guard applies in BOTH modes — Replace must never overwrite a file it can't read.
	const bool bExists = FPaths::FileExists(TargetPath);
	FString Existing;
	if (bExists)
	{
		if (!FFileHelper::LoadFileToString(Existing, *TargetPath))
		{
			OutError = FString::Printf(TEXT("Existing context file is unreadable; refusing to overwrite %s"), *TargetPath);
			return false;
		}
	}

	// Replace mode (only meaningful when the file exists) starts from an empty body so the
	// merge produces ONLY our sentinel-wrapped block. Append mode preserves existing content.
	// A missing file behaves like Append from empty in both cases.
	FString Body = (WriteMode == EConvaiContextWriteMode::Replace) ? FString() : Existing;

	// Cursor .mdc wants frontmatter when the file is new OR when we're replacing it.
	if (Agent == EConvaiMcpAgent::Cursor && Body.IsEmpty())
	{
		Body = TEXT("---") LINE_TERMINATOR
			   TEXT("description: Convai project primer") LINE_TERMINATOR
			   TEXT("alwaysApply: true") LINE_TERMINATOR
			   TEXT("---") LINE_TERMINATOR;
	}

	const FString Merged = ConvaiMcp::UpsertSentinelBlock(Body, ConvaiMcp::PrimerSentinelBegin, ConvaiMcp::PrimerSentinelEnd, PrimerBody);

	// Append-only idempotency: if our up-to-date block is already present and the merged
	// result is byte-for-byte identical to what's on disk, skip the write so we don't
	// needlessly touch the file. Replace mode intentionally always overwrites.
	if (WriteMode != EConvaiContextWriteMode::Replace && bExists && Merged.Equals(Existing, ESearchCase::CaseSensitive))
	{
		UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiMcp: Convai instructions already up to date; skipping write of %s"), *TargetPath);
		return true;
	}

	if (!FFileHelper::SaveStringToFile(Merged, *TargetPath, FFileHelper::EEncodingOptions::ForceUTF8))
	{
		OutError = FString::Printf(TEXT("Could not write %s"), *TargetPath);
		return false;
	}
	return true;
}

void FConvaiMcpSetupService::PersistAutoStartServer(bool bEnabled)
{
	if (!GConfig)
	{
		return;
	}
	GConfig->SetBool(ConvaiMcp::McpSettingsSection, TEXT("bAutoStartServer"), bEnabled, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void FConvaiMcpSetupService::StartServerOnce()
{
	if (GEngine)
	{
		GEngine->Exec(nullptr, TEXT("ModelContextProtocol.StartServer"));
	}
}

#undef LOCTEXT_NAMESPACE

#endif // CONVAI_MCP_SUPPORTED
