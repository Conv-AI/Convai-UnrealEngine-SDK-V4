/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiLogExporter.cpp
 *
 * Implementation of log export orchestration service.
 */

#include "Services/LogExport/ConvaiLogExporter.h"
#include "Services/LogExport/ConvaiSystemInfoCollector.h"
#include "Services/LogExport/ConvaiProjectInfoCollector.h"
#include "Services/LogExport/ConvaiLogCollector.h"
#include "Services/LogExport/ConvaiNetworkDiagnostics.h"
#include "Services/LogExport/ConvaiPerformanceCollector.h"
#include "Services/LogExport/ConvaiLogExportDialog.h"
#include "ConvaiEditor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "Serialization/JsonSerializer.h"

TSharedPtr<FConvaiLogExporter> FConvaiLogExporter::Instance = nullptr;

FConvaiLogExporter::FConvaiLogExporter()
{
	Collectors.Add(MakeShared<FConvaiSystemInfoCollector>());
	Collectors.Add(MakeShared<FConvaiProjectInfoCollector>());
	Collectors.Add(MakeShared<FConvaiLogCollector>());
	Collectors.Add(MakeShared<FConvaiNetworkDiagnostics>());
	Collectors.Add(MakeShared<FConvaiPerformanceCollector>());

	Packager = MakeUnique<FConvaiLogPackager>();
}

FConvaiLogExporter &FConvaiLogExporter::Get()
{
	if (!Instance.IsValid())
	{
		Instance = MakeShared<FConvaiLogExporter>();
	}
	return *Instance;
}

FConvaiPackageResult FConvaiLogExporter::ExportLogs(
	const FConvaiLogExportOptions &Options,
	FOnExportProgress ProgressCallback)
{
	const int32 TotalSteps = 4;
	int32 CurrentStep = 0;

	if (ProgressCallback.IsBound())
	{
		ProgressCallback.Execute(++CurrentStep, TotalSteps, TEXT("Collecting system and project information..."));
	}
	TSharedPtr<FJsonObject> Metadata = CollectAllMetadata(ProgressCallback, CurrentStep, TotalSteps);

	if (ProgressCallback.IsBound())
	{
		ProgressCallback.Execute(++CurrentStep, TotalSteps, TEXT("Gathering log files..."));
	}
	TArray<FConvaiLogFileInfo> LogFiles = CollectAllLogFiles(Options, ProgressCallback, CurrentStep, TotalSteps);

	if (LogFiles.Num() == 0)
	{
		return FConvaiPackageResult::Failure(TEXT("No log files found to export"));
	}

	if (ProgressCallback.IsBound())
	{
		ProgressCallback.Execute(++CurrentStep, TotalSteps, TEXT("Creating export package..."));
	}
	FConvaiPackageResult Result = CreatePackage(LogFiles, Metadata, Options, ProgressCallback, CurrentStep, TotalSteps);

	if (Result.bSuccess)
	{
		if (ProgressCallback.IsBound())
		{
			ProgressCallback.Execute(++CurrentStep, TotalSteps, TEXT("Export complete!"));
		}

		if (Options.bOpenLocationAfterExport)
		{
			OpenExportLocation(Result.PackagePath);
		}
	}
	else
	{
		UE_LOG(LogConvaiEditor, Error, TEXT("ConvaiLogExporter: log export failed - %s"), *Result.ErrorMessage);
	}

	return Result;
}

void FConvaiLogExporter::ExportLogsAsync(
	const FConvaiLogExportOptions &Options,
	TFunction<void(const FConvaiPackageResult &)> CompletionCallback)
{
	Async(EAsyncExecution::ThreadPool, [this, Options, CompletionCallback]()
		  {
		FConvaiPackageResult Result = ExportLogs(Options);

		if (CompletionCallback)
		{
			AsyncTask(ENamedThreads::GameThread, [Result, CompletionCallback]()
			{
				CompletionCallback(Result);
			});
		} });
}

