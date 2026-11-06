// Copyright Convai Inc. All Rights Reserved.
#include "Packaging/ConvaiProjectPackager.h"

#include "Packaging/ConvaiProjectArchive.h"

#include "Async/Async.h"
#include "EditorAnalytics.h"
#include "HAL/FileManager.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Logging/MessageLog.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/MonitoredProcess.h"
#include "Misc/Paths.h"

/** UAT's own output, line by line: the packaging log. */
DEFINE_LOG_CATEGORY_STATIC(LogConvaiCloudPackaging, Log, All);

namespace
{
/** Everything this tool writes lives here, so one cleanup covers a whole run. */
FString RunRoot()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Convai/CloudProjects"));
}

/** A run stopped by Cancel reports this whichever stage noticed, the same words the archive uses. */
const TCHAR* const CancelledError = TEXT("The upload was cancelled.");

/** A failed run lists this many of its error lines in Packaging Results, and quotes the first few in the panel. */
// ponytail: the first ten; the Output Log's LogConvaiCloudPackaging lines keep every one.
constexpr int32 ReportedErrors = 10;
constexpr int32 QuotedErrors = 3;

/**
 * The text after "Error: " in a UAT line, or empty. The same reading, in the same order, as the one the editor's own
 * Package Project uses to fill Packaging Results (UATHelperModule's ProcessAndHandleCookErrorOutput).
 */
FString UatErrorText(const FString& Line)
{
	FString Before, After;
	if (Line.Split(TEXT("CookResults:Warning: "), &Before, &After)) return FString();
	if (Line.Split(TEXT("CookResults:Error: "), &Before, &After)) return After.TrimStartAndEnd();
	if (Line.Split(TEXT("Warning: "), &Before, &After)) return FString();
	if (Line.Split(TEXT("Error: "), &Before, &After)) return After.TrimStartAndEnd();
	return FString();
}

/** Lists a failed run in Packaging Results, where Package Project's failures go, and says in the panel why it failed. */
FString ReportUatFailure(int32 ReturnCode, const TArray<FString>& Errors)
{
	// A process stopped other than by Cancel reports -1, which TranslateErrorCode reads as UAT Not Found. Other
	// negative codes are crashes (0xC0000005), which are known by their hex.
	const FString Code = ReturnCode < 0 ? FString::Printf(TEXT("0x%08X"), static_cast<uint32>(ReturnCode)) : FString::FromInt(ReturnCode);
	const FString Summary = ReturnCode == -1
		? FString(TEXT("Packaging failed: UAT stopped without reporting an exit code."))
		: FString::Printf(TEXT("Packaging failed: %s (UAT exit code %s)."), *FEditorAnalytics::TranslateErrorCode(ReturnCode), *Code);

	// The Output Log already has every one of these lines, as UAT printed them. No page of its own: Unreal's own
	// Package Project clears the current page and would write its results under this one's title.
	FMessageLog Log(TEXT("PackagingResults"));
	Log.SuppressLoggingToOutputLog(true);
	Log.Error(FText::FromString(Summary));
	for (const FString& Error : Errors) Log.Error(FText::FromString(Error));

	if (Errors.IsEmpty())
	{
		return Summary + TEXT(" Open the packaging log (the Output Log's LogConvaiCloudPackaging lines) for the reason, fix it, and upload again.");
	}
	FString Quoted;
	for (int32 Index = 0; Index < FMath::Min(Errors.Num(), QuotedErrors); ++Index)
	{
		Quoted += FString::Printf(TEXT("%s\"%s\""), Index > 0 ? TEXT(", ") : TEXT(""), *Errors[Index]);
	}
	return FString::Printf(TEXT("%s First errors: %s. The Message Log's Packaging Results page lists them; fix the first and upload again."),
		*Summary, *Quoted);
}

/**
 * UAT, killed with its whole tree when the editor goes down on an error: module shutdown never runs then, and the
 * cook would go on for most of an hour. FSerializedUATProcess cancels on the same delegate.
 */
