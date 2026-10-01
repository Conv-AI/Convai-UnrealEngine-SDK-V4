/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiLogPackager.cpp
 *
 * Implementation of log packaging service.
 */

#include "Services/LogExport/ConvaiLogPackager.h"
#include "ConvaiEditor.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "GenericPlatform/GenericPlatformFile.h"

FConvaiPackageResult FConvaiLogPackager::CreatePackage(
	const TArray<FConvaiLogFileInfo> &LogFiles,
	const TSharedPtr<FJsonObject> &Metadata,
	bool bCreateZip) const
{
	double StartTime = FPlatformTime::Seconds();

	FString PackageFolderName = GeneratePackageFolderName();
	FString BaseDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ConvaiLogExports"), PackageFolderName);

	FString Error;

	if (!CreateFolderStructure(BaseDir, Error))
	{
		return FConvaiPackageResult::Failure(FString::Printf(TEXT("Failed to create folder structure: %s"), *Error));
	}

	if (!CopyLogFiles(BaseDir, LogFiles, Error))
	{
		return FConvaiPackageResult::Failure(FString::Printf(TEXT("Failed to copy log files: %s"), *Error));
	}

	if (!WriteManifest(BaseDir, LogFiles, Error))
	{
		return FConvaiPackageResult::Failure(FString::Printf(TEXT("Failed to write manifest: %s"), *Error));
	}

	if (!WriteMetadata(BaseDir, Metadata, Error))
	{
		return FConvaiPackageResult::Failure(FString::Printf(TEXT("Failed to write metadata: %s"), *Error));
	}

	FString FinalPath = BaseDir;
	int64 TotalSize = CalculateTotalSize(LogFiles);

	if (bCreateZip)
	{
		FString ZipPath;
		if (CreateZipArchive(BaseDir, ZipPath, Error))
		{
			FinalPath = ZipPath;
		}
	}

	double ElapsedTime = FPlatformTime::Seconds() - StartTime;

	return FConvaiPackageResult::Success(FinalPath, LogFiles.Num(), TotalSize, ElapsedTime);
}

FString FConvaiLogPackager::GeneratePackageFolderName() const
{
	FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	return FString::Printf(TEXT("ConvaiLogExport_%s"), *Timestamp);
}

bool FConvaiLogPackager::CreateFolderStructure(const FString &BaseDir, FString &OutError) const
{
	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.CreateDirectoryTree(*BaseDir))
	{
		OutError = FString::Printf(TEXT("Failed to create base directory: %s"), *BaseDir);
		return false;
	}

	TArray<FString> SubDirs = {
		TEXT("ConvaiLogs"),
		TEXT("EngineLogs"),
		TEXT("CrashLogs"),
		TEXT("Config")};

	for (const FString &SubDir : SubDirs)
	{
		FString FullPath = FPaths::Combine(BaseDir, SubDir);
		if (!PlatformFile.CreateDirectory(*FullPath))
		{
			OutError = FString::Printf(TEXT("Failed to create subdirectory: %s"), *FullPath);
			return false;
		}
	}

	return true;
}

bool FConvaiLogPackager::CopyLogFiles(const FString &BaseDir, const TArray<FConvaiLogFileInfo> &LogFiles, FString &OutError) const
{
	for (const FConvaiLogFileInfo &LogFile : LogFiles)
	{
		FString DestPath = FPaths::Combine(BaseDir, LogFile.ArchivePath);

		FString DestDir = FPaths::GetPath(DestPath);
		IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*DestDir))
		{
			if (!PlatformFile.CreateDirectoryTree(*DestDir))
			{
				OutError = FString::Printf(TEXT("Failed to create directory: %s"), *DestDir);
				return false;
			}
		}

		if (!CopyFile(LogFile.SourcePath, DestPath, OutError))
		{
			UE_LOG(LogConvaiEditor, Warning, TEXT("Failed to copy file %s: %s"), *LogFile.SourcePath, *OutError);
			continue;
		}
	}

	return true;
}

