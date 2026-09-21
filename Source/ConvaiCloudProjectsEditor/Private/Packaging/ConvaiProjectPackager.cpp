// Copyright Convai Inc. All Rights Reserved.
#include "Packaging/ConvaiProjectPackager.h"

#include "Packaging/ConvaiProjectArchive.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "IUATHelperModule.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "ConvaiCloudProjects"

namespace
{
/** Everything this tool writes lives here, so one cleanup covers a whole run. */
FString RunRoot()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Convai/CloudProjects"));
}
}

const TCHAR* FConvaiProjectPackager::GetTargetPlatform()
{
	return TEXT("Win64");
}

FString FConvaiProjectPackager::GetPlatformUnavailableReason()
{
	ITargetPlatformManagerModule* Manager = GetTargetPlatformManager();
	if (!Manager) return TEXT("The editor's target platform manager is unavailable. Restart the editor and try again.");

	// Unreal names this platform twice. UAT takes the UBT string ("Win64"), while the target
	// platform registry is keyed by the platform info name ("Windows") -- exactly the split
	// TurnkeySupport makes between UBTPlatformString and PlatformInfo->Name. Asking the registry
	// for "Win64" always misses, which reads as "SDK not installed" on a machine that has it.
	for (const ITargetPlatform* Platform : Manager->GetTargetPlatforms())
	{
		if (Platform && Platform->PlatformName().StartsWith(TEXT("Windows"))) return FString();
	}

	return FString::Printf(
		TEXT("This editor cannot package for %s. Check that the Windows platform SDK is installed for this engine, ")
		TEXT("restart the editor, and upload again."), GetTargetPlatform());
}

FConvaiProjectPackager::~FConvaiProjectPackager()
{
	Cancel();
}

void FConvaiProjectPackager::Cancel()
{
	if (CancelFlag.IsValid()) CancelFlag->Store(true);
}

void FConvaiProjectPackager::Start(FCompletion Completion, FProgress Progress)
{
	check(IsInGameThread());
	if (bBusy)
	{
		Completion(FString(), TEXT("A package is already running."));
		return;
	}

	const FString Unavailable = GetPlatformUnavailableReason();
	if (!Unavailable.IsEmpty())
	{
		Completion(FString(), Unavailable);
		return;
	}

	const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	if (ProjectPath.IsEmpty())
	{
		Completion(FString(), TEXT("This editor has no project file to package."));
		return;
	}

	OnComplete = MoveTemp(Completion);
	OnProgress = MoveTemp(Progress);
	CancelFlag = MakeShared<TAtomic<bool>>(false);
	bBusy = true;

	// A stale stage would silently ship the previous build, so the run starts from nothing.
	StageDirectory = RunRoot() / TEXT("Stage");
	ArchivePath = RunRoot() / FString::Printf(TEXT("%s.zip"), FApp::GetProjectName());
	IFileManager::Get().DeleteDirectory(*StageDirectory, false, true);
	IFileManager::Get().MakeDirectory(*StageDirectory, true);

	FString Params = FString::Printf(TEXT("-nop4 -utf8output -nocompileeditor -skipbuildeditor -project=\"%s\""), *ProjectPath);
	Params += FString::Printf(TEXT(" -platform=%s"), GetTargetPlatform());
	Params += TEXT(" -clientconfig=Development");
	Params += TEXT(" -build -cook -stage -pak -iostore -compressed -package -archive -prereqs");
	Params += FString::Printf(TEXT(" -archivedirectory=\"%s\""), *StageDirectory);
	if (FApp::IsEngineInstalled()) Params += TEXT(" -installed");

	const FString CommandLine = FString::Printf(TEXT(" -ScriptsForProject=\"%s\" BuildCookRun %s"), *ProjectPath, *Params);

	if (OnProgress) OnProgress(EConvaiPackageStage::Packaging, TOptional<float>());

	TWeakPtr<FConvaiProjectPackager> WeakSelf = AsShared();
	IUATHelperModule::Get().CreateUatTask(
		CommandLine,
		FText::FromString(GetTargetPlatform()),
		LOCTEXT("PackagingProject", "Packaging project for Convai"),
		LOCTEXT("PackagingProjectShort", "Packaging"),
		FAppStyle::Get().GetBrush(TEXT("MainFrame.PackageProject")),
		nullptr,
		[WeakSelf](FString Result, double)
		{
			// UAT reports from its own thread; everything after this point is game-thread work.
			AsyncTask(ENamedThreads::GameThread, [WeakSelf, Result]()
			{
				const TSharedPtr<FConvaiProjectPackager> Self = WeakSelf.Pin();
				if (!Self || !Self->bBusy) return;

				if (Result != TEXT("Completed"))
				{
					Self->Finish(FString(), Result == TEXT("Canceled")
						? TEXT("Packaging was cancelled.")
						: TEXT("Packaging failed. Open the packaging log for the reason, fix it, and upload again."));
					return;
				}
				Self->BeginArchive();
			});
		});
}

void FConvaiProjectPackager::BeginArchive()
{
	// UAT archives into a per-platform folder. Read it back rather than assuming the name,
	// so a flavoured platform (Windows vs WindowsArm64) still resolves.
	TArray<FString> Staged;
	IFileManager::Get().FindFiles(Staged, *(StageDirectory / TEXT("*")), false, true);
	if (Staged.Num() != 1)
	{
		Finish(FString(), FString::Printf(
			TEXT("The packaged build could not be located under %s. Open the packaging log for details."), *StageDirectory));
		return;
	}

	const FString BuildDirectory = StageDirectory / Staged[0];
	if (OnProgress) OnProgress(EConvaiPackageStage::Compressing, 0.f);

	TWeakPtr<FConvaiProjectPackager> WeakSelf = AsShared();
	const TSharedPtr<TAtomic<bool>> Flag = CancelFlag;
	const FString Output = ArchivePath;

	Async(EAsyncExecution::ThreadPool, [WeakSelf, Flag, BuildDirectory, Output]()
	{
		FString Error;
		const bool bZipped = FConvaiProjectArchive::ZipDirectory(BuildDirectory, Output, Flag,
			[WeakSelf](float Fraction)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakSelf, Fraction]()
				{
					const TSharedPtr<FConvaiProjectPackager> Self = WeakSelf.Pin();
					if (Self && Self->bBusy && Self->OnProgress) Self->OnProgress(EConvaiPackageStage::Compressing, Fraction);
				});
			}, Error);

		AsyncTask(ENamedThreads::GameThread, [WeakSelf, bZipped, Output, Error]()
		{
			const TSharedPtr<FConvaiProjectPackager> Self = WeakSelf.Pin();
			if (!Self || !Self->bBusy) return;
			Self->Finish(bZipped ? Output : FString(), bZipped ? FString() : Error);
		});
	});
}

void FConvaiProjectPackager::Finish(FString InArchivePath, FString Error)
{
	bBusy = false;
	CancelFlag.Reset();

	// The staged tree is only an input to the archive; keeping it doubles the build's disk cost.
	if (!InArchivePath.IsEmpty()) IFileManager::Get().DeleteDirectory(*StageDirectory, false, true);

	FCompletion Completion = MoveTemp(OnComplete);
	OnComplete = nullptr;
	OnProgress = nullptr;
	if (Completion) Completion(MoveTemp(InArchivePath), MoveTemp(Error));
}

#undef LOCTEXT_NAMESPACE