class FUatProcess final : public FMonitoredProcess
{
public:
	FUatProcess(const FString& Url, const FString& Params) : FMonitoredProcess(Url, Params, true)
	{
		ShutdownAfterError = FCoreDelegates::OnShutdownAfterError.AddLambda([this]() { Cancel(true); });
	}
	virtual ~FUatProcess() override
	{
		FCoreDelegates::OnShutdownAfterError.Remove(ShutdownAfterError);
	}

private:
	FDelegateHandle ShutdownAfterError;
};
}

bool FConvaiProjectPackager::bUatSucceededOnce = false;

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
	// Whoever started the run is being torn down with us, so the cancel below completes to nobody.
	OnComplete = nullptr;
	OnProgress = nullptr;
	Cancel();
	// The zip thread posts to the game thread until it returns, and at editor exit this runs from module shutdown,
	// before the task graph those posts need is torn down. Bounded by the entry in flight; the zip never waits on us.
	ZipTask.Wait();
}

void FConvaiProjectPackager::Cancel()
{
	if (CancelFlag.IsValid()) CancelFlag->Store(true);
	if (!Uat.IsValid()) return;

	// Left alone, the cook runs on for most of an hour and Start refuses until it ends. Releasing the process waits
	// for its monitor thread, which kills the whole tree first, so the stage is no longer written when it is deleted.
	Uat->Cancel(true);
	Uat.Reset();
	IFileManager::Get().DeleteDirectory(*StageDirectory, false, true);
	Finish(FString(), CancelledError);
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

	FString CommandLine = FString::Printf(TEXT(" -ScriptsForProject=\"%s\""), *ProjectPath);
	// One UAT runs per engine, and a second fails at once. The editor starts its own (an SDK check at startup, a device
	// connecting), which FSerializedUATProcess queued this behind; the flag makes this one wait instead. A global
	// option of AutomationTool on every supported engine, 5.3 included.
	CommandLine += TEXT(" -WaitForUATMutex");
	CommandLine += FString::Printf(TEXT(" BuildCookRun %s"), *Params);
	// FSerializedUATProcess's own saving: scripts that have run once this session need no second compile.
	if (bUatSucceededOnce) CommandLine += TEXT(" -nocompile -nocompileuat");

	if (OnProgress) OnProgress(EConvaiPackageStage::Packaging, TOptional<float>());

	TWeakPtr<FConvaiProjectPackager> WeakSelf = AsShared();
	const TSharedPtr<TAtomic<bool>> Run = CancelFlag;
	// Filled and read back on UAT's own thread, which prints every line before it reports the exit, so no lock.
	const TSharedRef<TArray<FString>> FirstErrors = MakeShared<TArray<FString>>();
	Uat = LaunchUat(CommandLine, [FirstErrors](FString Line)
	{
		if (Line.IsEmpty()) return;
		UE_LOG(LogConvaiCloudPackaging, Log, TEXT("%s"), *Line);
		if (FirstErrors->Num() >= ReportedErrors) return;
		// Unique: the cook prints each of its errors again under its Warning/Error Summary banner.
		FString Error = UatErrorText(Line);
		if (!Error.IsEmpty()) FirstErrors->AddUnique(MoveTemp(Error));
	},
	[WeakSelf, Run, FirstErrors](int32 ReturnCode)
	{
		// UAT reports from its own thread; everything after this point is game-thread work.
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, Run, ReturnCode, Errors = *FirstErrors]()
		{
			if (const TSharedPtr<FConvaiProjectPackager> Self = WeakSelf.Pin()) Self->HandleUatExit(Run, ReturnCode, Errors);
		});
	});
	if (!Uat.IsValid())
	{
		const FString Script = FSerializedUATProcess::GetUATPath();
		Finish(FString(), FPaths::FileExists(Script)
			? FString::Printf(TEXT("Unreal's packaging script at %s could not be started. Restart the editor and upload again."), *Script)
			: FString::Printf(TEXT("Unreal's packaging script was not found at %s. Verify or reinstall this engine, then upload again."), *Script));
	}
}

