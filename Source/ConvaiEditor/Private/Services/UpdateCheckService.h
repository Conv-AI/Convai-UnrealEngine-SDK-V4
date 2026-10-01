/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * UpdateCheckService.h
 *
 * Manages checking for plugin updates from GitHub releases.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/IUpdateCheckService.h"
#include "Http.h"
#include "Async/Future.h"
#include "Utility/CircuitBreaker.h"
#include "Utility/RetryPolicy.h"
#include <atomic>

namespace ConvaiEditor
{
    template<typename T>
    class FAsyncOperation;
    struct FHttpAsyncResponse;
}

/**
 * Manages checking for plugin updates from GitHub releases.
 */
class FUpdateCheckService : public IUpdateCheckService, public TSharedFromThis<FUpdateCheckService>
{
public:
	FUpdateCheckService();
	explicit FUpdateCheckService(const FUpdateCheckConfig &InConfig);
	virtual ~FUpdateCheckService();

	/** Checks for available updates asynchronously */
	virtual TFuture<FUpdateCheckResult> CheckForUpdatesAsync(bool bForceRefresh = false) override;

	/** Returns the last check result */
	virtual FUpdateCheckResult GetLastCheckResult() const override;

	/** Returns whether an update is available */
	virtual bool IsUpdateAvailable() const override;

	/** Returns the latest version string */
	virtual FString GetLatestVersionString() const override;

	/** Returns the current plugin version */
	virtual FPluginVersionInfo GetCurrentVersion() const override;

	/** Opens the GitHub releases page in browser */
	virtual void OpenReleasesPage() const override;

	/** Returns time since last check in seconds */
	virtual double GetTimeSinceLastCheck() const override;

	/** Clears the update check cache */
	virtual void ClearCache() override;

	/** Marks the specified version as acknowledged by user */
	virtual void AcknowledgeUpdate(const FString &VersionString) override;

	/** Returns the update check complete event delegate */
	virtual FOnUpdateCheckComplete &OnUpdateCheckComplete() override { return UpdateCheckCompleteDelegate; }

	/** Returns the update availability changed event delegate */
	virtual FOnUpdateAvailabilityChanged &OnUpdateAvailabilityChanged() override { return UpdateAvailabilityChangedDelegate; }

	/** Initializes the service */
	virtual void Startup() override;

	/** Shuts down the service */
	virtual void Shutdown() override;

private:
	FUpdateCheckConfig Config;
	FUpdateCheckResult CachedResult;
	FDateTime LastCheckTimestamp;
	bool bCheckInProgress;
	bool bUsingLatestEndpoint;
	std::atomic<bool> bIsShuttingDown{false};
	FPluginVersionInfo CurrentVersionCache;
	mutable FCriticalSection CacheLock;
	FString LastAcknowledgedVersion;
	TArray<TSharedPtr<ConvaiEditor::FAsyncOperation<ConvaiEditor::FHttpAsyncResponse>>> ActiveOperations;

	static const TCHAR *ConfigSection;
	static const TCHAR *ConfigKeyLastAcknowledged;

	/** Returns the plugin's config file path */
	static FString GetPluginConfigPath();

	/** Loads last acknowledged version from config */
	void LoadAcknowledgedState();

	/** Saves last acknowledged version to config */
	void SaveAcknowledgedState() const;

	FOnUpdateCheckComplete UpdateCheckCompleteDelegate;
	FOnUpdateAvailabilityChanged UpdateAvailabilityChangedDelegate;

	/**
	 * Performs the HTTP request to GitHub API.
	 * @param Promise Promise to fulfill with the result
	 * @param AttemptNumber Current attempt number for retry logic
	 */
	void PerformUpdateCheck(TSharedPtr<TPromise<FUpdateCheckResult>> Promise, int32 AttemptNumber);

	/**
	 * Parses JSON response from GitHub releases API.
	 * @param JsonString Raw JSON response
	 * @param OutReleases Parsed release information
	 * @return True if parsing succeeded
	 */
	bool ParseGitHubReleasesJson(const FString &JsonString, TArray<FGitHubReleaseInfo> &OutReleases) const;

	/**
	 * Finds the latest applicable release.
	 * @param Releases List of all releases
	 * @return Latest applicable release
	 */
	FGitHubReleaseInfo FindLatestRelease(const TArray<FGitHubReleaseInfo> &Releases) const;

	/** Creates result by comparing current version with latest */
	FUpdateCheckResult CreateResultFromComparison(const FGitHubReleaseInfo &LatestRelease) const;

	/** Loads current plugin version information */
	FPluginVersionInfo LoadCurrentVersion() const;

	/** Checks if cached result is still valid */
	bool IsCacheValid() const;

	/** Updates cache with new result */
	void UpdateCache(const FUpdateCheckResult &Result);

	/** Notifies delegates of result changes */
	void NotifyDelegates(const FUpdateCheckResult &Result);

	TUniquePtr<ConvaiEditor::FCircuitBreaker> CircuitBreaker;
	TUniquePtr<ConvaiEditor::FRetryPolicy> RetryPolicy;
};
