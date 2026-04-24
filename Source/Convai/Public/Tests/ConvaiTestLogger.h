// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tests/ConvaiTestMacros.h"
#include "Tests/ConvaiTestTypes.h"



/**
 * Per-test log sink. Owns an FArchive for human-readable text and a second one for JSON-lines,
 * mirrors every entry into the run record, and echoes to UE_LOG so the console still sees them.
 *
 * One instance per test run — owned by UConvaiTestBase. The harness root folder is shared across
 * all tests within a run (Saved/ConvaiTests/<UtcTimestamp>/), each test gets its own subfolder.
 */
class CONVAI_API FConvaiTestLogger
{
public:
	FConvaiTestLogger();
	~FConvaiTestLogger();

	/** Opens the per-test artifact files. RunFolder is the shared parent; TestName becomes the subfolder. */
	void Open(const FString& RunFolder, const FString& TestName);

	/** Flushes and closes both archives. Called on Teardown. */
	void Close();

	/** Append one trace event. Echoes to UE_LOG(LogConvaiTest), run.log, run.jsonl, and OutRecord. */
	void LogEvent(
		double ElapsedMs,
		ELogVerbosity::Type Verbosity,
		const FString& Category,
		const FString& Message,
		FConvaiTestRunRecord& OutRecord);

	/** Writes a final summary.json alongside the text/jsonl artifacts. */
	void WriteSummary(const FConvaiTestRunRecord& Record);

	const FString& GetTestFolder() const { return TestFolder; }

private:
	FString TestFolder;
	TUniquePtr<FArchive> TextFile;
	TUniquePtr<FArchive> JsonlFile;
};