TSharedPtr<FMonitoredProcess> FConvaiProjectPackager::LaunchUatProcess(const FString& CommandLine, TFunction<void(FString)> OnOutput,
	TFunction<void(int32)> OnExit)
{
	const FString Script = FSerializedUATProcess::GetUATPath();
	if (!FPaths::FileExists(Script)) return nullptr;

	// The shell line FSerializedUATProcess builds, but not that class: its queue keeps raw pointers to waiting
	// processes and deletes them itself at exit, so one the packager owns could never be released safely.
#if PLATFORM_WINDOWS
	const TSharedRef<FMonitoredProcess> Process = MakeShared<FUatProcess>(
		TEXT("cmd.exe"), FString::Printf(TEXT("/c \"\"%s\" %s\""), *Script, *CommandLine));
#else
	const TSharedRef<FMonitoredProcess> Process = MakeShared<FUatProcess>(
		TEXT("/usr/bin/env"), FString::Printf(TEXT(" -- \"%s\" %s"), *Script, *CommandLine));
#endif
	Process->OnOutput().BindLambda([OnOutput](FString Line) { OnOutput(MoveTemp(Line)); });
	Process->OnCompleted().BindLambda([OnExit](int32 ReturnCode) { OnExit(ReturnCode); });
	// Only Cancel stops it on purpose, and that run has completed by then; any other stop is a failure.
	Process->OnCanceled().BindLambda([OnExit]() { OnExit(-1); });
	if (!Process->Launch()) return nullptr;
	return Process;
}

void FConvaiProjectPackager::HandleUatExit(const TSharedPtr<TAtomic<bool>>& Run, int32 ReturnCode, const TArray<FString>& Errors)
{
	// As FSerializedUATProcess counts it: 10 is Error_SDKNotFound, which the compiled scripts reported themselves.
	if (ReturnCode == 0 || ReturnCode == 10) bUatSucceededOnce = true;
	// A run that has completed already, a cancelled one included, ignores its UAT's late report.
	if (!bBusy || Run != CancelFlag) return;

	Uat.Reset();
	if (ReturnCode != 0)
	{
		Finish(FString(), ReportUatFailure(ReturnCode, Errors));
		return;
	}
	BeginArchive();
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

	// A zip can run for minutes; its own thread keeps it off the shared pool the editor's other work queues on.
	ZipTask = Async(EAsyncExecution::Thread, [WeakSelf, Flag, BuildDirectory, Output]()
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
			if (const TSharedPtr<FConvaiProjectPackager> Self = WeakSelf.Pin()) Self->HandleArchiveResult(bZipped, Output, Error);
		});
	});
}

void FConvaiProjectPackager::HandleArchiveResult(bool bZipped, const FString& Output, const FString& Error)
{
	if (!bBusy) return;

	// The zip reads the flag only between entries, so a Cancel during the last one still ends in a whole archive.
	if (CancelFlag->Load())
	{
		IFileManager::Get().Delete(*Output, false, true, true);
		Finish(FString(), CancelledError);
		return;
	}
	Finish(bZipped ? Output : FString(), bZipped ? FString() : Error);
}

void FConvaiProjectPackager::Finish(FString InArchivePath, FString Error)
{
	bBusy = false;
	CancelFlag.Reset();
	Uat.Reset();

	// The staged tree is only an input to the archive; keeping it doubles the build's disk cost.
	if (!InArchivePath.IsEmpty()) IFileManager::Get().DeleteDirectory(*StageDirectory, false, true);

	FCompletion Completion = MoveTemp(OnComplete);
	OnComplete = nullptr;
	OnProgress = nullptr;
	if (Completion) Completion(MoveTemp(InArchivePath), MoveTemp(Error));
}
