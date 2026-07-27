// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiTestHarnessSubsystem.h"
#include "Tests/ConvaiTestBase.h"
#include "Tests/ConvaiTest_Connection.h"
#include "Tests/ConvaiTest_Text.h"
#include "Tests/ConvaiTest_Audio.h"
#include "Tests/ConvaiTest_EndToEnd.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Stats/Stats.h"

void UConvaiTestHarnessSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	RegisterBuiltinTests();
	RegisterConsoleCommands();
	UE_LOG(LogConvaiTest, Log, TEXT("ConvaiTestHarness initialized with %d tests"), Registry.Num());
}

void UConvaiTestHarnessSubsystem::Deinitialize()
{
	AbortAll();
	UnregisterConsoleCommands();
	Super::Deinitialize();
}

void UConvaiTestHarnessSubsystem::Tick(float DeltaTime)
{
	if (ActiveTest)
	{
		ActiveTest->Tick(DeltaTime);
	}
}

TStatId UConvaiTestHarnessSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UConvaiTestHarnessSubsystem, STATGROUP_Tickables);
}

UWorld* UConvaiTestHarnessSubsystem::GetTickableGameObjectWorld() const
{
	if (const UGameInstance* GI = GetGameInstance())
	{
		return GI->GetWorld();
	}
	return nullptr;
}

void UConvaiTestHarnessSubsystem::RegisterBuiltinTests()
{
	RegisterTestClass(TEXT("Connection"), UConvaiTest_Connection::StaticClass());
	RegisterTestClass(TEXT("Text"),       UConvaiTest_Text::StaticClass());
	RegisterTestClass(TEXT("Audio"),      UConvaiTest_Audio::StaticClass());
	RegisterTestClass(TEXT("EndToEnd"),   UConvaiTest_EndToEnd::StaticClass());
}

void UConvaiTestHarnessSubsystem::RegisterTestClass(const FString& ShortName, TSubclassOf<UConvaiTestBase> Class)
{
	if (!Class) return;
	Registry.Add(ShortName, Class);
}

TArray<FString> UConvaiTestHarnessSubsystem::GetRegisteredTestNames() const
{
	TArray<FString> Names;
	Registry.GetKeys(Names);
	return Names;
}

bool UConvaiTestHarnessSubsystem::RunTestByName(const FString& TestName, const FConvaiTestContext& InContext)
{
	if (ActiveTest)
	{
		UE_LOG(LogConvaiTest, Warning, TEXT("A test is already running (%s). AbortAll first."), *ActiveTest->GetTestName());
		return false;
	}

	TSubclassOf<UConvaiTestBase>* Found = Registry.Find(TestName);
	if (!Found || !*Found)
	{
		UE_LOG(LogConvaiTest, Error, TEXT("Unknown test name '%s'. Registered: %s"),
			*TestName, *FString::Join(GetRegisteredTestNames(), TEXT(",")));
		return false;
	}

	ActiveContext = InContext;

	// Lazy-create the run folder if this is a direct single-test run.
	if (CurrentRunFolder.IsEmpty())
	{
		const FString Tag = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
		CurrentRunFolder = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ConvaiTests"), Tag);
		IFileManager::Get().MakeDirectory(*CurrentRunFolder, /*Tree*/ true);
		ActiveContext.RunTag = Tag;
		LastSuiteResults.Reset();
	}

	ActiveTest = NewObject<UConvaiTestBase>(this, *Found);
	FOnConvaiTestFinished Finished;
	Finished.BindUObject(this, &UConvaiTestHarnessSubsystem::OnActiveTestFinished);
	ActiveTest->BeginRun(ActiveContext, CurrentRunFolder, Finished);
	return true;
}

bool UConvaiTestHarnessSubsystem::RunAllTests(const FConvaiTestContext& InContext)
{
	if (ActiveTest || PendingQueue.Num() > 0)
	{
		UE_LOG(LogConvaiTest, Warning, TEXT("Suite already running. AbortAll first."));
		return false;
	}

	ActiveContext = InContext;
	PendingQueue = GetRegisteredTestNames();
	PendingQueue.Sort();

	const FString Tag = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	CurrentRunFolder = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ConvaiTests"), Tag + TEXT("_suite"));
	IFileManager::Get().MakeDirectory(*CurrentRunFolder, /*Tree*/ true);
	ActiveContext.RunTag = Tag;
	LastSuiteResults.Reset();

	UE_LOG(LogConvaiTest, Log, TEXT("Starting suite of %d tests -> %s"), PendingQueue.Num(), *CurrentRunFolder);
	StartNextQueuedTest();
	return true;
}

