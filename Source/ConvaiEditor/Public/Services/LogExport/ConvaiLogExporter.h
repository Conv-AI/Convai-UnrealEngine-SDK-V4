/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiLogExporter.h
 *
 * Log export service.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiLogPackager.h"
#include "IConvaiInfoCollector.h"

// Forward declarations
struct FConvaiIssueReport;

struct FConvaiLogExportOptions
{
	/** Whether to create a ZIP archive (recommended for easy sharing) */
	bool bCreateZipArchive = true;

	/** Whether to include crash logs */
	bool bIncludeCrashLogs = true;

	/** Whether to include engine logs */
	bool bIncludeEngineLogs = true;

	/** Whether to open the export location after completion */
	bool bOpenLocationAfterExport = true;

	/** Maximum age of logs to include (in hours, 0 = all) */
	int32 MaxLogAgeHours = 24;

	/** Whether to capture screenshots */
	bool bCaptureScreenshots = true;

	/** Whether to include network diagnostics */
	bool bIncludeNetworkDiagnostics = true;

	/** Whether to include performance metrics */
	bool bIncludePerformanceMetrics = true;

	/** User's issue report (from dialog) */
	TSharedPtr<FConvaiIssueReport> UserReport;

	FConvaiLogExportOptions() = default;

	/** Default export options */
	static FConvaiLogExportOptions Default()
	{
		return FConvaiLogExportOptions();
	}
};

/**
 * Main facade class for log export system.
 */
class CONVAIEDITOR_API FConvaiLogExporter
{
public:
	/** Progress callback signature: (CurrentStep, TotalSteps, StatusMessage) */
	DECLARE_DELEGATE_ThreeParams(FOnExportProgress, int32, int32, const FString &);

	FConvaiLogExporter();
	~FConvaiLogExporter() = default;

	/**
	 * Execute the log export process
	 * @param Options Export configuration options
	 * @param ProgressCallback Optional callback for progress updates
	 * @return Package result with path and statistics
	 */
	FConvaiPackageResult ExportLogs(
		const FConvaiLogExportOptions &Options = FConvaiLogExportOptions::Default(),
		FOnExportProgress ProgressCallback = FOnExportProgress());

	/**
	 * Execute log export asynchronously (on background thread)
	 * @param Options Export configuration options
	 * @param CompletionCallback Callback when export is complete (called on game thread)
	 */
	void ExportLogsAsync(
		const FConvaiLogExportOptions &Options,
		TFunction<void(const FConvaiPackageResult &)> CompletionCallback);

	/**
	 * Get singleton instance
	 * @return Reference to global log exporter
	 */
	static FConvaiLogExporter &Get();

private:
	/** Collect all metadata from registered collectors */
	TSharedPtr<FJsonObject> CollectAllMetadata(FOnExportProgress &ProgressCallback, int32 &CurrentStep, int32 TotalSteps) const;

	/** Collect all log files */
	TArray<FConvaiLogFileInfo> CollectAllLogFiles(const FConvaiLogExportOptions &Options, FOnExportProgress &ProgressCallback, int32 &CurrentStep, int32 TotalSteps) const;

	/** Create the final package */
	FConvaiPackageResult CreatePackage(
		const TArray<FConvaiLogFileInfo> &LogFiles,
		const TSharedPtr<FJsonObject> &Metadata,
		const FConvaiLogExportOptions &Options,
		FOnExportProgress &ProgressCallback,
		int32 &CurrentStep,
		int32 TotalSteps) const;

	/** Open the export location in system file explorer */
	void OpenExportLocation(const FString &Path) const;

	/** Information collectors (using Dependency Injection) */
	TArray<TSharedPtr<IConvaiInfoCollector>> Collectors;

	/** Log packager */
	TUniquePtr<FConvaiLogPackager> Packager;

	/** Singleton instance */
	static TSharedPtr<FConvaiLogExporter> Instance;
};
