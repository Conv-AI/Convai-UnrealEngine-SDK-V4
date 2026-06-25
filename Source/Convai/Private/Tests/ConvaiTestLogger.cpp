// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiTestLogger.h"



#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

DEFINE_LOG_CATEGORY(LogConvaiTest);

namespace
{
	FString VerbosityToString(ELogVerbosity::Type V)
	{
		switch (V)
		{
		case ELogVerbosity::Fatal:   return TEXT("Fatal");
		case ELogVerbosity::Error:   return TEXT("Error");
		case ELogVerbosity::Warning: return TEXT("Warning");
		case ELogVerbosity::Display: return TEXT("Display");
		case ELogVerbosity::Log:     return TEXT("Log");
		case ELogVerbosity::Verbose: return TEXT("Verbose");
		default:                     return TEXT("Log");
		}
	}

	void WriteLine(FArchive* Ar, const FString& Line)
	{
		if (!Ar) return;
		const FTCHARToUTF8 Utf8(*Line);
		Ar->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
		const char NL = '\n';
		Ar->Serialize(const_cast<char*>(&NL), 1);
	}
}

FConvaiTestLogger::FConvaiTestLogger() = default;
FConvaiTestLogger::~FConvaiTestLogger() { Close(); }

void FConvaiTestLogger::Open(const FString& RunFolder, const FString& TestName)
{
	TestFolder = FPaths::Combine(RunFolder, TestName);
	IFileManager::Get().MakeDirectory(*TestFolder, /*Tree*/ true);

	const FString TextPath = FPaths::Combine(TestFolder, TEXT("run.log"));
	const FString JsonlPath = FPaths::Combine(TestFolder, TEXT("run.jsonl"));

	TextFile.Reset(IFileManager::Get().CreateFileWriter(*TextPath, FILEWRITE_AllowRead));
	JsonlFile.Reset(IFileManager::Get().CreateFileWriter(*JsonlPath, FILEWRITE_AllowRead));

	UE_LOG(LogConvaiTest, Log, TEXT("[%s] log opened -> %s"), *TestName, *TestFolder);
}

void FConvaiTestLogger::Close()
{
	if (TextFile)  { TextFile->Flush();  TextFile.Reset();  }
	if (JsonlFile) { JsonlFile->Flush(); JsonlFile.Reset(); }
}

void FConvaiTestLogger::LogEvent(
	double ElapsedMs,
	ELogVerbosity::Type Verbosity,
	const FString& Category,
	const FString& Message,
	FConvaiTestRunRecord& OutRecord)
{
	FConvaiTestEvent Event;
	Event.ElapsedMs = ElapsedMs;
	Event.Category = Category;
	Event.Severity = static_cast<uint8>(Verbosity);
	Event.Message = Message;
	OutRecord.Events.Add(Event);

	const FString VerbosityStr = VerbosityToString(Verbosity);
	const FString TextLine = FString::Printf(
		TEXT("[%8.1f ms] %-7s %-18s %s"),
		ElapsedMs, *VerbosityStr, *Category, *Message);

	WriteLine(TextFile.Get(), TextLine);

	// Echo to UE_LOG at the requested verbosity.
	switch (Verbosity)
	{
	case ELogVerbosity::Error:
		UE_LOG(LogConvaiTest, Error, TEXT("%s"), *TextLine); break;
	case ELogVerbosity::Warning:
		UE_LOG(LogConvaiTest, Warning, TEXT("%s"), *TextLine); break;
	default:
		UE_LOG(LogConvaiTest, Log, TEXT("%s"), *TextLine); break;
	}

	FString JsonLine;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonLine);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("elapsed_ms"), ElapsedMs);
	Writer->WriteValue(TEXT("severity"), VerbosityStr);
	Writer->WriteValue(TEXT("category"), Category);
	Writer->WriteValue(TEXT("message"), Message);
	Writer->WriteObjectEnd();
	Writer->Close();
	WriteLine(JsonlFile.Get(), JsonLine);
}

void FConvaiTestLogger::WriteSummary(const FConvaiTestRunRecord& Record)
{
	FString Json;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("test_name"), Record.TestName);
	Writer->WriteValue(TEXT("status"), static_cast<int32>(Record.Status));
	Writer->WriteValue(TEXT("failure_reason"), static_cast<int32>(Record.FailureReason));
	Writer->WriteValue(TEXT("failure_detail"), Record.FailureDetail);
	Writer->WriteValue(TEXT("started_at_utc"), Record.StartedAtUtc.ToIso8601());
	Writer->WriteValue(TEXT("total_duration_ms"), Record.TotalDurationMs);
	Writer->WriteValue(TEXT("event_count"), Record.Events.Num());

	Writer->WriteArrayStart(TEXT("phase_timings"));
	for (const FConvaiTestPhaseTiming& P : Record.PhaseTimings)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("phase"), P.PhaseName);
		Writer->WriteValue(TEXT("start_ms"), P.StartMs);
		Writer->WriteValue(TEXT("duration_ms"), P.DurationMs);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	const FString SummaryPath = FPaths::Combine(TestFolder, TEXT("summary.json"));
	FFileHelper::SaveStringToFile(Json, *SummaryPath);
}