TSharedPtr<FJsonObject> FConvaiLogExporter::CollectAllMetadata(
	FOnExportProgress &ProgressCallback,
	int32 &CurrentStep,
	int32 TotalSteps) const
{
	TSharedPtr<FJsonObject> RootMetadata = MakeShared<FJsonObject>();

	RootMetadata->SetStringField(TEXT("ExportVersion"), TEXT("1.0"));
	RootMetadata->SetStringField(TEXT("ExportedAt"), FDateTime::UtcNow().ToIso8601());
	RootMetadata->SetStringField(TEXT("ExportedBy"), TEXT("Convai Log Exporter"));

	for (const TSharedPtr<IConvaiInfoCollector> &Collector : Collectors)
	{
		if (Collector.IsValid() && Collector->IsAvailable())
		{
			FString CollectorName = Collector->GetCollectorName();

			TSharedPtr<FJsonObject> CollectedInfo = Collector->CollectInfo();
			if (CollectedInfo.IsValid())
			{
				RootMetadata->SetObjectField(CollectorName, CollectedInfo);
			}
		}
	}

	return RootMetadata;
}

TArray<FConvaiLogFileInfo> FConvaiLogExporter::CollectAllLogFiles(
	const FConvaiLogExportOptions &Options,
	FOnExportProgress &ProgressCallback,
	int32 &CurrentStep,
	int32 TotalSteps) const
{
	TArray<FConvaiLogFileInfo> AllLogFiles;

	for (const TSharedPtr<IConvaiInfoCollector> &Collector : Collectors)
	{
		if (Collector.IsValid() && Collector->GetCollectorName() == TEXT("LogFiles"))
		{
			FConvaiLogCollector *LogCollector = static_cast<FConvaiLogCollector *>(Collector.Get());
			if (LogCollector)
			{
				AllLogFiles = LogCollector->GetLogFiles();

				if (!Options.bIncludeCrashLogs)
				{
					AllLogFiles = AllLogFiles.FilterByPredicate([](const FConvaiLogFileInfo &Info)
																{ return Info.Category != TEXT("CrashLogs"); });
				}

				if (!Options.bIncludeEngineLogs)
				{
					AllLogFiles = AllLogFiles.FilterByPredicate([](const FConvaiLogFileInfo &Info)
																{ return Info.Category != TEXT("EngineLogs"); });
				}

				if (Options.MaxLogAgeHours > 0)
				{
					FDateTime CutoffDate = FDateTime::UtcNow() - FTimespan::FromHours(Options.MaxLogAgeHours);
					AllLogFiles = AllLogFiles.FilterByPredicate([&CutoffDate](const FConvaiLogFileInfo &Info)
																{ return Info.LastModified >= CutoffDate; });
				}

				break;
			}
		}
	}

	return AllLogFiles;
}

FConvaiPackageResult FConvaiLogExporter::CreatePackage(
	const TArray<FConvaiLogFileInfo> &LogFiles,
	const TSharedPtr<FJsonObject> &Metadata,
	const FConvaiLogExportOptions &Options,
	FOnExportProgress &ProgressCallback,
	int32 &CurrentStep,
	int32 TotalSteps) const
{
	if (!Packager.IsValid())
	{
		return FConvaiPackageResult::Failure(TEXT("Packager not initialized"));
	}

	return Packager->CreatePackage(LogFiles, Metadata, Options.bCreateZipArchive);
}

void FConvaiLogExporter::OpenExportLocation(const FString &Path) const
{
#if PLATFORM_WINDOWS
	if (FPaths::GetExtension(Path) == TEXT("zip"))
	{
		FString AbsolutePath = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePlatformFilename(AbsolutePath);

		FString ExplorerArgs = FString::Printf(TEXT("/select,\"%s\""), *AbsolutePath);
		FPlatformProcess::CreateProc(TEXT("explorer.exe"), *ExplorerArgs, true, false, false, nullptr, 0, nullptr, nullptr);
	}
	else
	{
		FPlatformProcess::ExploreFolder(*Path);
	}
#elif PLATFORM_MAC
	if (FPaths::GetExtension(Path) == TEXT("zip"))
	{
		FString AbsolutePath = FPaths::ConvertRelativePathToFull(Path);
		FPlatformProcess::CreateProc(TEXT("/usr/bin/open"), *FString::Printf(TEXT("-R \"%s\""), *AbsolutePath), true, false, false, nullptr, 0, nullptr, nullptr);
	}
	else
	{
		FPlatformProcess::LaunchURL(*FString::Printf(TEXT("file://%s"), *Path), nullptr, nullptr);
	}
#elif PLATFORM_LINUX
	FString FolderPath = FPaths::GetExtension(Path) == TEXT("zip") ? FPaths::GetPath(Path) : Path;
	FPlatformProcess::LaunchURL(*FString::Printf(TEXT("file://%s"), *FolderPath), nullptr, nullptr);
#endif
}
