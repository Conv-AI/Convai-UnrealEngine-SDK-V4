// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiTestBase.h"



#include "Tests/ConvaiTestHarnessSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

bool UConvaiTestBase::Setup()
{
	if (Context.CharacterID.IsEmpty())
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError, TEXT("CharacterID is empty"));
		return false;
	}
	return true;
}

bool UConvaiTestBase::Verify()
{
	return true;
}

void UConvaiTestBase::Teardown()
{
	// Base has nothing to clean up; subclasses override.
}

void UConvaiTestBase::BeginRun(const FConvaiTestContext& InContext, const FString& RunFolder, FOnConvaiTestFinished OnFinished)
{
	Context = InContext;
	OnFinishedDelegate = OnFinished;

	Record = FConvaiTestRunRecord{};
	Record.TestName = GetTestName();
	Record.Status = EConvaiTestStatus::Running;
	Record.StartedAtUtc = FDateTime::UtcNow();

	StartTimeSeconds = FPlatformTime::Seconds();
	Logger.Open(RunFolder, GetTestName());

	LogEvent(ELogVerbosity::Log,
		FString::Printf(TEXT("==== BEGIN %s  CharacterID=%s  Timeout=%.1fs ===="),
			*GetTestName(), *Context.CharacterID, Context.TimeoutSeconds),
		TEXT("lifecycle"));

	if (!Setup())
	{
		// Setup may have already called Finalize via FinishWithFailure.
		if (!bFinalized)
		{
			Finalize(EConvaiTestStatus::Failed, EConvaiTestFailureReason::SetupError, TEXT("Setup returned false"));
		}
		return;
	}

	Execute();
}

void UConvaiTestBase::Tick(float DeltaSeconds)
{
	if (bFinalized || Record.Status != EConvaiTestStatus::Running)
	{
		return;
	}

	if (ElapsedMs() >= Context.TimeoutSeconds * 1000.0)
	{
		FinishWithFailure(EConvaiTestFailureReason::Timeout,
			FString::Printf(TEXT("Exceeded overall timeout of %.1fs"), Context.TimeoutSeconds));
	}
}

void UConvaiTestBase::Abort()
{
	if (!bFinalized)
	{
		Finalize(EConvaiTestStatus::Aborted, EConvaiTestFailureReason::UserAborted, TEXT("User aborted"));
	}
}

double UConvaiTestBase::ElapsedMs() const
{
	return (FPlatformTime::Seconds() - StartTimeSeconds) * 1000.0;
}

void UConvaiTestBase::LogEvent(ELogVerbosity::Type Verbosity, const FString& Message, const FString& Category)
{
	Logger.LogEvent(ElapsedMs(), Verbosity, Category, Message, Record);
}

void UConvaiTestBase::RecordPhase(const FString& PhaseName, double StartMs, double DurationMs)
{
	FConvaiTestPhaseTiming Timing;
	Timing.PhaseName = PhaseName;
	Timing.StartMs = StartMs;
	Timing.DurationMs = DurationMs;
	Record.PhaseTimings.Add(Timing);
}

void UConvaiTestBase::FinishWithPass(const FString& Reason)
{
	if (!bFinalized)
	{
		if (!Verify())
		{
			Finalize(EConvaiTestStatus::Failed, EConvaiTestFailureReason::UnexpectedState,
				TEXT("Verify() returned false"));
			return;
		}
		Finalize(EConvaiTestStatus::Passed, EConvaiTestFailureReason::None, Reason);
	}
}

void UConvaiTestBase::FinishWithFailure(EConvaiTestFailureReason Reason, const FString& Detail)
{
	if (!bFinalized)
	{
		Finalize(EConvaiTestStatus::Failed, Reason, Detail);
	}
}

UWorld* UConvaiTestBase::GetTestWorld() const
{
	if (const UConvaiTestHarnessSubsystem* Harness = Cast<UConvaiTestHarnessSubsystem>(GetOuter()))
	{
		if (const UGameInstance* GI = Harness->GetGameInstance())
		{
			return GI->GetWorld();
		}
	}
	return nullptr;
}

void UConvaiTestBase::Finalize(EConvaiTestStatus FinalStatus, EConvaiTestFailureReason Reason, const FString& Detail)
{
	if (bFinalized) return;
	bFinalized = true;

	Record.Status = FinalStatus;
	Record.FailureReason = Reason;
	Record.FailureDetail = Detail;
	Record.TotalDurationMs = ElapsedMs();

	const TCHAR* Tag = (FinalStatus == EConvaiTestStatus::Passed) ? TEXT("PASS") : TEXT("FAIL");
	LogEvent(FinalStatus == EConvaiTestStatus::Passed ? ELogVerbosity::Log : ELogVerbosity::Error,
		FString::Printf(TEXT("==== %s %s : %s ===="), Tag, *GetTestName(), *Detail),
		TEXT("lifecycle"));

	Teardown();

	Logger.WriteSummary(Record);
	Logger.Close();

	OnFinishedDelegate.ExecuteIfBound(Record);
}