void UConvaiTestHarnessSubsystem::StartNextQueuedTest()
{
	if (PendingQueue.Num() == 0)
	{
		PrintLastReport();
		CurrentRunFolder.Reset();
		return;
	}

	const FString Next = PendingQueue[0];
	PendingQueue.RemoveAt(0);

	TSubclassOf<UConvaiTestBase>* Found = Registry.Find(Next);
	if (!Found || !*Found)
	{
		UE_LOG(LogConvaiTest, Warning, TEXT("Registry missing '%s', skipping"), *Next);
		StartNextQueuedTest();
		return;
	}

	ActiveTest = NewObject<UConvaiTestBase>(this, *Found);
	FOnConvaiTestFinished Finished;
	Finished.BindUObject(this, &UConvaiTestHarnessSubsystem::OnActiveTestFinished);
	ActiveTest->BeginRun(ActiveContext, CurrentRunFolder, Finished);
}

void UConvaiTestHarnessSubsystem::OnActiveTestFinished(const FConvaiTestRunRecord& Record)
{
	LastSuiteResults.Add(Record);
	ActiveTest = nullptr;

	if (PendingQueue.Num() > 0)
	{
		StartNextQueuedTest();
	}
	else
	{
		PrintLastReport();
		CurrentRunFolder.Reset();
	}
}

void UConvaiTestHarnessSubsystem::AbortAll()
{
	PendingQueue.Reset();
	if (ActiveTest)
	{
		ActiveTest->Abort();
		ActiveTest = nullptr;
	}
	CurrentRunFolder.Reset();
}

void UConvaiTestHarnessSubsystem::PrintLastReport() const
{
	int32 Passed = 0, Failed = 0, Aborted = 0;
	for (const FConvaiTestRunRecord& R : LastSuiteResults)
	{
		switch (R.Status)
		{
		case EConvaiTestStatus::Passed:  ++Passed;  break;
		case EConvaiTestStatus::Failed:  ++Failed;  break;
		case EConvaiTestStatus::Aborted: ++Aborted; break;
		default: break;
		}
	}

	UE_LOG(LogConvaiTest, Log, TEXT("==== SUITE REPORT: %d passed, %d failed, %d aborted ===="),
		Passed, Failed, Aborted);

	for (const FConvaiTestRunRecord& R : LastSuiteResults)
	{
		const TCHAR* Tag =
			R.Status == EConvaiTestStatus::Passed ? TEXT("PASS") :
			R.Status == EConvaiTestStatus::Failed ? TEXT("FAIL") : TEXT("ABRT");
		UE_LOG(LogConvaiTest, Log, TEXT("  %s  %-12s  %.1fms  %s"),
			Tag, *R.TestName, R.TotalDurationMs, *R.FailureDetail);
	}

	if (CurrentRunFolder.IsEmpty()) return;

	// Write a suite.json alongside the per-test folders.
	FString Json;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("passed"), Passed);
	Writer->WriteValue(TEXT("failed"), Failed);
	Writer->WriteValue(TEXT("aborted"), Aborted);
	Writer->WriteArrayStart(TEXT("tests"));
	for (const FConvaiTestRunRecord& R : LastSuiteResults)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("name"), R.TestName);
		Writer->WriteValue(TEXT("status"), static_cast<int32>(R.Status));
		Writer->WriteValue(TEXT("failure_reason"), static_cast<int32>(R.FailureReason));
		Writer->WriteValue(TEXT("failure_detail"), R.FailureDetail);
		Writer->WriteValue(TEXT("total_duration_ms"), R.TotalDurationMs);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	const FString SuitePath = FPaths::Combine(CurrentRunFolder, TEXT("suite.json"));
	FFileHelper::SaveStringToFile(Json, *SuitePath);
}

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------

