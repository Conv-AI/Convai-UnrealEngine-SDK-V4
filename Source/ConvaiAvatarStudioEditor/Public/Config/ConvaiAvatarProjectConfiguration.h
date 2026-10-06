// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

struct FConvaiAvatarConfigurationChange
{
	FString FileName;
	FString Section;
	FString Key;
	TArray<FString> Before;
	TArray<FString> After;
};

/** Read-only preview. Apply/Restore re-analyze and reject stale previews before writing. */
struct FConvaiAvatarConfigurationPlan
{
	FString ProjectDirectory;
	FString Revision;
	FString SourceUrl;
	FString ProfileId;
	/** Exact validated declarative profile reviewed by the user; retained for apply and recovery. */
	FString ProfileJson;
	TArray<FConvaiAvatarConfigurationChange> Changes;
	TArray<FString> Notes;
	bool bRestore = false;
	bool bHasBackup = false;
	bool bAlreadyApplied = false;
	FString Describe() const;
};

/** Explicit, per-project rendering/packaging profile; no plugins, accounts, or engine files are changed. */
class CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarProjectConfiguration
{
public:
	static bool Analyze(const FString& ProjectDirectory, FConvaiAvatarConfigurationPlan& OutPlan, FString& OutError,
		const FString& ProfileJson = FString(), const FString& ProfileSourceUrl = FString(), const FString& ProfileNotice = FString());
	static bool Apply(const FConvaiAvatarConfigurationPlan& ReviewedPlan, FString& OutError);
	static bool AnalyzeRestore(const FString& ProjectDirectory, FConvaiAvatarConfigurationPlan& OutPlan, FString& OutError);
	static bool Restore(const FConvaiAvatarConfigurationPlan& ReviewedPlan, FString& OutError);
	static bool HasBackup(const FString& ProjectDirectory);
	/** Changes only this plugin's host cook exclusion; does not serialize the packaging settings CDO. */
	static bool SetStagingCookExclusion(const FString& ProjectDirectory, const FString& PluginName, bool bExclude, FString& OutError);
	/** Reads only this owned avatar's generated child rules or its legacy whole-root rule. */
	static bool HasStagingCookExclusion(const FString& ProjectDirectory, const FString& PluginName, bool& bOutExcluded, FString& OutError);
	/** Startup metadata-only migration; no-op unless a verified staging record has a legacy whole-root rule. */
	static bool MigrateLegacyStagingCookExclusion(const FString& ProjectDirectory, const FString& PluginName, FString& OutError);
	/** Writes the same reviewed profile to the owned shared CA uploader project only; never reloads host config.
	 * Optional selected plugin adds proxy-only map/cook isolation. Unrelated INI sections and bytes are preserved. */
	static bool ApplyProxyProfile(const FString& ProxyProjectDirectory, FString& OutError, const FString& SelectedPluginName = FString(),
		const FString& ProfileJson = FString(), const FString& Configuration = FString());
	/** Validated codec arguments only; never accepts arbitrary command-line text from a remote profile. */
	static bool GetPakCompressionArguments(const FString& ProfileJson, const FString& Configuration, FString& OutArguments, FString& OutError);
	static bool IsSupportedConfiguration(const FString& Configuration);
	static FString GetBackupDirectory(const FString& ProjectDirectory);
	static FString GetSourceUrl();
	static FString GetProfileId();
	/** Used for offline regression tests and audit; only supported file/section/key/value combinations pass. */
	static bool ValidateProfile(const FString& ProfileJson, FString& OutError);
};
