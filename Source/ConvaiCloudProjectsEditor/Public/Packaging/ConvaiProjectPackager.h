// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

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
	/** Stops the zip stage. A running UAT task is cancelled from its own notification. */
	void Cancel();
	bool IsBusy() const { return bBusy; }

private:
	void BeginArchive();
	void Finish(FString ArchivePath, FString Error);

	/** Per-run root under Saved, cleaned at the start of every run so stale stages never ship. */
	FString StageDirectory;
	FString ArchivePath;
	FCompletion OnComplete;
	FProgress OnProgress;
	bool bBusy = false;
	TSharedPtr<TAtomic<bool>> CancelFlag;
};