void UConvaiTestHarnessSubsystem::RegisterConsoleCommands()
{
	IConsoleManager& CM = IConsoleManager::Get();

	ConsoleCommands.Add(CM.RegisterConsoleCommand(
		TEXT("Convai.Test.Run"),
		TEXT("Convai.Test.Run <TestName> CharacterID=<id> [SecondaryCharacterID=<id>] [Text=<str>] [Timeout=<sec>]"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UConvaiTestHarnessSubsystem::Cmd_Run),
		ECVF_Default));

	ConsoleCommands.Add(CM.RegisterConsoleCommand(
		TEXT("Convai.Test.RunAll"),
		TEXT("Convai.Test.RunAll CharacterID=<id> [same args as Run]"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UConvaiTestHarnessSubsystem::Cmd_RunAll),
		ECVF_Default));

	ConsoleCommands.Add(CM.RegisterConsoleCommand(
		TEXT("Convai.Test.Abort"),
		TEXT("Aborts the currently running Convai test / suite."),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UConvaiTestHarnessSubsystem::Cmd_Abort),
		ECVF_Default));

	ConsoleCommands.Add(CM.RegisterConsoleCommand(
		TEXT("Convai.Test.Report"),
		TEXT("Prints the last suite report to the log."),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UConvaiTestHarnessSubsystem::Cmd_Report),
		ECVF_Default));

	ConsoleCommands.Add(CM.RegisterConsoleCommand(
		TEXT("Convai.Test.List"),
		TEXT("Lists all registered Convai test names."),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UConvaiTestHarnessSubsystem::Cmd_List),
		ECVF_Default));
}

void UConvaiTestHarnessSubsystem::UnregisterConsoleCommands()
{
	IConsoleManager& CM = IConsoleManager::Get();
	for (IConsoleCommand* Cmd : ConsoleCommands)
	{
		if (Cmd)
		{
			CM.UnregisterConsoleObject(Cmd);
		}
	}
	ConsoleCommands.Reset();
}

FConvaiTestContext UConvaiTestHarnessSubsystem::ParseContextFromArgs(const TArray<FString>& Args)
{
	FConvaiTestContext Ctx;
	for (const FString& Arg : Args)
	{
		FString Key, Value;
		if (!Arg.Split(TEXT("="), &Key, &Value)) continue;

		if (Key.Equals(TEXT("CharacterID"), ESearchCase::IgnoreCase))
		{
			Ctx.CharacterID = Value;
		}
		else if (Key.Equals(TEXT("SecondaryCharacterID"), ESearchCase::IgnoreCase))
		{
			Ctx.SecondaryCharacterID = Value;
		}
		else if (Key.Equals(TEXT("Text"), ESearchCase::IgnoreCase))
		{
			Ctx.SampleText = Value.Replace(TEXT("_"), TEXT(" "));
		}
		else if (Key.Equals(TEXT("Timeout"), ESearchCase::IgnoreCase))
		{
			Ctx.TimeoutSeconds = FCString::Atof(*Value);
		}
		else if (Key.Equals(TEXT("Audio"), ESearchCase::IgnoreCase))
		{
			Ctx.SampleAudio = TSoftObjectPtr<USoundWave>(FSoftObjectPath(Value));
		}
	}
	return Ctx;
}

void UConvaiTestHarnessSubsystem::Cmd_Run(const TArray<FString>& Args)
{
	if (Args.Num() == 0)
	{
		UE_LOG(LogConvaiTest, Warning, TEXT("Usage: Convai.Test.Run <TestName> CharacterID=<id> [...]"));
		return;
	}
	const FString TestName = Args[0];
	TArray<FString> RestArgs = Args;
	RestArgs.RemoveAt(0);
	const FConvaiTestContext Ctx = ParseContextFromArgs(RestArgs);
	RunTestByName(TestName, Ctx);
}

void UConvaiTestHarnessSubsystem::Cmd_RunAll(const TArray<FString>& Args)
{
	const FConvaiTestContext Ctx = ParseContextFromArgs(Args);
	RunAllTests(Ctx);
}

void UConvaiTestHarnessSubsystem::Cmd_Abort(const TArray<FString>&)
{
	AbortAll();
}

void UConvaiTestHarnessSubsystem::Cmd_Report(const TArray<FString>&)
{
	PrintLastReport();
}

void UConvaiTestHarnessSubsystem::Cmd_List(const TArray<FString>&)
{
	UE_LOG(LogConvaiTest, Log, TEXT("Registered Convai tests:"));
	for (const FString& Name : GetRegisteredTestNames())
	{
		UE_LOG(LogConvaiTest, Log, TEXT("  - %s"), *Name);
	}
}


