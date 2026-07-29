// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Tests/ConvaiTestMacros.h"
#include "Tests/ConvaiTestTypes.h"
#include "Tests/ConvaiTestLogger.h"
#include "ConvaiTestBase.generated.h"



class UWorld;
class UConvaiTestHarnessSubsystem;

DECLARE_DELEGATE_OneParam(FOnConvaiTestFinished, const FConvaiTestRunRecord& /*Record*/);

/**
 * Abstract base for every concrete Convai integration test.
 *
 * Lifecycle: Setup() -> Execute() -> (async work, polled via Tick) -> Verify() -> Teardown().
 * The derived class drives state transitions and calls FinishWithPass / FinishWithFailure when done.
 *
 * The base handles:
 *  - timing / elapsed ms
 *  - overall timeout (kills the run if Execute-to-Verify takes too long)
 *  - logging -> FConvaiTestLogger -> per-run folder
 *  - per-phase scoped traces via CONVAI_TEST_TRACE
 *  - GC protection (held UPROPERTY on harness subsystem)
 */
UCLASS(Abstract, Within = ConvaiTestHarnessSubsystem)
class CONVAI_API UConvaiTestBase : public UObject
{
	GENERATED_BODY()

public:
	/** Human-readable test name (used for console lookup and folder name). Override in subclass. */
	virtual FString GetTestName() const PURE_VIRTUAL(UConvaiTestBase::GetTestName, return TEXT("Base"););

	/** Starts the test. Called by the harness after Context has been stamped. */
	void BeginRun(const FConvaiTestContext& InContext, const FString& RunFolder, FOnConvaiTestFinished OnFinished);

	/** Called by the subsystem on every tick while Running. Derived classes do NOT usually override — they drive via delegates/timers. */
	void Tick(float DeltaSeconds);

	/** Called by the harness when the user aborts the run. */
	void Abort();

	/** Live elapsed time in milliseconds since BeginRun. Used by scoped trace helpers. */
	double ElapsedMs() const;

	/** Append one entry to the trace log + run record. Public so helper macros / trace scopes can call it. */
	void LogEvent(ELogVerbosity::Type Verbosity, const FString& Message, const FString& Category = TEXT("general"));

	/** Record a finished phase timing. Called by FConvaiTestScopedTrace destructor. */
	void RecordPhase(const FString& PhaseName, double StartMs, double DurationMs);

	EConvaiTestStatus GetStatus() const { return Record.Status; }
	const FConvaiTestRunRecord& GetRecord() const { return Record; }

protected:
	/** Override: validate Context, cache world/subsystems. Return false to abort before Execute. */
	virtual bool Setup();

	/** Override: kick off the actual work (spawn actors, start sessions, send requests...). */
	virtual void Execute() PURE_VIRTUAL(UConvaiTestBase::Execute, );

	/** Override: optional final assertion once the async work settles. Return false to fail. */
	virtual bool Verify();

	/** Override: cleanup spawned actors, components, delegate bindings. Always called. */
	virtual void Teardown();

	/** Derived class calls this when it determines the test succeeded. */
	void FinishWithPass(const FString& Reason);

	/** Derived class calls this when it determines the test failed. */
	void FinishWithFailure(EConvaiTestFailureReason Reason, const FString& Detail);

	/** Helper: returns the world the test is running in (harness owner's world). */
	UWorld* GetTestWorld() const;

	/** Shared context (read-only during the run). */
	UPROPERTY()
	FConvaiTestContext Context;

	/** Full per-run record, built up as the test runs. */
	UPROPERTY()
	FConvaiTestRunRecord Record;

private:
	void Finalize(EConvaiTestStatus FinalStatus, EConvaiTestFailureReason Reason, const FString& Detail);

	double StartTimeSeconds = 0.0;
	bool bFinalized = false;

	FConvaiTestLogger Logger;
	FOnConvaiTestFinished OnFinishedDelegate;
};

/**
 * RAII helper — drop one on the stack at the start of a phase and its destructor records the timing.
 * Paired with the CONVAI_TEST_TRACE macro.
 */
struct CONVAI_API FConvaiTestScopedTrace
{
	FConvaiTestScopedTrace(UConvaiTestBase* InTest, const FString& InPhaseName)
		: Test(InTest), PhaseName(InPhaseName), StartMs(InTest ? InTest->ElapsedMs() : 0.0)
	{
		if (Test)
		{
			Test->LogEvent(ELogVerbosity::Log, FString::Printf(TEXT("-> enter %s"), *PhaseName), TEXT("phase"));
		}
	}

	~FConvaiTestScopedTrace()
	{
		if (Test)
		{
			const double EndMs = Test->ElapsedMs();
			Test->RecordPhase(PhaseName, StartMs, EndMs - StartMs);
			Test->LogEvent(ELogVerbosity::Log,
				FString::Printf(TEXT("<- leave %s (%.1fms)"), *PhaseName, EndMs - StartMs),
				TEXT("phase"));
		}
	}

	FConvaiTestScopedTrace(const FConvaiTestScopedTrace&) = delete;
	FConvaiTestScopedTrace& operator=(const FConvaiTestScopedTrace&) = delete;

private:
	UConvaiTestBase* Test;
	FString PhaseName;
	double StartMs;
};


