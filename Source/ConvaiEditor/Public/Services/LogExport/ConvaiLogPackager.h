/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiLogPackager.h
 *
 * Packages log files for export.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiLogCollector.h"

struct FConvaiPackageResult
{
	/** Whether the operation was successful */
	bool bSuccess = false;

	/** Path to the generated package (folder or ZIP) */
	FString PackagePath;

	/** Error message if operation failed */
	FString ErrorMessage;

	/** Total number of files included */
	int32 TotalFiles = 0;

	/** Total size in bytes */
	int64 TotalSizeBytes = 0;

	/** Time taken to create package */
	double ElapsedSeconds = 0.0;

	FConvaiPackageResult() = default;

	/** Create a success result */
	static FConvaiPackageResult Success(const FString &InPath, int32 InFiles, int64 InSize, double InElapsed)
	{
		FConvaiPackageResult Result;
		Result.bSuccess = true;
		Result.PackagePath = InPath;
		Result.TotalFiles = InFiles;
		Result.TotalSizeBytes = InSize;
		Result.ElapsedSeconds = InElapsed;
		return Result;
	}

	/** Create a failure result */
	static FConvaiPackageResult Failure(const FString &InError)
	{
		FConvaiPackageResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = InError;
		return Result;
	}
};

/**
 * Packages log files and metadata into a structured archive
 *
 * Creates organized folder structure:
 * ConvaiLogExport_TIMESTAMP/
 *   ├── Manifest.json (summary of all collected info)
 *   ├── ConvaiLogs/ (Convai plugin logs)
 *   ├── EngineLogs/ (UE logs)
 *   ├── CrashLogs/ (crash reports if any)
 *   ├── Config/ (sanitized config files)
 *   └── SystemInfo.json (all collected metadata)
 *
 * Optionally creates ZIP archive on supported platforms
 *
 * Single Responsibility: Only handles packaging and archiving
 */
class FConvaiLogPackager
{
public:
	FConvaiLogPackager() = default;
	~FConvaiLogPackager() = default;

	/**
	 * Create a log package with all provided data
	 * @param LogFiles Array of log files to include
	 * @param Metadata JSON object containing all collected metadata
	 * @param bCreateZip Whether to attempt creating a ZIP archive (platform-dependent)
	 * @return Package result with path and statistics
	 */
	FConvaiPackageResult CreatePackage(
		const TArray<FConvaiLogFileInfo> &LogFiles,
		const TSharedPtr<FJsonObject> &Metadata,
		bool bCreateZip = true) const;

private:
	/** Generate unique package folder name with timestamp */
	FString GeneratePackageFolderName() const;

	/** Create the base folder structure */
	bool CreateFolderStructure(const FString &BaseDir, FString &OutError) const;

	/** Copy all log files to appropriate directories */
	bool CopyLogFiles(const FString &BaseDir, const TArray<FConvaiLogFileInfo> &LogFiles, FString &OutError) const;

	/** Write manifest.json file */
	bool WriteManifest(const FString &BaseDir, const TArray<FConvaiLogFileInfo> &LogFiles, FString &OutError) const;

	/** Write SystemInfo.json file */
	bool WriteMetadata(const FString &BaseDir, const TSharedPtr<FJsonObject> &Metadata, FString &OutError) const;

	/** Attempt to create ZIP archive (platform-specific) */
	bool CreateZipArchive(const FString &SourceDir, FString &OutZipPath, FString &OutError) const;

	/** Helper: Write JSON to file */
	bool WriteJsonToFile(const FString &FilePath, const TSharedPtr<FJsonObject> &JsonObject, FString &OutError) const;

	/** Helper: Copy single file */
	bool CopyFile(const FString &SourcePath, const FString &DestPath, FString &OutError) const;

	/** Helper: Calculate total size of files */
	int64 CalculateTotalSize(const TArray<FConvaiLogFileInfo> &LogFiles) const;
};
