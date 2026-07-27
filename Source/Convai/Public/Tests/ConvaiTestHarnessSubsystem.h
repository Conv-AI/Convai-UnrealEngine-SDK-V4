// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Templates/SubclassOf.h"
#include "Tests/ConvaiTestMacros.h"
#include "Tests/ConvaiTestTypes.h"
#include "ConvaiTestHarnessSubsystem.generated.h"



class UConvaiTestBase;
struct IConsoleCommand;

/**
 * Game-instance subsystem that owns the Convai test harness.
 *
 * Responsibilities:
 *  - register known test classes at startup
 *  - run a single named test or a suite, one at a time
 *  - keep strong UPROPERTY refs to the currently-running test (GC protection)
 *  - tick the active test every frame
 *  - aggregate results + write a suite-level report
 *  - expose Convai.Test.* console commands as the primary driver
 *
 * To add a new test: create a UConvaiTestBase subclass, register its class in RegisterBuiltinTests().
 */
UCLASS()
class CONVAI_API UConvaiTestHarnessSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	// USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual UWorld* GetTickableGameObjectWorld() const override;

	/** Register a test class by short name. Call during Initialize or from game code. */
	void RegisterTestClass(const FString& ShortName, TSubclassOf<UConvaiTestBase> Class);

	/** Starts a single test by its registered name. Returns false if already running or name unknown. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Test")
	bool RunTestByName(const FString& TestName, const FConvaiTestContext& InContext);

	/** Runs all registered tests sequentially. Context is shared. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Test")
	bool RunAllTests(const FConvaiTestContext& InContext);

	/** Aborts whatever is currently running and clears any queued tests. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Test")
	void AbortAll();

	/** Writes a summary of the last suite run to the run folder + logs a one-line report. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Test")
	void PrintLastReport() const;

	/** Returns names of all registered tests (used by console help). */
	TArray<FString> GetRegisteredTestNames() const;

	const FString& GetCurrentRunFolder() const { return CurrentRunFolder; }

private:
	void RegisterBuiltinTests();
	void RegisterConsoleCommands();
	void UnregisterConsoleCommands();

	void StartNextQueuedTest();
	void OnActiveTestFinished(const FConvaiTestRunRecord& Record);

	/** Console command handlers. Each parses its args then forwards to RunTestByName / RunAllTests / AbortAll. */
	void Cmd_Run(const TArray<FString>& Args);
	void Cmd_RunAll(const TArray<FString>& Args);
	void Cmd_Abort(const TArray<FString>& Args);
	void Cmd_Report(const TArray<FString>& Args);
	void Cmd_List(const TArray<FString>& Args);

	/** Build a context from console args of the form "Key=Value Key=Value". Unknown keys are ignored. */
	static FConvaiTestContext ParseContextFromArgs(const TArray<FString>& Args);

	/** Name -> test class. */
	TMap<FString, TSubclassOf<UConvaiTestBase>> Registry;

	/** The test currently being run (null if idle). */
	UPROPERTY()
	UConvaiTestBase* ActiveTest = nullptr;

	/** Queue of test names still to run (populated by RunAllTests). */
	TArray<FString> PendingQueue;

	/** Context reused across a suite run. */
	UPROPERTY()
	FConvaiTestContext ActiveContext;

	/** Per-suite root folder under Saved/ConvaiTests/<timestamp>. */
	FString CurrentRunFolder;

	/** Aggregated records for the last (or current) suite run. */
	UPROPERTY()
	TArray<FConvaiTestRunRecord> LastSuiteResults;

	TArray<IConsoleCommand*> ConsoleCommands;
};


