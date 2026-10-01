/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiLogCollector.cpp
 *
 * Implementation of log file collection service.
 */

#include "Services/LogExport/ConvaiLogCollector.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "GenericPlatform/GenericPlatformFile.h"

TSharedPtr<FJsonObject> FConvaiLogCollector::CollectInfo() const
{
	TSharedPtr<FJsonObject> LogInfo = MakeShared<FJsonObject>();

	TArray<FConvaiLogFileInfo> LogFiles = GetLogFiles();

	TMap<FString, int32> CategoryCounts;
	int64 TotalSize = 0;
	int32 CriticalCount = 0;

	TArray<TSharedPtr<FJsonValue>> FileArray;

	for (const FConvaiLogFileInfo &LogFile : LogFiles)
	{
		TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
		FileObj->SetStringField(TEXT("SourcePath"), LogFile.SourcePath);
		FileObj->SetStringField(TEXT("ArchivePath"), LogFile.ArchivePath);
		FileObj->SetStringField(TEXT("Category"), LogFile.Category);
		FileObj->SetNumberField(TEXT("FileSizeBytes"), LogFile.FileSizeBytes);
		FileObj->SetStringField(TEXT("LastModified"), LogFile.LastModified.ToIso8601());
		FileObj->SetBoolField(TEXT("IsCritical"), LogFile.bIsCritical);

		FileArray.Add(MakeShared<FJsonValueObject>(FileObj));

		CategoryCounts.FindOrAdd(LogFile.Category, 0)++;
		TotalSize += LogFile.FileSizeBytes;
		if (LogFile.bIsCritical)
		{
			CriticalCount++;
		}
	}

	LogInfo->SetArrayField(TEXT("Files"), FileArray);

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("TotalFiles"), LogFiles.Num());
	Summary->SetNumberField(TEXT("TotalSizeBytes"), TotalSize);
	Summary->SetNumberField(TEXT("TotalSizeMB"), TotalSize / (1024.0 * 1024.0));
	Summary->SetNumberField(TEXT("CriticalFiles"), CriticalCount);

	TSharedPtr<FJsonObject> CategoryObj = MakeShared<FJsonObject>();
	for (const auto &Pair : CategoryCounts)
	{
		CategoryObj->SetNumberField(Pair.Key, Pair.Value);
	}
	Summary->SetObjectField(TEXT("CategoryBreakdown"), CategoryObj);

	LogInfo->SetObjectField(TEXT("Summary"), Summary);

	LogInfo->SetStringField(TEXT("CollectionTimestamp"), FDateTime::UtcNow().ToIso8601());

	return LogInfo;
}

TArray<FConvaiLogFileInfo> FConvaiLogCollector::GetLogFiles() const
{
	TArray<FConvaiLogFileInfo> AllLogFiles;

	CollectConvaiLogs(AllLogFiles);
	CollectEngineLogs(AllLogFiles);
	CollectCrashLogs(AllLogFiles);
	CollectConfigFiles(AllLogFiles);

	return AllLogFiles;
}

void FConvaiLogCollector::CollectConvaiLogs(TArray<FConvaiLogFileInfo> &OutLogFiles) const
{
	const FString ConvaiLogsDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved"), TEXT("ConvaiLogs"));

	TArray<FString> LogFiles;
	GetFilesInDirectory(ConvaiLogsDir, TEXT("*.log"), LogFiles, 100);

	for (const FString &LogFile : LogFiles)
	{
		int64 FileSize = 0;
		FDateTime ModTime;
		if (GetFileInfo(LogFile, FileSize, ModTime))
		{
			if (IsRecentLogFile(ModTime))
			{
				FString FileName = FPaths::GetCleanFilename(LogFile);
				FString ArchivePath = FPaths::Combine(TEXT("ConvaiLogs"), FileName);

				FConvaiLogFileInfo Info(LogFile, ArchivePath, TEXT("ConvaiLogs"), true);
				Info.FileSizeBytes = FileSize;
				Info.LastModified = ModTime;

				OutLogFiles.Add(Info);
			}
		}
	}
}

void FConvaiLogCollector::CollectEngineLogs(TArray<FConvaiLogFileInfo> &OutLogFiles) const
{
	const FString EngineLogsDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved"), TEXT("Logs"));

	TArray<FString> LogFiles;
	GetFilesInDirectory(EngineLogsDir, TEXT("*.log"), LogFiles, 50);

	for (const FString &LogFile : LogFiles)
	{
		int64 FileSize = 0;
		FDateTime ModTime;
		if (GetFileInfo(LogFile, FileSize, ModTime))
		{
			if (IsRecentLogFile(ModTime))
			{
				FString FileName = FPaths::GetCleanFilename(LogFile);
				FString ArchivePath = FPaths::Combine(TEXT("EngineLogs"), FileName);

				FConvaiLogFileInfo Info(LogFile, ArchivePath, TEXT("EngineLogs"), false);
				Info.FileSizeBytes = FileSize;
				Info.LastModified = ModTime;

				OutLogFiles.Add(Info);
			}
		}
	}
}

