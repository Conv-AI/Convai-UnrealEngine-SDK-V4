// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tests/ConvaiTestMacros.h"
#include "ConvaiTestTypes.generated.h"

class USoundWave;

UENUM(BlueprintType)
enum class EConvaiTestStatus : uint8
{
	NotStarted  UMETA(DisplayName = "Not Started"),
	Running     UMETA(DisplayName = "Running"),
	Passed      UMETA(DisplayName = "Passed"),
	Failed      UMETA(DisplayName = "Failed"),
	Aborted     UMETA(DisplayName = "Aborted"),
};

UENUM(BlueprintType)
enum class EConvaiTestFailureReason : uint8
{
	None                UMETA(DisplayName = "None"),
	Timeout             UMETA(DisplayName = "Timeout"),
	UnexpectedState     UMETA(DisplayName = "Unexpected State"),
	CallbackMissing     UMETA(DisplayName = "Callback Missing"),
	FailureEventRaised  UMETA(DisplayName = "Failure Event Raised"),
	BadPayload          UMETA(DisplayName = "Bad Payload"),
	SetupError          UMETA(DisplayName = "Setup Error"),
	UserAborted         UMETA(DisplayName = "User Aborted"),
};

/**
 * Plain POD context passed into every test. Populated by the harness before Setup() runs.
 * Defaults are chosen so a quick console invocation with only CharacterID works.
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiTestContext
{
	GENERATED_BODY()

	/** Character the test will talk to. Required. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Test")
	FString CharacterID;

	/** Optional secondary character (used by connection tests that need two IDs). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Test")
	FString SecondaryCharacterID;

	/** Text the Text / EndToEnd tests will send. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Test")
	FString SampleText = TEXT("Hello, can you hear me?");

	/** Pre-recorded audio asset used by the Audio / EndToEnd tests. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Test")
	TSoftObjectPtr<USoundWave> SampleAudio;

	/** Overall per-test timeout in seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Test")
	float TimeoutSeconds = 30.0f;

	/** Wall-clock tag used for the output folder name (UTC timestamp filled by harness). */
	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	FString RunTag;
};

/** One entry in a test's trace log. Every subscribed callback, state change, and phase boundary produces one. */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiTestEvent
{
	GENERATED_BODY()

	/** Monotonic milliseconds since test Setup() started. */
	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	double ElapsedMs = 0.0;

	/** "handshake", "text.response", "audio.send", etc. — used for filtering/reporting. */
	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	FString Category;

	/** Info/Warning/Error bucket (stored as uint8 of ELogVerbosity). */
	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	uint8 Severity = 0;

	/** Human-readable payload. */
	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	FString Message;
};

/** Stopwatch record written by FConvaiTestScopedTrace. */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiTestPhaseTiming
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	FString PhaseName;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	double StartMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	double DurationMs = 0.0;
};

/** Full record of one test run. Handed back to the harness for aggregation + report writing. */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiTestRunRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	FString TestName;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	EConvaiTestStatus Status = EConvaiTestStatus::NotStarted;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	EConvaiTestFailureReason FailureReason = EConvaiTestFailureReason::None;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	FString FailureDetail;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	FDateTime StartedAtUtc;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	double TotalDurationMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	TArray<FConvaiTestEvent> Events;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Test")
	TArray<FConvaiTestPhaseTiming> PhaseTimings;
};