bool FConvaiLogPackager::WriteManifest(const FString &BaseDir, const TArray<FConvaiLogFileInfo> &LogFiles, FString &OutError) const
{
	TSharedPtr<FJsonObject> ManifestObj = MakeShared<FJsonObject>();

	ManifestObj->SetStringField(TEXT("PackageName"), FPaths::GetCleanFilename(BaseDir));
	ManifestObj->SetStringField(TEXT("CreatedAt"), FDateTime::UtcNow().ToIso8601());
	ManifestObj->SetStringField(TEXT("ProjectName"), FApp::GetProjectName());
	ManifestObj->SetNumberField(TEXT("TotalFiles"), LogFiles.Num());
	ManifestObj->SetNumberField(TEXT("TotalSizeBytes"), CalculateTotalSize(LogFiles));

	TMap<FString, int32> CategoryCounts;
	for (const FConvaiLogFileInfo &LogFile : LogFiles)
	{
		CategoryCounts.FindOrAdd(LogFile.Category, 0)++;
	}

	TSharedPtr<FJsonObject> CategoriesObj = MakeShared<FJsonObject>();
	for (const auto &Pair : CategoryCounts)
	{
		CategoriesObj->SetNumberField(Pair.Key, Pair.Value);
	}
	ManifestObj->SetObjectField(TEXT("Categories"), CategoriesObj);

	TArray<TSharedPtr<FJsonValue>> FileArray;
	for (const FConvaiLogFileInfo &LogFile : LogFiles)
	{
		TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
		FileObj->SetStringField(TEXT("Path"), LogFile.ArchivePath);
		FileObj->SetStringField(TEXT("Category"), LogFile.Category);
		FileObj->SetNumberField(TEXT("SizeBytes"), LogFile.FileSizeBytes);
		FileObj->SetBoolField(TEXT("Critical"), LogFile.bIsCritical);
		FileArray.Add(MakeShared<FJsonValueObject>(FileObj));
	}
	ManifestObj->SetArrayField(TEXT("Files"), FileArray);

	ManifestObj->SetStringField(TEXT("Instructions"),
								TEXT("This package contains diagnostic information for Convai support. ")
									TEXT("SystemInfo.json contains system and project metadata. ")
										TEXT("Log files are organized by category in their respective folders."));

	FString ManifestPath = FPaths::Combine(BaseDir, TEXT("Manifest.json"));
	return WriteJsonToFile(ManifestPath, ManifestObj, OutError);
}

bool FConvaiLogPackager::WriteMetadata(const FString &BaseDir, const TSharedPtr<FJsonObject> &Metadata, FString &OutError) const
{
	if (!Metadata.IsValid())
	{
		OutError = TEXT("Invalid metadata object");
		return false;
	}

	FString MetadataPath = FPaths::Combine(BaseDir, TEXT("SystemInfo.json"));
	return WriteJsonToFile(MetadataPath, Metadata, OutError);
}

bool FConvaiLogPackager::CreateZipArchive(const FString &SourceDir, FString &OutZipPath, FString &OutError) const
{
#if PLATFORM_WINDOWS
	FString ZipPath = SourceDir + TEXT(".zip");

	FString PowerShellCommand = FString::Printf(
		TEXT("-WindowStyle Hidden -NoProfile -ExecutionPolicy Bypass -Command \"Compress-Archive -Path '%s\\*' -DestinationPath '%s' -CompressionLevel Optimal -Force\""),
		*SourceDir,
		*ZipPath);

	void *ReadPipe = nullptr;
	void *WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		TEXT("powershell.exe"),
		*PowerShellCommand,
		false,
		true,
		true,
		nullptr,
		0,
		nullptr,
		WritePipe,
		ReadPipe);

	if (!ProcHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		OutError = TEXT("Failed to start compression process");
		return false;
	}

	FPlatformProcess::WaitForProc(ProcHandle);

	int32 ReturnCode = 0;
	FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);

	FString StdOut = FPlatformProcess::ReadPipe(ReadPipe);

	FPlatformProcess::CloseProc(ProcHandle);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*ZipPath))
	{
		OutZipPath = ZipPath;
		return true;
	}

	OutError = FString::Printf(TEXT("Compression failed. Return code: %d, Output: %s"), ReturnCode, *StdOut);
	return false;

