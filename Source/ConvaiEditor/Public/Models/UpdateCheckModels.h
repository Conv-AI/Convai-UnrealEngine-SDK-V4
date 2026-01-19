/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * UpdateCheckModels.h
 *
 * Data models for the update check system.
 */

#pragma once

#include "CoreMinimal.h"
#include "Utility/ContentFilteringUtility.h"

/** Version information for the Convai plugin. */
struct CONVAIEDITOR_API FPluginVersionInfo
{
	FSemanticVersion Version;
	FString VersionString;
	FString FriendlyName;

	FPluginVersionInfo()
		: Version(), VersionString(TEXT("")), FriendlyName(TEXT(""))
	{
	}

	FPluginVersionInfo(const FSemanticVersion &InVersion, const FString &InVersionString, const FString &InFriendlyName)
		: Version(InVersion), VersionString(InVersionString), FriendlyName(InFriendlyName)
	{
	}

	bool IsValid() const
	{
		return Version.IsValid() && !VersionString.IsEmpty();
	}
};

/** Parsed information from a GitHub release. */
struct CONVAIEDITOR_API FGitHubReleaseInfo
{
	FString TagName;
	FString ReleaseName;
	FString Description;
	FString ReleaseUrl;
	bool bIsPreRelease;
	bool bIsDraft;
	FDateTime PublishedAt;
	FSemanticVersion Version;

	FGitHubReleaseInfo()
		: TagName(TEXT("")), ReleaseName(TEXT("")), Description(TEXT("")),
		  ReleaseUrl(TEXT("")), bIsPreRelease(false), bIsDraft(false),
		  PublishedAt(0), Version()
	{
	}

	bool IsValid() const
	{
		return !TagName.IsEmpty() && Version.IsValid();
	}

	static FSemanticVersion ParseVersionFromTag(const FString &TagName)
	{
		FString VersionString = TagName;

		if (VersionString.StartsWith(TEXT("v")) || VersionString.StartsWith(TEXT("V")))
		{
			VersionString = VersionString.RightChop(1);
		}

		return FSemanticVersion::Parse(VersionString);
	}
};

/** Status of an update check operation. */
enum class EUpdateCheckStatus : uint8
{
	NotChecked,
	InProgress,
	UpdateAvailable,
	UpToDate,
	NetworkError,
	ParseError,
	UnknownError
};

/** Result of an update check operation. */
struct CONVAIEDITOR_API FUpdateCheckResult
{
	EUpdateCheckStatus Status;
	FPluginVersionInfo CurrentVersion;
	FPluginVersionInfo LatestVersion;
	FGitHubReleaseInfo LatestRelease;
	FString ErrorMessage;
	FDateTime CheckTimestamp;
	bool bUpdateAvailable;

	FUpdateCheckResult()
		: Status(EUpdateCheckStatus::NotChecked), CurrentVersion(), LatestVersion(),
		  LatestRelease(), ErrorMessage(TEXT("")), CheckTimestamp(0), bUpdateAvailable(false)
	{
	}

	static FUpdateCheckResult UpdateAvailable(
		const FPluginVersionInfo &InCurrentVersion,
		const FPluginVersionInfo &InLatestVersion,
		const FGitHubReleaseInfo &InReleaseInfo)
	{
		FUpdateCheckResult Result;
		Result.Status = EUpdateCheckStatus::UpdateAvailable;
		Result.CurrentVersion = InCurrentVersion;
		Result.LatestVersion = InLatestVersion;
		Result.LatestRelease = InReleaseInfo;
		Result.CheckTimestamp = FDateTime::UtcNow();
		Result.bUpdateAvailable = true;
		return Result;
	}

	static FUpdateCheckResult UpToDate(const FPluginVersionInfo &InCurrentVersion)
	{
		FUpdateCheckResult Result;
		Result.Status = EUpdateCheckStatus::UpToDate;
		Result.CurrentVersion = InCurrentVersion;
		Result.CheckTimestamp = FDateTime::UtcNow();
		Result.bUpdateAvailable = false;
		return Result;
	}

	static FUpdateCheckResult Error(EUpdateCheckStatus InStatus, const FString &InErrorMessage)
	{
		FUpdateCheckResult Result;
		Result.Status = InStatus;
		Result.ErrorMessage = InErrorMessage;
		Result.CheckTimestamp = FDateTime::UtcNow();
		Result.bUpdateAvailable = false;
		return Result;
	}

	bool IsValid() const
	{
		return Status != EUpdateCheckStatus::NotChecked;
	}

	bool IsSuccess() const
	{
		return Status == EUpdateCheckStatus::UpdateAvailable || Status == EUpdateCheckStatus::UpToDate;
	}

	FString GetStatusMessage() const
	{
		switch (Status)
		{
		case EUpdateCheckStatus::NotChecked:
			return TEXT("Update check has not been performed yet");
		case EUpdateCheckStatus::InProgress:
			return TEXT("Checking for updates...");
		case EUpdateCheckStatus::UpdateAvailable:
			return FString::Printf(TEXT("Update available: v%s → v%s"),
								   *CurrentVersion.VersionString, *LatestVersion.VersionString);
		case EUpdateCheckStatus::UpToDate:
			return FString::Printf(TEXT("You're up to date (v%s)"), *CurrentVersion.VersionString);
		case EUpdateCheckStatus::NetworkError:
			return FString::Printf(TEXT("Network error: %s"), *ErrorMessage);
		case EUpdateCheckStatus::ParseError:
			return FString::Printf(TEXT("Failed to parse update data: %s"), *ErrorMessage);
		case EUpdateCheckStatus::UnknownError:
		default:
			return FString::Printf(TEXT("Error checking for updates: %s"), *ErrorMessage);
		}
	}
};

/** Configuration for the update check service. */
struct CONVAIEDITOR_API FUpdateCheckConfig
{
	FString GitHubLatestApiUrl;
	FString GitHubAllReleasesApiUrl;
	FString GitHubReleasesUrl;
	float TimeoutSeconds;
	int32 MaxRetries;
	float RetryDelaySeconds;
	float CacheTTLSeconds;
	bool bAutoCheckOnStartup;
	bool bIncludePreReleases;

	static FUpdateCheckConfig Default()
	{
		FUpdateCheckConfig Config;
		Config.GitHubLatestApiUrl = TEXT("https://api.github.com/repos/Conv-AI/Convai-UnrealEngine-SDK/releases/latest");
		Config.GitHubAllReleasesApiUrl = TEXT("https://api.github.com/repos/Conv-AI/Convai-UnrealEngine-SDK/releases");
		Config.GitHubReleasesUrl = TEXT("https://github.com/Conv-AI/Convai-UnrealEngine-SDK/releases");
		Config.TimeoutSeconds = 10.0f;
		Config.MaxRetries = 2;
		Config.RetryDelaySeconds = 2.0f;
		Config.CacheTTLSeconds = 3600.0f;
		Config.bAutoCheckOnStartup = true;
		Config.bIncludePreReleases = false;
		return Config;
	}
};
