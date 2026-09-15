/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiLogCollector.h
 *
 * Collects log files from various sources.
 */

#pragma once

#include "CoreMinimal.h"
#include "IConvaiInfoCollector.h"

/**
 * Log file information.
 */
struct FConvaiLogFileInfo
{
	FString SourcePath;
	FString ArchivePath;
	FString Category;
	int64 FileSizeBytes = 0;
	FDateTime LastModified;
	bool bIsCritical = false;

	FConvaiLogFileInfo() = default;

	FConvaiLogFileInfo(const FString &InSourcePath, const FString &InArchivePath, const FString &InCategory, bool bInIsCritical = false)
		: SourcePath(InSourcePath), ArchivePath(InArchivePath), Category(InCategory), bIsCritical(bInIsCritical)
	{
	}
};

/**
 * Collects log files from various sources.
 */
class FConvaiLogCollector : public IConvaiInfoCollector
{
public:
	FConvaiLogCollector() = default;
	virtual ~FConvaiLogCollector() = default;

	// IConvaiInfoCollector interface
	virtual TSharedPtr<FJsonObject> CollectInfo() const override;
	virtual FString GetCollectorName() const override { return TEXT("LogFiles"); }
	virtual bool IsAvailable() const override { return true; }

	/**
	 * Get list of all log files to be exported
	 * @return Array of log file information structures
	 */
	TArray<FConvaiLogFileInfo> GetLogFiles() const;

private:
	/** Collect Convai-specific log files */
	void CollectConvaiLogs(TArray<FConvaiLogFileInfo> &OutLogFiles) const;

	/** Collect Unreal Engine log files (filtered to recent/relevant) */
	void CollectEngineLogs(TArray<FConvaiLogFileInfo> &OutLogFiles) const;

	/** Collect crash report files if available */
	void CollectCrashLogs(TArray<FConvaiLogFileInfo> &OutLogFiles) const;

	/** Collect relevant config files (sanitized) */
	void CollectConfigFiles(TArray<FConvaiLogFileInfo> &OutLogFiles) const;

	/** Helper: Get all files in directory matching pattern, sorted by modification time (newest first) */
	void GetFilesInDirectory(const FString &Directory, const FString &Pattern, TArray<FString> &OutFiles, int32 MaxFiles = 10) const;

	/** Helper: Get file info (size, modification time) */
	bool GetFileInfo(const FString &FilePath, int64 &OutSize, FDateTime &OutModificationTime) const;

	/** Helper: Check if a log file is recent enough to include (within last 24 hours) */
	bool IsRecentLogFile(const FDateTime &ModificationTime) const;
};