void FConvaiLogCollector::CollectCrashLogs(TArray<FConvaiLogFileInfo> &OutLogFiles) const
{
	const FString CrashLogsDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved"), TEXT("Crashes"));

	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*CrashLogsDir))
	{
		return;
	}
	TArray<FString> CrashFolders;
	PlatformFile.IterateDirectory(*CrashLogsDir, [&CrashFolders](const TCHAR *FilenameOrDirectory, bool bIsDirectory) -> bool
								  {
		if (bIsDirectory)
		{
			CrashFolders.Add(FilenameOrDirectory);
		}
		return true; });

	CrashFolders.Sort([&PlatformFile](const FString &A, const FString &B)
					  { return PlatformFile.GetTimeStamp(*A) > PlatformFile.GetTimeStamp(*B); });
	int32 CrashCount = 0;
	for (const FString &CrashFolder : CrashFolders)
	{
		if (CrashCount >= 5)
			break;

		TArray<FString> CrashFiles;
		PlatformFile.IterateDirectoryRecursively(*CrashFolder, [&CrashFiles](const TCHAR *FilenameOrDirectory, bool bIsDirectory) -> bool
												 {
			if (!bIsDirectory)
			{
				FString Extension = FPaths::GetExtension(FilenameOrDirectory);
				if (Extension == TEXT("log") || Extension == TEXT("txt") || Extension == TEXT("xml"))
				{
					CrashFiles.Add(FilenameOrDirectory);
				}
			}
			return true; });

		for (const FString &CrashFile : CrashFiles)
		{
			int64 FileSize = 0;
			FDateTime ModTime;
			if (GetFileInfo(CrashFile, FileSize, ModTime))
			{
				FString RelativePath = CrashFile;
				FPaths::MakePathRelativeTo(RelativePath, *CrashLogsDir);

				FString ArchivePath = FPaths::Combine(TEXT("CrashLogs"), RelativePath);

				FConvaiLogFileInfo Info(CrashFile, ArchivePath, TEXT("CrashLogs"), true);
				Info.FileSizeBytes = FileSize;
				Info.LastModified = ModTime;

				OutLogFiles.Add(Info);
			}
		}

		CrashCount++;
	}
}

void FConvaiLogCollector::CollectConfigFiles(TArray<FConvaiLogFileInfo> &OutLogFiles) const
{
	TArray<FString> ConfigFiles = {
		FPaths::Combine(FPaths::ProjectDir(), TEXT("Config"), TEXT("DefaultEngine.ini")),
		FPaths::Combine(FPaths::ProjectDir(), TEXT("Config"), TEXT("DefaultGame.ini")),
		FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins"), TEXT("Convai"), TEXT("Config"), TEXT("ConvaiEditorSettings.ini"))};

	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	for (const FString &ConfigFile : ConfigFiles)
	{
		if (PlatformFile.FileExists(*ConfigFile))
		{
			int64 FileSize = 0;
			FDateTime ModTime;
			if (GetFileInfo(ConfigFile, FileSize, ModTime))
			{
				FString FileName = FPaths::GetCleanFilename(ConfigFile);
				FString ArchivePath = FPaths::Combine(TEXT("Config"), FileName);

				FConvaiLogFileInfo Info(ConfigFile, ArchivePath, TEXT("Config"), false);
				Info.FileSizeBytes = FileSize;
				Info.LastModified = ModTime;

				OutLogFiles.Add(Info);
			}
		}
	}
}

void FConvaiLogCollector::GetFilesInDirectory(const FString &Directory, const FString &Pattern, TArray<FString> &OutFiles, int32 MaxFiles) const
{
	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*Directory))
	{
		return;
	}

	TArray<FString> AllFiles;
	PlatformFile.FindFiles(AllFiles, *Directory, *Pattern);

	for (int32 i = 0; i < AllFiles.Num(); ++i)
	{
		AllFiles[i] = FPaths::Combine(Directory, AllFiles[i]);
	}

	AllFiles.Sort([&PlatformFile](const FString &A, const FString &B)
				  { return PlatformFile.GetTimeStamp(*A) > PlatformFile.GetTimeStamp(*B); });
	int32 Count = FMath::Min(AllFiles.Num(), MaxFiles);
	for (int32 i = 0; i < Count; ++i)
	{
		OutFiles.Add(AllFiles[i]);
	}
}

bool FConvaiLogCollector::GetFileInfo(const FString &FilePath, int64 &OutSize, FDateTime &OutModificationTime) const
{
	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.FileExists(*FilePath))
	{
		return false;
	}

	OutSize = PlatformFile.FileSize(*FilePath);
	OutModificationTime = PlatformFile.GetTimeStamp(*FilePath);

	return true;
}

bool FConvaiLogCollector::IsRecentLogFile(const FDateTime &ModificationTime) const
{
	FTimespan MaxAge = FTimespan::FromHours(24);
	FDateTime Now = FDateTime::UtcNow();

	return (Now - ModificationTime) <= MaxAge;
}
