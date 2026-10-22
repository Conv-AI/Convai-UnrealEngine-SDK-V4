// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"

/** The packager's own two phases. The caller decides how to present them. */
enum class EConvaiPackageStage : uint8
{
	Packaging,
	Compressing
};

/**
 * Packages the current project with UAT and zips the staged build into one uploadable archive.
 * Start on the game thread; the completion lands there too.
 */
class FConvaiProjectPackager : public TSharedFromThis<FConvaiProjectPackager>
{
public:
	/** Archive path on success, otherwise an error. Exactly one of the two is set. */
	using FCompletion = TFunction<void(FString /*ArchivePath*/, FString /*Error*/)>;
	/** Fraction is unset while UAT runs: cooking reports no percentage worth showing. */
	using FProgress = TFunction<void(EConvaiPackageStage, TOptional<float> /*Fraction*/)>;

	~FConvaiProjectPackager();

	/**
	 * Windows. The cloud hosts run Linux, but they run the Windows build on it, so the archive
	 * carries a Win64 package -- the same choice Cloud Avatars makes for its uploads. The Linux
	 * export named in the build job is the container the host wraps around it, not the game.
	 */
	static const TCHAR* GetTargetPlatform();
	/** Empty when the platform can be packaged; otherwise why it cannot (missing toolchain). */
	static FString GetPlatformUnavailableReason();

	void Start(FCompletion Completion, FProgress Progress = {});
	/** While UAT runs, kills its process tree and completes at once; the zip stops at its next entry. */
	void Cancel();
	bool IsBusy() const { return bBusy; }

private:
	/** Starts BuildCookRun; null when nothing was launched. OnExit receives UAT's exit code on the process's own thread. */
	using FLaunchUat = TFunction<TSharedPtr<class FMonitoredProcess>(const FString& /*CommandLine*/, TFunction<void(int32 /*ReturnCode*/)> /*OnExit*/)>;
	static TSharedPtr<class FMonitoredProcess> LaunchUatProcess(const FString& CommandLine, TFunction<void(int32)> OnExit);

	/** The continuations of UAT and of the zip, both on the game thread. Run names the run UAT was started for. */
	void HandleUatExit(const TSharedPtr<TAtomic<bool>>& Run, int32 ReturnCode);
	void BeginArchive();
	void HandleArchiveResult(bool bZipped, const FString& Output, const FString& Error);
	void Finish(FString ArchivePath, FString Error);

	/** Per-run root under Saved, cleaned at the start of every run so stale stages never ship. */
	FString StageDirectory;
	FString ArchivePath;
	FCompletion OnComplete;
	FProgress OnProgress;
	bool bBusy = false;
	TSharedPtr<TAtomic<bool>> CancelFlag;
	/** UAT while it runs. Releasing it kills the process tree and waits for the process's monitor thread. */
	TSharedPtr<class FMonitoredProcess> Uat;
	TFuture<void> ZipTask;
	FLaunchUat LaunchUat = &FConvaiProjectPackager::LaunchUatProcess;
	/** Set once a UAT run in this editor session has run its compiled scripts; later launches skip compiling them. */
	static bool bUatSucceededOnce;

#if WITH_DEV_AUTOMATION_TESTS
	friend class FConvaiProjectPackagerHarness;
#endif
};
