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
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Hash/Blake3.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Engine.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "ConvaiMcpSetup"

namespace ConvaiMcp
{
	// This file is deliberately not registered with GConfig. Keeping consent,
	// pending options, and executable approval in a bounded standalone file under
	// LocalAppData prevents project-supplied Default*.ini layers from manufacturing
	// setup state.
	static const TCHAR* UserStateSection = TEXT("ConvaiMcpSetup");
	static const TCHAR* LegacyPendingSection = TEXT("ConvaiMcpSetup");
	static constexpr int32 PendingVersion = 2;
	static constexpr int32 PreferenceVersion = 3;
	static constexpr int64 MaxUserStateBytes = 64 * 1024;
	static constexpr int64 MaxClientConfigBytes = 1024 * 1024;
	static constexpr int64 HashBufferBytes = 1024 * 1024;

	struct FUserState
	{
		bool bHasPreferredAgent = false;
		EConvaiMcpAgent PreferredAgent = EConvaiMcpAgent::ClaudeCode;
		bool bPending = false;
		FString PendingAuthorization;
		FConvaiMcpSetupOptions PendingOptions;
		FString ApprovedCliPath;
		FString ApprovedCliIdentity;

		bool Equals(const FUserState& Other) const
		{
			return bHasPreferredAgent == Other.bHasPreferredAgent
				&& (!bHasPreferredAgent || PreferredAgent == Other.PreferredAgent)
				&& bPending == Other.bPending
				&& (!bPending || (
					PendingAuthorization == Other.PendingAuthorization
					&& PendingOptions.Agent == Other.PendingOptions.Agent
					&& PendingOptions.bAutoStartServer == Other.PendingOptions.bAutoStartServer
					&& PendingOptions.bConfigureTerminal == Other.PendingOptions.bConfigureTerminal
					&& PendingOptions.bLaunchCodexWithFullPermissions == Other.PendingOptions.bLaunchCodexWithFullPermissions
					&& PendingOptions.bAddPrimer == Other.PendingOptions.bAddPrimer
					&& PendingOptions.ContextWriteMode == Other.PendingOptions.ContextWriteMode))
				&& ApprovedCliPath == Other.ApprovedCliPath
				&& ApprovedCliIdentity == Other.ApprovedCliIdentity;
		}
	};

	enum class ERawPendingState : uint8
	{
		None,
		Pending,
		Invalid
	};

	struct FClientConfigDescriptor
	{
		FString RelativePath;
		FString ServersRootKey;
		FString UrlFieldName;
		bool bIncludeTypeField = false;
		bool bToml = false;
	};