#elif PLATFORM_MAC
	FString ZipPath = SourceDir + TEXT(".zip");
	FString Command = FString::Printf(TEXT("-c \"cd '%s' && zip -r '%s.zip' . -q\""), *FPaths::GetPath(SourceDir), *FPaths::GetBaseFilename(SourceDir));

	void *ReadPipe = nullptr;
	void *WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		TEXT("/bin/sh"),
		*Command,
		false,
		true,
		true,
		nullptr,
		0,
		nullptr,
		WritePipe,
		ReadPipe);

	if (!ProcHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		OutError = TEXT("Failed to start compression process");
		return false;
	}

	FPlatformProcess::WaitForProc(ProcHandle);

	int32 ReturnCode = 0;
	FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);

	FPlatformProcess::CloseProc(ProcHandle);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*ZipPath))
	{
		OutZipPath = ZipPath;
		return true;
	}

	OutError = FString::Printf(TEXT("Compression failed. Return code: %d"), ReturnCode);
	return false;

#elif PLATFORM_LINUX
	FString ZipPath = SourceDir + TEXT(".zip");
	FString Command = FString::Printf(TEXT("-c \"cd '%s' && zip -r '%s.zip' . -q\""), *FPaths::GetPath(SourceDir), *FPaths::GetBaseFilename(SourceDir));

	void *ReadPipe = nullptr;
	void *WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		TEXT("/bin/sh"),
		*Command,
		false,
		true,
		true,
		nullptr,
		0,
		nullptr,
		WritePipe,
		ReadPipe);

	if (!ProcHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		OutError = TEXT("Failed to start compression process. Ensure 'zip' is installed.");
		return false;
	}

	FPlatformProcess::WaitForProc(ProcHandle);

	int32 ReturnCode = 0;
	FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);

	FPlatformProcess::CloseProc(ProcHandle);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*ZipPath))
	{
		OutZipPath = ZipPath;
		return true;
	}

	OutError = FString::Printf(TEXT("Compression failed. Return code: %d"), ReturnCode);
	return false;

#else
	OutError = TEXT("ZIP compression not supported on this platform");
	return false;
#endif
}

bool FConvaiLogPackager::WriteJsonToFile(const FString &FilePath, const TSharedPtr<FJsonObject> &JsonObject, FString &OutError) const
{
	FString JsonString;
	TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&JsonString);

	if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), JsonWriter))
	{
		OutError = TEXT("Failed to serialize JSON");
		return false;
	}

	if (!FFileHelper::SaveStringToFile(JsonString, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write file: %s"), *FilePath);
		return false;
	}

	return true;
}

bool FConvaiLogPackager::CopyFile(const FString &SourcePath, const FString &DestPath, FString &OutError) const
{
	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.FileExists(*SourcePath))
	{
		OutError = FString::Printf(TEXT("Source file does not exist: %s"), *SourcePath);
		return false;
	}

	if (!PlatformFile.CopyFile(*DestPath, *SourcePath))
	{
		OutError = FString::Printf(TEXT("Failed to copy file from %s to %s"), *SourcePath, *DestPath);
		return false;
	}

	return true;
}

int64 FConvaiLogPackager::CalculateTotalSize(const TArray<FConvaiLogFileInfo> &LogFiles) const
{
	int64 TotalSize = 0;
	for (const FConvaiLogFileInfo &LogFile : LogFiles)
	{
		TotalSize += LogFile.FileSizeBytes;
	}
	return TotalSize;
}
