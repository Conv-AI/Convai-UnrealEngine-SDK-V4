/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IUpdateCheckService.h
 *
 * Service interface for checking plugin updates from GitHub releases.
 * Follows SOLID principles with clean separation of concerns.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Async/Future.h"
#include "Models/UpdateCheckModels.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FOnUpdateCheckComplete, const FUpdateCheckResult &);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnUpdateAvailabilityChanged, bool /* bUpdateAvailable */, const FString & /* LatestVersion */);

/**
 * Interface for checking plugin updates from GitHub.
 */
class CONVAIEDITOR_API IUpdateCheckService : public IConvaiService
{
public:
	virtual ~IUpdateCheckService() = default;

	virtual TFuture<FUpdateCheckResult> CheckForUpdatesAsync(bool bForceRefresh = false) = 0;
	virtual FUpdateCheckResult GetLastCheckResult() const = 0;
	virtual bool IsUpdateAvailable() const = 0;
	virtual FString GetLatestVersionString() const = 0;

	/**
	 * Gets the current installed plugin version.
	 *
	 * @return Current plugin version info
	 */
	virtual FPluginVersionInfo GetCurrentVersion() const = 0;

	/**
	 * Opens the GitHub releases page in the default browser.
	 */
	virtual void OpenReleasesPage() const = 0;

	/**
	 * Gets the time since the last successful check.
	 *
	 * @return Time in seconds since last check, or -1 if never checked
	 */
	virtual double GetTimeSinceLastCheck() const = 0;

	/**
	 * Clears the cached update check data.
	 */
	virtual void ClearCache() = 0;

	/**
	 * Marks the specified version as acknowledged by the user so that UI
	 * indicators stop highlighting it until a newer version is detected.
	 * @param VersionString Version string to acknowledge (e.g., "3.6.8")
	 */
	virtual void AcknowledgeUpdate(const FString &VersionString) = 0;

	/**
	 * Delegate called when an update check completes.
	 */
	virtual FOnUpdateCheckComplete &OnUpdateCheckComplete() = 0;

	/**
	 * Delegate called when update availability changes.
	 */
	virtual FOnUpdateAvailabilityChanged &OnUpdateAvailabilityChanged() = 0;
};