	static FString ProjectStateKey()
	{
		FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(ProjectPath);
#if PLATFORM_WINDOWS
		ProjectPath.ToLowerInline();
#endif
		const FTCHARToUTF8 Utf8(*ProjectPath);
		return LexToString(FBlake3::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()))).ToLower();
	}

	static FString LegacyUserStateSection()
	{
		FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(ProjectPath);
		ProjectPath.ToLowerInline();
		const FTCHARToUTF8 Utf8(*ProjectPath);
		const FString LegacyHash = FSHA1::HashBuffer(
			Utf8.Get(), static_cast<uint64>(Utf8.Length())).ToString().Left(20);
		return FString::Printf(TEXT("ConvaiMcpSetup.Project.%s"), *LegacyHash);
	}

	static FString UserSettingsRoot()
	{
		const FString UserSettingsDirectory = FPlatformProcess::UserSettingsDir();
		if (UserSettingsDirectory.IsEmpty() || FPaths::IsRelative(UserSettingsDirectory))
		{
			return FString();
		}
		FString Root = FPaths::ConvertRelativePathToFull(UserSettingsDirectory);
		FPaths::NormalizeDirectoryName(Root);
		return Root;
	}

	static FString UserStateRoot()
	{
		const FString SettingsRoot = UserSettingsRoot();
		if (SettingsRoot.IsEmpty())
		{
			return FString();
		}
		FString Root = FPaths::Combine(
			SettingsRoot,
			TEXT("Convai"),
			TEXT("UnrealEditor"),
			TEXT("McpSetup"));
		FPaths::NormalizeDirectoryName(Root);
		FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(ProjectRoot);
		if (Root.Equals(ProjectRoot, ESearchCase::IgnoreCase)
			|| FPaths::IsUnderDirectory(Root, ProjectRoot))
		{
			return FString();
		}
		return Root;
	}

	static FString UserStatePath()
	{
		const FString Root = UserStateRoot();
		if (Root.IsEmpty())
		{
			return FString();
		}
		FString Path = FPaths::Combine(Root, ProjectStateKey() + TEXT(".ini"));
		FPaths::NormalizeFilename(Path);
		return Path;
	}

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

	// Explicitly selected only by the warned, twice-confirmed full-permissions option in the setup UI.
	// This command is for Unreal's interactive terminal and is never reused by Auto Tagger.
	static const TCHAR* FullPermissionsCodexLaunchLine = TEXT("codex --dangerously-bypass-approvals-and-sandbox");

	// Older auto-approve launch line we used to emit; recognized so re-running setup cleans it up
	// instead of leaving a stale duplicate alongside the new line.
	static const TCHAR* LegacyCodexAutoApproveLaunchLine = TEXT("codex --ask-for-approval never --sandbox workspace-write");

	// True if Line is one of OUR managed terminal StartupCommands entries for ProjDir:
	// the TERM line, our exact project cd, an EXACT managed CLI launch line we emit, or a
	// leftover legacy sentinel. Matched EXACTLY (never a prefix) so a user's own commands
	// like "codex login" or "codex --model ..." are preserved, never silently removed.
	static bool IsKnownCliLaunchLine(const FString& Line)
	{
		return Line.Equals(TEXT("claude")) || Line.Equals(TEXT("codex")) || Line.Equals(TEXT("gemini"))
			|| Line.Equals(FullPermissionsCodexLaunchLine) || Line.Equals(LegacyCodexAutoApproveLaunchLine);
	}

	static FString TerminalLaunchCommand(const FConvaiMcpSetupOptions& Options)
	{
		return Options.Agent == EConvaiMcpAgent::Codex && Options.bLaunchCodexWithFullPermissions
			? FString(FullPermissionsCodexLaunchLine)
			: FConvaiMcpSetupService::GetCliCommand(Options.Agent);
	}

	static bool IsKnownAgentValue(const int32 Value)
	{
		return Value >= static_cast<int32>(EConvaiMcpAgent::ClaudeCode)
			&& Value <= static_cast<int32>(EConvaiMcpAgent::Codex);
	}

	static bool IsKnownWriteModeValue(const int32 Value)
	{
		return Value == static_cast<int32>(EConvaiContextWriteMode::Append)
			|| Value == static_cast<int32>(EConvaiContextWriteMode::Replace);
	}

	static FString AbsoluteProjectDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	}

	static bool IsCliAgentValue(const EConvaiMcpAgent Agent)
	{
		return Agent == EConvaiMcpAgent::ClaudeCode
			|| Agent == EConvaiMcpAgent::Codex
			|| Agent == EConvaiMcpAgent::Gemini;
	}

	static bool IsSamePath(const FString& A, const FString& B)
	{
#if PLATFORM_WINDOWS
		return A.Equals(B, ESearchCase::IgnoreCase);
#else
		return A.Equals(B, ESearchCase::CaseSensitive);
#endif
	}

	/**
	 * Reject every existing reparse point below TrustedRoot, including Target
	 * itself. TrustedRoot is the boundary selected by the application (project
	 * root, source-workspace root, or LocalAppData) and is intentionally not
	 * inspected so a project opened through a user-selected junction still works.
	 */
	static bool EnsurePathBelowTrustedRootHasNoLinks(
		const FString& TrustedRoot,
		const FString& Target,
		FString& OutError)
	{
		FString Root = FPaths::ConvertRelativePathToFull(TrustedRoot);
		FPaths::NormalizeDirectoryName(Root);
		FString Current = FPaths::ConvertRelativePathToFull(Target);
		FPaths::NormalizeFilename(Current);
		FPaths::CollapseRelativeDirectories(Current);

		if (!IsSamePath(Current, Root) && !FPaths::IsUnderDirectory(Current, Root))
		{
			OutError = FString::Printf(TEXT("Path escapes its trusted root: %s"), *Current);
			return false;
		}

		IFileManager& FileManager = IFileManager::Get();
		while (!IsSamePath(Current, Root))
		{
			const bool bExists = FileManager.FileExists(*Current) || FileManager.DirectoryExists(*Current);
			if (bExists && FileManager.IsSymlink(*Current))
			{
				OutError = FString::Printf(TEXT("Refusing to follow a symlink, junction, or other reparse point: %s"), *Current);
				return false;
			}

			FString Parent = FPaths::GetPath(Current);
			FPaths::NormalizeDirectoryName(Parent);
			if (Parent.IsEmpty()
				|| IsSamePath(Parent, Current)
				|| (!IsSamePath(Parent, Root) && !FPaths::IsUnderDirectory(Parent, Root)))
			{
				OutError = FString::Printf(TEXT("Could not prove path containment below %s: %s"), *Root, *Target);
				return false;
			}
			Current = MoveTemp(Parent);
		}
		return true;
	}

	static bool HasAnyReparsePoint(const FString& Path)
	{
		FString Current = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Current);
		FPaths::CollapseRelativeDirectories(Current);
		IFileManager& FileManager = IFileManager::Get();
		while (!Current.IsEmpty())
		{
			const bool bExists = FileManager.FileExists(*Current) || FileManager.DirectoryExists(*Current);
			if (bExists && FileManager.IsSymlink(*Current))
			{
				return true;
			}
			if (FPaths::IsDrive(Current))
			{
				break;
			}
			FString Parent = FPaths::GetPath(Current);
			FPaths::NormalizeDirectoryName(Parent);
			if (Parent.IsEmpty() || IsSamePath(Parent, Current))
			{
				break;
			}
			Current = MoveTemp(Parent);
		}
		return false;
	}

	static bool IsValidCliIdentity(const FString& Identity)
	{
		if (Identity.Len() != 71 || !Identity.StartsWith(TEXT("blake3:"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 Index = 7; Index < Identity.Len(); ++Index)
		{
			if (!FChar::IsHexDigit(Identity[Index]))
			{
				return false;
			}
		}
		return true;
	}

	static void SerializeUserState(const FUserState& State, FConfigFile& OutConfig)
	{
		OutConfig.Empty();
		OutConfig.SetInt64(UserStateSection, TEXT("PreferenceVersion"), PreferenceVersion);
		OutConfig.SetBool(UserStateSection, TEXT("bHasPreferredAgent"), State.bHasPreferredAgent);
		if (State.bHasPreferredAgent)
		{
			OutConfig.SetInt64(UserStateSection, TEXT("PreferredAgent"), static_cast<int32>(State.PreferredAgent));
		}
		OutConfig.SetBool(UserStateSection, TEXT("bPending"), State.bPending);
		if (State.bPending)
		{
			OutConfig.SetInt64(UserStateSection, TEXT("PendingVersion"), PendingVersion);
			OutConfig.SetString(UserStateSection, TEXT("PendingAuthorization"), *State.PendingAuthorization);
			OutConfig.SetInt64(UserStateSection, TEXT("PendingAgent"), static_cast<int32>(State.PendingOptions.Agent));
			OutConfig.SetBool(UserStateSection, TEXT("bAutoStartServer"), State.PendingOptions.bAutoStartServer);
			OutConfig.SetBool(UserStateSection, TEXT("bConfigureTerminal"), State.PendingOptions.bConfigureTerminal);
			OutConfig.SetBool(UserStateSection, TEXT("bLaunchCodexWithFullPermissions"), State.PendingOptions.bLaunchCodexWithFullPermissions);
			OutConfig.SetBool(UserStateSection, TEXT("bAddPrimer"), State.PendingOptions.bAddPrimer);
			OutConfig.SetInt64(UserStateSection, TEXT("ContextWriteMode"), static_cast<int32>(State.PendingOptions.ContextWriteMode));
		}
		if (!State.ApprovedCliPath.IsEmpty())
		{
			OutConfig.SetString(UserStateSection, TEXT("ApprovedCliPath"), *State.ApprovedCliPath);
			OutConfig.SetString(UserStateSection, TEXT("ApprovedCliIdentity"), *State.ApprovedCliIdentity);
		}
	}

	static bool DeserializeUserState(const FConfigFile& Config, FUserState& OutState, FString& OutError)
	{
		int32 Version = 0;
		if (!Config.GetInt(UserStateSection, TEXT("PreferenceVersion"), Version)
			|| Version != PreferenceVersion
			|| !Config.GetBool(UserStateSection, TEXT("bHasPreferredAgent"), OutState.bHasPreferredAgent)
			|| !Config.GetBool(UserStateSection, TEXT("bPending"), OutState.bPending))
		{
			OutError = TEXT("The standalone MCP setup state has an invalid or unsupported header.");
			return false;
		}

		if (OutState.bHasPreferredAgent)
		{
			int32 AgentValue = INDEX_NONE;
			if (!Config.GetInt(UserStateSection, TEXT("PreferredAgent"), AgentValue)
				|| !IsKnownAgentValue(AgentValue))
			{
				OutError = TEXT("The standalone MCP setup state has an invalid preferred agent.");
				return false;
			}
			OutState.PreferredAgent = static_cast<EConvaiMcpAgent>(AgentValue);
		}

		const bool bHasApprovedPath = Config.GetString(
			UserStateSection, TEXT("ApprovedCliPath"), OutState.ApprovedCliPath);
		const bool bHasApprovedIdentity = Config.GetString(
			UserStateSection, TEXT("ApprovedCliIdentity"), OutState.ApprovedCliIdentity);
		if (bHasApprovedPath != bHasApprovedIdentity
			|| (bHasApprovedPath && (!OutState.bHasPreferredAgent
				|| (OutState.PreferredAgent != EConvaiMcpAgent::Codex
					&& OutState.PreferredAgent != EConvaiMcpAgent::ClaudeCode)
				|| FPaths::IsRelative(OutState.ApprovedCliPath)
				|| !IsValidCliIdentity(OutState.ApprovedCliIdentity))))
		{
			OutError = TEXT("The standalone MCP setup state has an invalid CLI approval record.");
			return false;
		}
		if (bHasApprovedPath)
		{
			OutState.ApprovedCliPath = FPaths::ConvertRelativePathToFull(OutState.ApprovedCliPath);
			FPaths::NormalizeFilename(OutState.ApprovedCliPath);
			OutState.ApprovedCliIdentity.ToLowerInline();
		}

		if (!OutState.bPending)
		{
			return true;
		}

		int32 PendingStateVersion = 0;
		int32 PendingAgent = INDEX_NONE;
		int32 WriteMode = INDEX_NONE;
		FGuid Authorization;
		if (!Config.GetInt(UserStateSection, TEXT("PendingVersion"), PendingStateVersion)
			|| PendingStateVersion != PendingVersion
			|| !Config.GetString(UserStateSection, TEXT("PendingAuthorization"), OutState.PendingAuthorization)
			|| !FGuid::Parse(OutState.PendingAuthorization, Authorization)
			|| !Config.GetInt(UserStateSection, TEXT("PendingAgent"), PendingAgent)
			|| !IsKnownAgentValue(PendingAgent)
			|| !Config.GetBool(UserStateSection, TEXT("bAutoStartServer"), OutState.PendingOptions.bAutoStartServer)
			|| !Config.GetBool(UserStateSection, TEXT("bConfigureTerminal"), OutState.PendingOptions.bConfigureTerminal)
			|| !Config.GetBool(UserStateSection, TEXT("bAddPrimer"), OutState.PendingOptions.bAddPrimer)
			|| !Config.GetInt(UserStateSection, TEXT("ContextWriteMode"), WriteMode)
			|| !IsKnownWriteModeValue(WriteMode))
		{
			OutError = TEXT("The standalone MCP setup state has an invalid pending transaction.");
			return false;
		}

		OutState.PendingOptions.Agent = static_cast<EConvaiMcpAgent>(PendingAgent);
		OutState.PendingOptions.ContextWriteMode = static_cast<EConvaiContextWriteMode>(WriteMode);
		OutState.PendingOptions.bLaunchCodexWithFullPermissions = false;
		Config.GetBool(
			UserStateSection,
			TEXT("bLaunchCodexWithFullPermissions"),
			OutState.PendingOptions.bLaunchCodexWithFullPermissions);
		if (!OutState.bHasPreferredAgent
			|| OutState.PreferredAgent != OutState.PendingOptions.Agent
			|| (OutState.PendingOptions.bConfigureTerminal && !IsCliAgentValue(OutState.PendingOptions.Agent))
			|| (OutState.PendingOptions.bLaunchCodexWithFullPermissions
				&& (!OutState.PendingOptions.bConfigureTerminal
					|| OutState.PendingOptions.Agent != EConvaiMcpAgent::Codex)))
		{
			OutError = TEXT("The standalone MCP setup pending transaction is internally inconsistent.");
			return false;
		}
		return true;
	}

	static bool LoadUserState(FUserState& OutState, bool& bOutExists, FString& OutError)
	{
		OutState = FUserState();
		const FString SettingsRoot = UserSettingsRoot();
		const FString Root = UserStateRoot();
		const FString Path = UserStatePath();
		if (SettingsRoot.IsEmpty() || Root.IsEmpty() || Path.IsEmpty())
		{
			bOutExists = false;
			OutError = TEXT("A secure per-user settings directory is unavailable for MCP setup state.");
			return false;
		}
		bOutExists = IFileManager::Get().FileExists(*Path);
		if (!bOutExists)
		{
			return true;
		}

		if (!EnsurePathBelowTrustedRootHasNoLinks(SettingsRoot, Path, OutError))
		{
			return false;
		}
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size < 0 || Size > MaxUserStateBytes)
		{
			OutError = FString::Printf(
				TEXT("The standalone MCP setup state is unreadable or exceeds %lld bytes: %s"),
				MaxUserStateBytes,
				*Path);
			return false;
		}

		FConfigFile Config;
		Config.Read(Path);
		return DeserializeUserState(Config, OutState, OutError);
	}

	static bool SaveUserState(const FUserState& State, FString& OutError)
	{
		const FString SettingsRoot = UserSettingsRoot();
		const FString Root = UserStateRoot();
		const FString Path = UserStatePath();
		if (SettingsRoot.IsEmpty() || Root.IsEmpty() || Path.IsEmpty())
		{
			OutError = TEXT("A secure per-user settings directory is unavailable for MCP setup state.");
			return false;
		}
		if (!EnsurePathBelowTrustedRootHasNoLinks(SettingsRoot, Path, OutError))
		{
			return false;
		}

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*Root) && !PlatformFile.CreateDirectoryTree(*Root))
		{
			OutError = FString::Printf(TEXT("Could not create the MCP setup state directory: %s"), *Root);
			return false;
		}
		if (!EnsurePathBelowTrustedRootHasNoLinks(SettingsRoot, Path, OutError))
		{
			return false;
		}

		FConfigFile Config;
		SerializeUserState(State, Config);
		if (!Config.Write(Path, false))
		{
			OutError = FString::Printf(TEXT("Could not write standalone MCP setup state: %s"), *Path);
			return false;
		}

		const int64 WrittenSize = IFileManager::Get().FileSize(*Path);
		if (WrittenSize < 0 || WrittenSize > MaxUserStateBytes)
		{
			OutError = FString::Printf(TEXT("Standalone MCP setup state write produced an invalid file: %s"), *Path);
			return false;
		}

		FUserState Readback;
		bool bReadbackExists = false;
		FString ReadbackError;
		if (!LoadUserState(Readback, bReadbackExists, ReadbackError)
			|| !bReadbackExists
			|| !State.Equals(Readback))
		{
			OutError = FString::Printf(
				TEXT("Standalone MCP setup state could not be verified after writing: %s"),
				ReadbackError.IsEmpty() ? TEXT("readback did not match") : *ReadbackError);
			return false;
		}
		return true;
	}

	static ERawPendingState ReadRawPendingState(FString& OutError)
	{
		const FString SettingsRoot = UserSettingsRoot();
		const FString Root = UserStateRoot();
		const FString Path = UserStatePath();
		if (SettingsRoot.IsEmpty() || Root.IsEmpty() || Path.IsEmpty())
		{
			OutError = TEXT("A secure per-user settings directory is unavailable for MCP setup state.");
			return ERawPendingState::Invalid;
		}
		if (!IFileManager::Get().FileExists(*Path))
		{
			return ERawPendingState::None;
		}
		if (!EnsurePathBelowTrustedRootHasNoLinks(SettingsRoot, Path, OutError))
		{
			return ERawPendingState::Invalid;
		}
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size < 0 || Size > MaxUserStateBytes)
		{
			OutError = TEXT("The standalone MCP setup state is unreadable or oversized.");
			return ERawPendingState::Invalid;
		}
		FConfigFile Config;
		Config.Read(Path);
		bool bPending = false;
		if (!Config.GetBool(UserStateSection, TEXT("bPending"), bPending))
		{
			OutError = TEXT("The standalone MCP setup state has no valid pending marker.");
			return ERawPendingState::Invalid;
		}
		return bPending ? ERawPendingState::Pending : ERawPendingState::None;
	}

	static void RemoveLegacyConfigState()
	{
		if (!GConfig)
		{
			return;
		}
		GConfig->EmptySection(LegacyPendingSection, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
		const FString LegacySection = LegacyUserStateSection();
		GConfig->EmptySection(*LegacySection, GEditorSettingsIni);
		GConfig->Flush(false, GEditorSettingsIni);
	}

	static bool NormalizeSafeNativeExecutable(const FString& Candidate, FString& OutPath)
	{
		OutPath.Reset();
		FString FullPath = FPaths::ConvertRelativePathToFull(Candidate);
		FPaths::NormalizeFilename(FullPath);
		FPaths::CollapseRelativeDirectories(FullPath);
		if (!FullPath.EndsWith(TEXT(".exe"), ESearchCase::IgnoreCase)
			|| !IFileManager::Get().FileExists(*FullPath)
			|| IFileManager::Get().FileSize(*FullPath) <= 0
			|| HasAnyReparsePoint(FullPath))
		{
			return false;
		}

		FString ProjectRoot = AbsoluteProjectDir();
		FPaths::NormalizeDirectoryName(ProjectRoot);
		if (IsSamePath(FullPath, ProjectRoot) || FPaths::IsUnderDirectory(FullPath, ProjectRoot))
		{
			return false;
		}

		const FString FilenameOnDisk = IFileManager::Get().GetFilenameOnDisk(*FullPath);
		if (!FilenameOnDisk.IsEmpty())
		{
			FullPath = FPaths::ConvertRelativePathToFull(FilenameOnDisk);
			FPaths::NormalizeFilename(FullPath);
		}
		if (HasAnyReparsePoint(FullPath))
		{
			return false;
		}
		OutPath = MoveTemp(FullPath);
		return true;
	}

	static void AddUniquePath(TArray<FString>& Paths, const FString& Path)
	{
		if (!Paths.ContainsByPredicate([&Path](const FString& Existing)
		{
			return IsSamePath(Existing, Path);
		}))
		{
			Paths.Add(Path);
		}
	}

	static bool ResolveCodexNpmShim(const FString& ShimCandidate, FString& OutNativePath)
	{
#if PLATFORM_WINDOWS
		FString ShimPath = FPaths::ConvertRelativePathToFull(ShimCandidate);
		FPaths::NormalizeFilename(ShimPath);
		if (!IFileManager::Get().FileExists(*ShimPath) || HasAnyReparsePoint(ShimPath))
		{
			return false;
		}

		const FString ShimDirectory = FPaths::GetPath(ShimPath);
		const FString CodexPackageRoot = FPaths::Combine(
			ShimDirectory, TEXT("node_modules"), TEXT("@openai"), TEXT("codex"));
		if (!IFileManager::Get().DirectoryExists(*CodexPackageRoot)
			|| HasAnyReparsePoint(CodexPackageRoot))
		{
			return false;
		}

#if PLATFORM_CPU_ARM_FAMILY
		const TCHAR* PlatformPackage = TEXT("codex-win32-arm64");
		const TCHAR* TargetTriple = TEXT("aarch64-pc-windows-msvc");
#else
		const TCHAR* PlatformPackage = TEXT("codex-win32-x64");
		const TCHAR* TargetTriple = TEXT("x86_64-pc-windows-msvc");
#endif

		TArray<FString> LexicalCandidates;
		LexicalCandidates.Add(FPaths::Combine(
			CodexPackageRoot, TEXT("node_modules"), TEXT("@openai"), PlatformPackage,
			TEXT("vendor"), TargetTriple, TEXT("bin"), TEXT("codex.exe")));
		LexicalCandidates.Add(FPaths::Combine(
			ShimDirectory, TEXT("node_modules"), TEXT("@openai"), PlatformPackage,
			TEXT("vendor"), TargetTriple, TEXT("bin"), TEXT("codex.exe")));
		LexicalCandidates.Add(FPaths::Combine(
			CodexPackageRoot, TEXT("vendor"), TargetTriple, TEXT("bin"), TEXT("codex.exe")));

		TArray<FString> NativeCandidates;
		for (const FString& LexicalCandidate : LexicalCandidates)
		{
			FString NativePath;
			if (NormalizeSafeNativeExecutable(LexicalCandidate, NativePath))
			{
				AddUniquePath(NativeCandidates, NativePath);
			}
		}
		if (NativeCandidates.Num() == 1)
		{
			OutNativePath = MoveTemp(NativeCandidates[0]);
			return true;
		}
#endif
		OutNativePath.Reset();
		return false;
	}

	static bool ResolveClaudeNpmShim(const FString& ShimCandidate, FString& OutNativePath)
	{
#if PLATFORM_WINDOWS
		FString ShimPath = FPaths::ConvertRelativePathToFull(ShimCandidate);
		FPaths::NormalizeFilename(ShimPath);
		if (!IFileManager::Get().FileExists(*ShimPath) || HasAnyReparsePoint(ShimPath))
		{
			return false;
		}

		// Current Claude Code npm launchers delegate to this native binary.
		// Resolve it rather than executing the mutable command-processor shim so the
		// setup flow can pin and re-verify the exact executable identity.
		const FString NativeCandidate = FPaths::Combine(
			FPaths::GetPath(ShimPath), TEXT("node_modules"), TEXT("@anthropic-ai"),
			TEXT("claude-code"), TEXT("bin"), TEXT("claude.exe"));
		return NormalizeSafeNativeExecutable(NativeCandidate, OutNativePath);
#else
		OutNativePath.Reset();
		return false;
#endif
	}

	static FClientConfigDescriptor GetClientConfigDescriptor(const EConvaiMcpAgent Agent)
	{
		switch (Agent)
		{
		case EConvaiMcpAgent::ClaudeCode:
			return { TEXT(".mcp.json"), TEXT("mcpServers"), TEXT("url"), true, false };
		case EConvaiMcpAgent::Cursor:
			return { TEXT(".cursor/mcp.json"), TEXT("mcpServers"), TEXT("url"), false, false };
		case EConvaiMcpAgent::VSCode:
			return { TEXT(".vscode/mcp.json"), TEXT("servers"), TEXT("url"), true, false };
		case EConvaiMcpAgent::Gemini:
			return { TEXT(".gemini/settings.json"), TEXT("mcpServers"), TEXT("httpUrl"), false, false };
		case EConvaiMcpAgent::Codex:
			return { TEXT(".codex/config.toml"), TEXT("mcp_servers"), TEXT("url"), false, true };
		default:
			return {};
		}
	}

	static FString GetClientConfigRoot()
	{
		FString Root = FApp::IsEngineInstalled() ? FPaths::ProjectDir() : FPaths::RootDir();
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Root);
		return Root;
	}

	static FString GetExpectedMcpServerUrl()
	{
		uint32 Port = 8000;
		FString UrlPath = TEXT("/mcp");
		if (GConfig)
		{
			int32 ConfigPort = 0;
			if (GConfig->GetInt(McpSettingsSection, TEXT("ServerPortNumber"), ConfigPort, GEditorPerProjectIni)
				&& ConfigPort >= 0)
			{
				Port = static_cast<uint32>(ConfigPort);
			}
			GConfig->GetString(McpSettingsSection, TEXT("ServerUrlPath"), UrlPath, GEditorPerProjectIni);
		}

		uint32 CommandLinePort = 0;
		if (FParse::Value(FCommandLine::Get(), TEXT("ModelContextProtocolPort="), CommandLinePort)
			&& CommandLinePort >= 1 && CommandLinePort <= 65535)
		{
			Port = CommandLinePort;
		}
		return FString::Printf(TEXT("http://127.0.0.1:%u%s"), Port, *UrlPath);
	}

	static bool LoadBoundedTextFile(
		const FString& Path,
		const int64 MaximumBytes,
		FString& OutContent,
		FString& OutError)
	{
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size < 0 || Size > MaximumBytes)
		{
			OutError = FString::Printf(TEXT("Configuration is unreadable or exceeds %lld bytes: %s"), MaximumBytes, *Path);
			return false;
		}
		if (!FFileHelper::LoadFileToString(OutContent, *Path))
		{
			OutError = FString::Printf(TEXT("Could not read configuration: %s"), *Path);
			return false;
		}
		return true;
	}

	static FString StripTomlComment(const FString& Input)
	{
		TCHAR Quote = 0;
		bool bEscaped = false;
		for (int32 Index = 0; Index < Input.Len(); ++Index)
		{
			const TCHAR Character = Input[Index];
			if (Quote != 0)
			{
				if (Quote == TEXT('"') && Character == TEXT('\\') && !bEscaped)
				{
					bEscaped = true;
					continue;
				}
				if (Character == Quote && !bEscaped)
				{
					Quote = 0;
				}
				bEscaped = false;
				continue;
			}
			if (Character == TEXT('"') || Character == TEXT('\''))
			{
				Quote = Character;
			}
			else if (Character == TEXT('#'))
			{
				return Input.Left(Index);
			}
		}
		return Input;
	}

	static bool VerifyCodexToml(const FString& Content, const FString& ExpectedUrl, FString& OutError)
	{
		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines, false);
		bool bInServerSection = false;
		int32 MatchingSections = 0;
		int32 UrlEntries = 0;
		FString FoundUrl;
		for (FString Line : Lines)
		{
			Line = StripTomlComment(Line).TrimStartAndEnd();
			if (Line.IsEmpty())
			{
				continue;
			}
			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				const FString Section = Line.Mid(1, Line.Len() - 2).TrimStartAndEnd();
				bInServerSection = Section.Equals(TEXT("mcp_servers.unreal-mcp"), ESearchCase::CaseSensitive);
				if (bInServerSection)
				{
					++MatchingSections;
				}
				continue;
			}
			if (!bInServerSection)
			{
				continue;
			}
			FString Key;
			FString Value;
			if (!Line.Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			Key.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			if (!Key.Equals(TEXT("url"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			++UrlEntries;
			if (Value.Len() >= 2
				&& ((Value[0] == TEXT('"') && Value[Value.Len() - 1] == TEXT('"'))
					|| (Value[0] == TEXT('\'') && Value[Value.Len() - 1] == TEXT('\''))))
			{
				FoundUrl = Value.Mid(1, Value.Len() - 2);
			}
		}

		if (MatchingSections != 1 || UrlEntries != 1 || !FoundUrl.Equals(ExpectedUrl, ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(
				TEXT("Codex config must contain exactly one [mcp_servers.unreal-mcp] entry with url = \"%s\"."),
				*ExpectedUrl);
			return false;
		}
		return true;
	}

	static bool VerifyJsonClientConfig(
		const FString& Content,
		const FClientConfigDescriptor& Descriptor,
		const FString& ExpectedUrl,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = TEXT("Generated MCP client configuration is not valid JSON.");
			return false;
		}
		const TSharedPtr<FJsonObject>* Servers = nullptr;
		const TSharedPtr<FJsonObject>* Server = nullptr;
		FString Url;
		FString Type;
		if (!Root->TryGetObjectField(Descriptor.ServersRootKey, Servers)
			|| !Servers
			|| !Servers->IsValid()
			|| !(*Servers)->TryGetObjectField(TEXT("unreal-mcp"), Server)
			|| !Server
			|| !Server->IsValid()
			|| !(*Server)->TryGetStringField(Descriptor.UrlFieldName, Url)
			|| !Url.Equals(ExpectedUrl, ESearchCase::CaseSensitive)
			|| (Descriptor.bIncludeTypeField
				&& (!(*Server)->TryGetStringField(TEXT("type"), Type)
					|| !Type.Equals(TEXT("http"), ESearchCase::CaseSensitive))))
		{
			OutError = Descriptor.bIncludeTypeField
				? FString::Printf(
					TEXT("MCP client config does not contain the expected unreal-mcp type=http entry and URL (%s)."),
					*ExpectedUrl)
				: FString::Printf(
					TEXT("MCP client config does not contain the expected unreal-mcp URL (%s)."),
					*ExpectedUrl);
			return false;
		}
		return true;
	}

	static bool VerifyClientConfiguration(
		const EConvaiMcpAgent Agent,
		FString& OutConfigPath,
		FString& OutError)
	{
		const FClientConfigDescriptor Descriptor = GetClientConfigDescriptor(Agent);
		if (Descriptor.RelativePath.IsEmpty())
		{
			OutError = TEXT("The selected MCP client has no known configuration descriptor.");
			return false;
		}
		const FString Root = GetClientConfigRoot();
		OutConfigPath = FPaths::Combine(Root, Descriptor.RelativePath);
		FPaths::NormalizeFilename(OutConfigPath);
		if (!EnsurePathBelowTrustedRootHasNoLinks(Root, OutConfigPath, OutError))
		{
			return false;
		}

		FString Content;
		if (!LoadBoundedTextFile(OutConfigPath, MaxClientConfigBytes, Content, OutError))
		{
			return false;
		}
		const FString ExpectedUrl = GetExpectedMcpServerUrl();
		return Descriptor.bToml
			? VerifyCodexToml(Content, ExpectedUrl, OutError)
			: VerifyJsonClientConfig(Content, Descriptor, ExpectedUrl, OutError);
	}

	static TArray<FString> StripManagedTerminalBlocks(
		const TArray<FString>& Commands,
		const FString& ProjectDirectory)
	{
		const FString ProjectCd = FString::Printf(TEXT("cd /d \"%s\""), *ProjectDirectory);
		TArray<FString> Cleaned;
		Cleaned.Reserve(Commands.Num());
		for (int32 Index = 0; Index < Commands.Num(); ++Index)
		{
			const FString& Line = Commands[Index];
			if (Line.Equals(LegacyTermSentinelBegin))
			{
				int32 EndIndex = Index + 1;
				while (EndIndex < Commands.Num() && !Commands[EndIndex].Equals(LegacyTermSentinelEnd))
				{
					++EndIndex;
				}
				if (EndIndex < Commands.Num())
				{
					Index = EndIndex;
					continue;
				}
				// An orphaned marker is ours, but never treat unrelated following commands as managed.
				continue;
			}
			if (Line.Equals(LegacyTermSentinelEnd)
				|| Line.Equals(FullPermissionsCodexLaunchLine))
			{
				continue;
			}
			const bool bManagedTriplet = Index + 2 < Commands.Num()
				&& Line.Equals(TEXT("set TERM=xterm-256color"))
				&& Commands[Index + 1].Equals(ProjectCd)
				&& IsKnownCliLaunchLine(Commands[Index + 2]);
			if (bManagedTriplet)
			{
				Index += 2;
				continue;
			}
			Cleaned.Add(Line);
		}
		return Cleaned;
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

bool FConvaiMcpSetupService::SupportsVisionCli(EConvaiMcpAgent Agent)
{
	return Agent == EConvaiMcpAgent::Codex || Agent == EConvaiMcpAgent::ClaudeCode;
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

FString FConvaiMcpSetupService::GetAgentInstallUrl(EConvaiMcpAgent Agent)
{
	switch (Agent)
	{
	case EConvaiMcpAgent::ClaudeCode: return TEXT("https://code.claude.com/docs/en/setup");
	case EConvaiMcpAgent::Cursor:     return TEXT("https://cursor.com/downloads");
	case EConvaiMcpAgent::VSCode:     return TEXT("https://code.visualstudio.com/download");
	case EConvaiMcpAgent::Gemini:     return TEXT("https://geminicli.com/docs/get-started/installation/");
	case EConvaiMcpAgent::Codex:      return TEXT("https://developers.openai.com/codex/cli");
	default:                          return FString();
	}
}

bool FConvaiMcpSetupService::FindCliExecutable(EConvaiMcpAgent Agent, FString& OutExecutablePath)
{
	OutExecutablePath.Reset();
	const FString Command = GetCliCommand(Agent);
	if (Command.IsEmpty())
	{
		return false;
	}

	FString PathValue = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
	TArray<FString> PathEntries;
	PathValue.ParseIntoArray(PathEntries, TEXT(";"), true);
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);
	const TCHAR* Extensions[] = { TEXT(".exe"), TEXT(".cmd") };
	for (FString Directory : PathEntries)
	{
		Directory.TrimStartAndEndInline();
		Directory.TrimQuotesInline();
		if (Directory.IsEmpty())
		{
			continue;
		}
		Directory = FPaths::ConvertRelativePathToFull(Directory);
		for (const TCHAR* Extension : Extensions)
		{
			FString Candidate = FPaths::Combine(Directory, Command + Extension);
			FPaths::NormalizeFilename(Candidate);
			if (ConvaiMcp::IsSamePath(Candidate, ProjectDir)
				|| FPaths::IsUnderDirectory(Candidate, ProjectDir)
				|| !FPaths::FileExists(Candidate)
				|| ConvaiMcp::HasAnyReparsePoint(Candidate))
			{
				continue;
			}

			if (Agent == EConvaiMcpAgent::Codex || Agent == EConvaiMcpAgent::ClaudeCode)
			{
				FString NativePath;
				const bool bResolved = FCString::Stricmp(Extension, TEXT(".exe")) == 0
					? ConvaiMcp::NormalizeSafeNativeExecutable(Candidate, NativePath)
					: (Agent == EConvaiMcpAgent::Codex
						? ConvaiMcp::ResolveCodexNpmShim(Candidate, NativePath)
						: ConvaiMcp::ResolveClaudeNpmShim(Candidate, NativePath));
				if (bResolved)
				{
					// Match normal PATH lookup semantics: the first safe launcher wins.
					// Setup records its resolved native path and content identity, so a
					// later PATH or package change still requires explicit re-approval.
					OutExecutablePath = MoveTemp(NativePath);
					return true;
				}
				continue;
			}

			OutExecutablePath = MoveTemp(Candidate);
			return true;
		}
	}
	return false;
}

bool FConvaiMcpSetupService::ComputeCliExecutableIdentity(
	const FString& ExecutablePath,
	FString& OutIdentity,
	FString& OutError)
{
	OutIdentity.Reset();
	OutError.Reset();
	FString SafePath;
	if (!ConvaiMcp::NormalizeSafeNativeExecutable(ExecutablePath, SafePath))
	{
		OutError = TEXT("The CLI path is not an approved native executable path or traverses a reparse point.");
		return false;
	}

	IFileManager& FileManager = IFileManager::Get();
	const int64 InitialSize = FileManager.FileSize(*SafePath);
	const FDateTime InitialTimestamp = FileManager.GetTimeStamp(*SafePath);
	TUniquePtr<FArchive> Reader(FileManager.CreateFileReader(*SafePath));
	if (!Reader || InitialSize <= 0 || Reader->TotalSize() != InitialSize)
	{
		OutError = FString::Printf(TEXT("Could not open the native CLI executable for identity verification: %s"), *SafePath);
		return false;
	}

	FBlake3 Hasher;
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(static_cast<int32>(ConvaiMcp::HashBufferBytes));
	int64 Remaining = InitialSize;
	while (Remaining > 0)
	{
		const int64 BytesToRead = FMath::Min<int64>(Remaining, Buffer.Num());
		Reader->Serialize(Buffer.GetData(), BytesToRead);
		if (Reader->IsError())
		{
			OutError = FString::Printf(TEXT("Failed while hashing the native CLI executable: %s"), *SafePath);
			return false;
		}
		Hasher.Update(Buffer.GetData(), static_cast<uint64>(BytesToRead));
		Remaining -= BytesToRead;
	}
	Reader.Reset();

	if (FileManager.FileSize(*SafePath) != InitialSize
		|| FileManager.GetTimeStamp(*SafePath) != InitialTimestamp
		|| ConvaiMcp::HasAnyReparsePoint(SafePath))
	{
		OutError = TEXT("The native CLI executable changed while its identity was being computed.");
		return false;
	}

	OutIdentity = TEXT("blake3:") + LexToString(Hasher.Finalize()).ToLower();
	return true;
}

bool FConvaiMcpSetupService::TryGetPreferredAgent(EConvaiMcpAgent& OutAgent)
{
	ConvaiMcp::FUserState State;
	bool bExists = false;
	FString Error;
	if (!ConvaiMcp::LoadUserState(State, bExists, Error) || !bExists || !State.bHasPreferredAgent)
	{
		if (!Error.IsEmpty())
		{
			UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: could not read preferred agent: %s"), *Error);
		}
		return false;
	}
	OutAgent = State.PreferredAgent;
	return true;
}

bool FConvaiMcpSetupService::IsPreferredVisionCliReady(
	EConvaiMcpAgent& OutAgent,
	FString& OutExecutablePath,
	FText& OutDetail,
	FString* OutApprovedIdentity)
{
	if (OutApprovedIdentity)
	{
		OutApprovedIdentity->Reset();
	}
	if (!TryGetPreferredAgent(OutAgent))
	{
		OutExecutablePath.Reset();
		OutDetail = LOCTEXT("NoPreferredAgent", "No AI agent has been selected for this project. Run MCP setup first.");
		return false;
	}
	if (IsSetupPending())
	{
		OutExecutablePath.Reset();
		OutDetail = LOCTEXT("PreferredAgentSetupPending", "MCP setup is still pending. Finish or rerun setup before using Auto Tagger.");
		return false;
	}
	if (!SupportsVisionCli(OutAgent))
	{
		OutExecutablePath.Reset();
		OutDetail = FText::Format(
			LOCTEXT("PreferredAgentNoVisionCli", "{0} is connected for AI coding, but Scene Auto Tagger currently supports Claude Code or Codex."),
			GetAgentDisplayName(OutAgent));
		return false;
	}
	if (!FindCliExecutable(OutAgent, OutExecutablePath))
	{
		OutDetail = FText::Format(
			LOCTEXT("PreferredAgentCliMissing", "{0} is selected, but its `{1}` CLI was not found on PATH."),
			GetAgentDisplayName(OutAgent),
			FText::FromString(GetCliCommand(OutAgent)));
		return false;
	}

	ConvaiMcp::FUserState State;
	bool bStateExists = false;
	FString StateError;
	if (!ConvaiMcp::LoadUserState(State, bStateExists, StateError) || !bStateExists)
	{
		OutExecutablePath.Reset();
		OutDetail = FText::FromString(StateError.IsEmpty()
			? TEXT("The standalone MCP setup state is unavailable. Run setup again.")
			: StateError);
		return false;
	}
	FString ResolvedPath = OutExecutablePath;
	FPaths::NormalizeFilename(ResolvedPath);
	if (State.ApprovedCliPath.IsEmpty()
		|| State.ApprovedCliIdentity.IsEmpty()
		|| !ConvaiMcp::IsSamePath(State.ApprovedCliPath, ResolvedPath))
	{
		OutExecutablePath.Reset();
		OutDetail = FText::Format(
			LOCTEXT("PreferredAgentCliChanged", "The detected {0} CLI is new or changed. Open AI Assistant Setup and connect it again."),
			GetAgentDisplayName(OutAgent));
		return false;
	}

	FString CurrentIdentity;
	FString IdentityError;
	if (!ComputeCliExecutableIdentity(ResolvedPath, CurrentIdentity, IdentityError)
		|| !CurrentIdentity.Equals(State.ApprovedCliIdentity, ESearchCase::CaseSensitive))
	{
		OutExecutablePath.Reset();
		OutDetail = IdentityError.IsEmpty()
			? FText::Format(
				LOCTEXT("PreferredAgentCliIdentityChanged", "The approved {0} native executable has changed. Review and apply MCP setup again."),
				GetAgentDisplayName(OutAgent))
			: FText::FromString(IdentityError);
		return false;
	}
	if (OutApprovedIdentity)
	{
		*OutApprovedIdentity = CurrentIdentity;
	}
	OutDetail = FText::Format(
		LOCTEXT("PreferredAgentCliFound", "{0} is selected and its CLI is ready. Sign-in is checked when analysis starts."),
		GetAgentDisplayName(OutAgent));
	return true;
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

	const TArray<FString> UserCommands = ConvaiMcp::StripManagedTerminalBlocks(Existing, ProjDir);
	for (const FString& Line : UserCommands)
	{
		if (Line.TrimStartAndEnd().IsEmpty())
		{
			continue;
		}
		return true;
	}
	return false;
}

bool FConvaiMcpSetupService::IsTerminalConfiguredForAgent(EConvaiMcpAgent Agent)
{
	if (!GConfig || !IsCliAgent(Agent))
	{
		return false;
	}
	TArray<FString> Commands;
	GConfig->GetArray(ConvaiMcp::TerminalSettingsSection, TEXT("StartupCommands"), Commands, GEditorPerProjectIni);
	FString ProjDir = ConvaiMcp::AbsoluteProjectDir();
	FPaths::MakePlatformFilename(ProjDir);
	ProjDir.RemoveFromEnd(TEXT("\\"));
	const FString ProjectCd = FString::Printf(TEXT("cd /d \"%s\""), *ProjDir);
	for (int32 Index = 0; Index + 2 < Commands.Num(); ++Index)
	{
		if (Commands[Index].Equals(TEXT("set TERM=xterm-256color"))
			&& Commands[Index + 1].Equals(ProjectCd)
			&& (Commands[Index + 2].Equals(GetCliCommand(Agent))
				|| (Agent == EConvaiMcpAgent::Codex
					&& Commands[Index + 2].Equals(ConvaiMcp::FullPermissionsCodexLaunchLine))))
		{
			return true;
		}
	}
	return false;
}

bool FConvaiMcpSetupService::IsTerminalConfiguredWithFullPermissions(EConvaiMcpAgent Agent)
{
	if (!GConfig || Agent != EConvaiMcpAgent::Codex)
	{
		return false;
	}
	TArray<FString> Commands;
	GConfig->GetArray(ConvaiMcp::TerminalSettingsSection, TEXT("StartupCommands"), Commands, GEditorPerProjectIni);
	FString ProjDir = ConvaiMcp::AbsoluteProjectDir();
	FPaths::MakePlatformFilename(ProjDir);
	ProjDir.RemoveFromEnd(TEXT("\\"));
	const FString ProjectCd = FString::Printf(TEXT("cd /d \"%s\""), *ProjDir);
	for (int32 Index = 0; Index + 2 < Commands.Num(); ++Index)
	{
		if (Commands[Index].Equals(TEXT("set TERM=xterm-256color"))
			&& Commands[Index + 1].Equals(ProjectCd)
			&& Commands[Index + 2].Equals(ConvaiMcp::FullPermissionsCodexLaunchLine))
		{
			return true;
		}
	}
	return false;
}

bool FConvaiMcpSetupService::EnableAndScheduleSetup(
	const FConvaiMcpSetupOptions& Options,
	FString& OutError,
	bool* bOutRestartRequired)
{
	if (bOutRestartRequired)
	{
		*bOutRestartRequired = true;
	}
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

	if (!WritePending(Options, OutError))
	{
		return false;
	}

	const bool bCanApplyNow = FModuleManager::Get().IsModuleLoaded(TEXT("ModelContextProtocolEngine"))
		&& (!Options.bConfigureTerminal || FModuleManager::Get().IsModuleLoaded(TEXT("Terminal")));
	if (bCanApplyNow)
	{
		FString ApplyError;
		if (!ApplyPendingSetupIfAny(&ApplyError))
		{
			OutError = ApplyError.IsEmpty() ? TEXT("MCP setup could not be applied.") : MoveTemp(ApplyError);
			return false;
		}
		if (bOutRestartRequired)
		{
			*bOutRestartRequired = IsSetupPending();
		}
		UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiMcp: setup applied without a restart (agent=%s)."), *GetClientConfigToken(Options.Agent));
	}
	else
	{
		UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiMcp: plugins enabled, setup scheduled for next restart (agent=%s)."), *GetClientConfigToken(Options.Agent));
	}
	return true;
}

bool FConvaiMcpSetupService::IsSetupPending()
{
	FString Error;
	return ConvaiMcp::ReadRawPendingState(Error) != ConvaiMcp::ERawPendingState::None;
}

bool FConvaiMcpSetupService::WritePending(const FConvaiMcpSetupOptions& Options, FString& OutError)
{
	if (!ConvaiMcp::IsKnownAgentValue(static_cast<int32>(Options.Agent))
		|| !ConvaiMcp::IsKnownWriteModeValue(static_cast<int32>(Options.ContextWriteMode))
		|| (Options.bConfigureTerminal && !IsCliAgent(Options.Agent))
		|| (Options.bLaunchCodexWithFullPermissions
			&& (!Options.bConfigureTerminal || Options.Agent != EConvaiMcpAgent::Codex)))
	{
		OutError = TEXT("MCP setup options are invalid.");
		return false;
	}

	// Explicit confirmation starts a fresh, single-file transaction. This also
	// safely repairs a corrupt prior state file instead of requiring manual deletion.
	ConvaiMcp::FUserState State;
	State.bHasPreferredAgent = true;
	State.PreferredAgent = Options.Agent;
	State.bPending = true;
	State.PendingAuthorization = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	State.PendingOptions = Options;
	State.PendingOptions.ConfirmedCliExecutablePath.Reset();

	FString ConfirmedExecutablePath;
	if (IsCliAgent(Options.Agent))
	{
		FString ResolvedExecutablePath;
		if (Options.ConfirmedCliExecutablePath.IsEmpty()
			|| !FindCliExecutable(Options.Agent, ResolvedExecutablePath))
		{
			OutError = TEXT("The selected CLI is no longer available. Recheck its path in MCP setup and confirm again.");
			return false;
		}
		ConfirmedExecutablePath = FPaths::ConvertRelativePathToFull(Options.ConfirmedCliExecutablePath);
		ResolvedExecutablePath = FPaths::ConvertRelativePathToFull(ResolvedExecutablePath);
		FPaths::NormalizeFilename(ConfirmedExecutablePath);
		FPaths::NormalizeFilename(ResolvedExecutablePath);
		if (!ConvaiMcp::IsSamePath(ConfirmedExecutablePath, ResolvedExecutablePath))
		{
			OutError = TEXT("The selected CLI path changed after it was displayed. Reopen MCP setup and approve the new path.");
			return false;
		}
	}

	if (SupportsVisionCli(Options.Agent))
	{
		FString Identity;
		if (!ComputeCliExecutableIdentity(ConfirmedExecutablePath, Identity, OutError))
		{
			return false;
		}
		State.ApprovedCliPath = MoveTemp(ConfirmedExecutablePath);
		State.ApprovedCliIdentity = MoveTemp(Identity);
	}

	if (!ConvaiMcp::SaveUserState(State, OutError))
	{
		return false;
	}
	ConvaiMcp::RemoveLegacyConfigState();
	return true;
}

bool FConvaiMcpSetupService::ReadPending(FConvaiMcpSetupOptions& OutOptions, FString* OutError)
{
	ConvaiMcp::FUserState State;
	bool bExists = false;
	FString Error;
	if (!ConvaiMcp::LoadUserState(State, bExists, Error) || !bExists || !State.bPending)
	{
		if (OutError)
		{
			*OutError = Error.IsEmpty() ? TEXT("No valid pending MCP setup transaction exists.") : MoveTemp(Error);
		}
		return false;
	}
	OutOptions = State.PendingOptions;
	return true;
}

bool FConvaiMcpSetupService::ClearPending(FString& OutError)
{
	ConvaiMcp::FUserState State;
	bool bExists = false;
	if (!ConvaiMcp::LoadUserState(State, bExists, OutError) || !bExists)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Standalone MCP setup state disappeared before the transaction could be cleared.");
		}
		return false;
	}
	State.bPending = false;
	State.PendingAuthorization.Reset();
	State.PendingOptions = FConvaiMcpSetupOptions();
	return ConvaiMcp::SaveUserState(State, OutError);
}

bool FConvaiMcpSetupService::ApplyPendingSetupIfAny(FString* OutError)
{
	FString RawStateError;
	const ConvaiMcp::ERawPendingState RawState = ConvaiMcp::ReadRawPendingState(RawStateError);
	if (RawState == ConvaiMcp::ERawPendingState::None)
	{
		return true;
	}
	if (RawState == ConvaiMcp::ERawPendingState::Invalid)
	{
		const FString Error = RawStateError.IsEmpty()
			? TEXT("Pending MCP setup state is corrupt. Run setup again to replace it.")
			: RawStateError;
		if (OutError) { *OutError = Error; }
		UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: %s"), *Error);
		ConvaiMcp::Toast(FText::FromString(Error), false);
		return false;
	}
	FConvaiMcpSetupOptions Options;
	FString PendingError;
	if (!ReadPending(Options, &PendingError))
	{
		const FString Error = PendingError.IsEmpty()
			? TEXT("Pending MCP setup state is invalid. Run setup again.")
			: PendingError;
		if (OutError) { *OutError = Error; }
		UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: %s"), *Error);
		ConvaiMcp::Toast(FText::FromString(Error), false);
		return false;
	}

	// Guard: the MCP/Terminal modules must be loaded before we touch their
	// settings / console commands. If not, leave the record sticky for a later launch.
	const bool bMcpLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("ModelContextProtocolEngine"));
	const bool bTerminalLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("Terminal"));
	if (!bMcpLoaded)
	{
		const FString Error = TEXT("Convai MCP setup is pending but the MCP plugin is not loaded yet. It will finish on the next restart.");
		if (OutError) { *OutError = Error; }
		UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: %s"), *Error);
		ConvaiMcp::Toast(FText::FromString(Error), false);
		return false;
	}
	if (Options.bConfigureTerminal && !bTerminalLoaded)
	{
		const FString Error = TEXT("Convai MCP terminal setup is pending but the Terminal module is not loaded yet. It will finish on the next restart.");
		if (OutError) { *OutError = Error; }
		UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: %s"), *Error);
		ConvaiMcp::Toast(FText::FromString(Error), false);
		return false;
	}

	FString ApplyError;
	if (!ApplyConfiguration(Options, ApplyError))
	{
		if (OutError) { *OutError = ApplyError; }
		UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: setup remains pending: %s"), *ApplyError);
		ConvaiMcp::Toast(FText::FromString(FString::Printf(TEXT("Convai MCP setup remains pending: %s"), *ApplyError)), false);
		return false;
	}
	FString ClearError;
	if (!ClearPending(ClearError))
	{
		if (OutError) { *OutError = ClearError; }
		UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiMcp: setup applied, but its verified pending state could not be cleared: %s"), *ClearError);
		ConvaiMcp::Toast(FText::FromString(ClearError), false);
		return false;
	}
	return true;
}

bool FConvaiMcpSetupService::ApplyConfiguration(const FConvaiMcpSetupOptions& Options, FString& OutError)
{
	FString Summary;

	// (a) Always establish and verify the chosen agent's exact unreal-mcp entry.
	// GEngine::Exec only reports that a console command was recognized, not that
	// its file write succeeded, so disk verification is the source of truth.
	const ConvaiMcp::FClientConfigDescriptor ClientDescriptor = ConvaiMcp::GetClientConfigDescriptor(Options.Agent);
	const FString ClientConfigRoot = ConvaiMcp::GetClientConfigRoot();
	FString ClientConfigPath = FPaths::Combine(ClientConfigRoot, ClientDescriptor.RelativePath);
	FPaths::NormalizeFilename(ClientConfigPath);
	if (!ConvaiMcp::EnsurePathBelowTrustedRootHasNoLinks(ClientConfigRoot, ClientConfigPath, OutError))
	{
		return false;
	}

	const bool bConfigExisted = FPaths::FileExists(ClientConfigPath);
	bool bConfigVerified = false;
	FString VerificationError;
	if (bConfigExisted)
	{
		bConfigVerified = ConvaiMcp::VerifyClientConfiguration(
			Options.Agent, ClientConfigPath, VerificationError);
	}

	if (!bConfigVerified)
	{
		// Epic's Codex TOML writer is intentionally write-once. Never turn an
		// existing but unconfigured file into a false success.
		if (bConfigExisted && Options.Agent == EConvaiMcpAgent::Codex)
		{
			OutError = FString::Printf(
				TEXT("Existing Codex config is not configured for this Unreal MCP server: %s (%s)"),
				*ClientConfigPath,
				VerificationError.IsEmpty() ? TEXT("missing or incorrect unreal-mcp entry") : *VerificationError);
			return false;
		}
		if (!GEngine)
		{
			OutError = TEXT("The engine command interface is unavailable; client configuration was not generated.");
			return false;
		}
		const FString Cmd = FString::Printf(
			TEXT("ModelContextProtocol.GenerateClientConfig %s"),
			*GetClientConfigToken(Options.Agent));
		if (!GEngine->Exec(nullptr, *Cmd))
		{
			OutError = TEXT("The MCP plugin did not accept the client-configuration command.");
			return false;
		}
		VerificationError.Reset();
		if (!ConvaiMcp::VerifyClientConfiguration(Options.Agent, ClientConfigPath, VerificationError))
		{
			OutError = FString::Printf(
				TEXT("MCP client configuration could not be verified after generation: %s"),
				*VerificationError);
			return false;
		}
		Summary += FString::Printf(
			TEXT("Verified client config for %s. "),
			*GetAgentDisplayName(Options.Agent).ToString());
	}
	else
	{
		Summary += FString::Printf(
			TEXT("Existing client config for %s verified. "),
			*GetAgentDisplayName(Options.Agent).ToString());
	}

	// (b) Always reconcile prior Convai terminal blocks, then optionally add the
	// selected plain CLI command. This also removes legacy unsandboxed Codex lines.
	FString TerminalError;
	if (!ReconcileTerminalStartupCommands(Options, TerminalError))
	{
		OutError = MoveTemp(TerminalError);
		return false;
	}
	if (Options.bConfigureTerminal)
	{
		Summary += TEXT("Terminal configured. ");
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
			OutError = MoveTemp(Err);
			return false;
		}
	}

	// (d) Persist the chosen auto-start value for future launches (true OR false, so
	// unchecking on a re-run disables a previously-enabled auto-start).
	PersistAutoStartServer(Options.bAutoStartServer);

	// Increase scrollback only when the user opted into Terminal configuration.
	// 1000000 is the engine's ClampMax; the default of 131072 is too small for long agent runs.
	// Written before the ReloadSettingsCdo below so it applies live this session.
	if (GConfig && Options.bConfigureTerminal)
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
		HowTo = FString::Printf(TEXT("Open a terminal in the project folder and run `%s`."), *ConvaiMcp::TerminalLaunchCommand(Options));
	}
	else
	{
		HowTo = FString::Printf(TEXT("Open %s on this project folder; it will pick up the MCP config."), *GetAgentDisplayName(Options.Agent).ToString());
	}
	ConvaiMcp::Toast(FText::FromString(FString::Printf(TEXT("Convai MCP ready. %s\n%s"), *Summary, *HowTo)), true);
	UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiMcp: setup applied (%s)"), *Summary);
	return true;
}

bool FConvaiMcpSetupService::ReconcileTerminalStartupCommands(const FConvaiMcpSetupOptions& Options, FString& OutError)
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

	// Remove only a complete Convai-owned triplet or legacy sentinel block. Isolated
	// user commands such as a plain `codex` line remain untouched.
	TArray<FString> Cleaned = ConvaiMcp::StripManagedTerminalBlocks(Commands, ProjDir);

	// Append the functional commands only — no comment sentinels (Windows/cmd.exe).
	if (Options.bConfigureTerminal)
	{
		if (!IsCliAgent(Agent))
		{
			OutError = TEXT("Terminal setup requires a CLI agent.");
			return false;
		}
		Cleaned.Add(TEXT("set TERM=xterm-256color"));
		Cleaned.Add(FString::Printf(TEXT("cd /d \"%s\""), *ProjDir));
		Cleaned.Add(ConvaiMcp::TerminalLaunchCommand(Options));
	}

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

	FString ProjectRoot = ConvaiMcp::AbsoluteProjectDir();
	FPaths::NormalizeDirectoryName(ProjectRoot);
	FString TargetPath = FPaths::Combine(ProjectRoot, GetAgentContextFile(Agent));
	TargetPath = FPaths::ConvertRelativePathToFull(TargetPath);
	FPaths::NormalizeFilename(TargetPath);
	FPaths::CollapseRelativeDirectories(TargetPath);
	if (!ConvaiMcp::EnsurePathBelowTrustedRootHasNoLinks(ProjectRoot, TargetPath, OutError))
	{
		return false;
	}

	// Ensure parent dir exists (.cursor/rules, .github).
	const FString TargetDir = FPaths::GetPath(TargetPath);
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*TargetDir) && !PF.CreateDirectoryTree(*TargetDir))
	{
		OutError = FString::Printf(TEXT("Could not create context-file directory %s"), *TargetDir);
		return false;
	}
	// Re-check after directory creation so a pre-existing nested junction cannot
	// redirect the eventual write out of the project.
	if (!ConvaiMcp::EnsurePathBelowTrustedRootHasNoLinks(ProjectRoot, TargetPath, OutError))
	{
		return false;
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

	if (!ConvaiMcp::EnsurePathBelowTrustedRootHasNoLinks(ProjectRoot, TargetPath, OutError))
	{
		return false;
	}
	if (!FFileHelper::SaveStringToFile(Merged, *TargetPath, FFileHelper::EEncodingOptions::ForceUTF8))
	{
		OutError = FString::Printf(TEXT("Could not write %s"), *TargetPath);
		return false;
	}
	if (!ConvaiMcp::EnsurePathBelowTrustedRootHasNoLinks(ProjectRoot, TargetPath, OutError))
	{
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
