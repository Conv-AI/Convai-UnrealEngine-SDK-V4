// Copyright 2022 Convai Inc. All Rights Reserved.

// Issue 02 — the whole pipe, end to end, carrying one scenario.
//
// Ported from convai_harness's main.cpp: filter -> run -> record -> report, with
// a watchdog at twice the declared deadline. Two deliberate differences.
//
// Scenarios are polled from a ticker rather than run on their own thread. The
// engine renders audio on the game thread's schedule, so a scenario that blocked
// would stop the very thing every Reference Audio measurement depends on.
//
// The process sets an exit code. The dead in-plugin harness did not, which made
// it unusable from a script — see issue 12.

#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestScenario.h"
#include "ConvaiTests.h"

#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace ConvaiTestRegistry
{
    namespace
    {
        // Function-local static: scenarios register from static initialisers, so
        // a namespace-scope container would be a static-init-order gamble.
        TArray<TPair<FString, FScenarioFactory>>& Storage()
        {
            static TArray<TPair<FString, FScenarioFactory>> Scenarios;
            return Scenarios;
        }
    }

    void Register(const TCHAR* Name, FScenarioFactory Factory)
    {
        Storage().Emplace(FString(Name), MoveTemp(Factory));
    }

    const TArray<TPair<FString, FScenarioFactory>>& All()
    {
        return Storage();
    }
}

namespace
{
    struct FScenarioRecord
    {
        FString Name;
        FConvaiScenarioResult Result;
        bool bTimedOut = false;
    };

    FString RunFolder()
    {
        static const FString Folder = FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("ConvaiTests"),
            FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
        return Folder;
    }

    void WriteReport(const FString& Path, const TArray<FScenarioRecord>& Records)
    {
        int32 Passed = 0;
        int32 Failed = 0;
        int32 SetupFailed = 0;
        for (const FScenarioRecord& Record : Records)
        {
            if (Record.Result.bPassed)
            {
                ++Passed;
            }
            else if (Record.Result.bSetupFailed)
            {
                ++SetupFailed;
            }
            else
            {
                ++Failed;
            }
        }

        FString Json;
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);

        Writer->WriteObjectStart();

        // Schema version so the Python side can refuse a report it does not
        // understand rather than silently misreading one.
        Writer->WriteValue(TEXT("schema"), TEXT("convai-tests/1"));

        Writer->WriteObjectStart(TEXT("summary"));
        Writer->WriteValue(TEXT("passed"), Passed);
        Writer->WriteValue(TEXT("failed"), Failed);
        Writer->WriteValue(TEXT("setup_failed"), SetupFailed);
        Writer->WriteObjectEnd();

        Writer->WriteArrayStart(TEXT("scenarios"));
        for (const FScenarioRecord& Record : Records)
        {
            Writer->WriteObjectStart();
            Writer->WriteValue(TEXT("name"), Record.Name);
            // A timeout outranks bSetupFailed: the watchdog fired, which is
            // the scenario failing to finish rather than failing to begin.
            Writer->WriteValue(TEXT("status"),
                               Record.bTimedOut  ? TEXT("timeout")
                               : Record.Result.bPassed ? TEXT("pass")
                               : Record.Result.bSetupFailed ? TEXT("setup-failed")
                                                            : TEXT("fail"));
            Writer->WriteValue(TEXT("duration_ms"), Record.Result.ElapsedSeconds * 1000.0);
            if (!Record.Result.FailReason.IsEmpty())
            {
                Writer->WriteValue(TEXT("fail_reason"), Record.Result.FailReason);
            }

            Writer->WriteObjectStart(TEXT("metrics"));
            for (const TPair<FString, double>& Metric : Record.Result.Metrics)
            {
                Writer->WriteValue(Metric.Key, Metric.Value);
            }
            Writer->WriteObjectEnd();

            if (Record.Result.MetricNotes.Num() > 0)
            {
                Writer->WriteObjectStart(TEXT("metric_notes"));
                for (const TPair<FString, FString>& Note : Record.Result.MetricNotes)
                {
                    Writer->WriteValue(Note.Key, Note.Value);
                }
                Writer->WriteObjectEnd();
            }

            Writer->WriteArrayStart(TEXT("findings"));
            for (const FConvaiScenarioFinding& Finding : Record.Result.Findings)
            {
                Writer->WriteObjectStart();
                Writer->WriteValue(TEXT("dedup_key"), Finding.DedupKey);
                Writer->WriteValue(TEXT("summary"), Finding.Summary);
                Writer->WriteValue(TEXT("evidence"), Finding.Evidence);
                Writer->WriteValue(TEXT("trace"),
                                   FPaths::Combine(RunFolder(), Record.Name + TEXT(".jsonl")));
                Writer->WriteObjectStart(TEXT("metrics"));
                for (const TPair<FString, double>& Metric : Finding.Metrics)
                {
                    Writer->WriteValue(Metric.Key, Metric.Value);
                }
                Writer->WriteObjectEnd();
                Writer->WriteObjectEnd();
            }
            Writer->WriteArrayEnd();

            // Never merged into findings. The agent is told these are leads.
            Writer->WriteArrayStart(TEXT("hypotheses"));
            for (const FString& Hypothesis : Record.Result.Hypotheses)
            {
                Writer->WriteValue(Hypothesis);
            }
            Writer->WriteArrayEnd();

            Writer->WriteObjectEnd();
        }
        Writer->WriteArrayEnd();
        Writer->WriteObjectEnd();
        Writer->Close();

        // Forced, not AutoDetect: AutoDetect writes UTF-16LE with a BOM as soon
        // as any string in the report is non-ASCII, and one em dash in a fail
        // reason is enough. run.py then dies on the whole batch rather than on
        // one report.
        FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS report=%s"), *Path);
    }

    // Drives the registry one scenario at a time off the core ticker.
    class FScenarioRunner : public TSharedFromThis<FScenarioRunner>
    {
    public:
        FScenarioRunner(UWorld* InWorld, const FString& InFilter, const FString& InExactName,
                        const FString& InReportPath, bool bInQuitWhenDone)
            : World(InWorld)
            , Filter(InFilter)
            , ExactName(InExactName)
            , ReportPath(InReportPath)
            , bQuitWhenDone(bInQuitWhenDone)
        {
        }

        void Start()
        {
            for (const TPair<FString, ConvaiTestRegistry::FScenarioFactory>& Entry :
                 ConvaiTestRegistry::All())
            {
                // Exact match exists because the filter is a substring test and
                // some scenario names are prefixes of others --
                // reference_feed_capture would drag in
                // reference_feed_capture_under_load, which defeats running one
                // scenario per process.
                const bool bSelected = ExactName.IsEmpty()
                                           ? (Filter.IsEmpty() || Entry.Key.Contains(Filter))
                                           : Entry.Key.Equals(ExactName, ESearchCase::IgnoreCase);
                if (bSelected)
                {
                    Pending.Add(Entry);
                }
            }

            UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS selected=%d"), Pending.Num());

            TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateSP(this, &FScenarioRunner::Tick));
        }

    private:
        bool Tick(float DeltaSeconds)
        {
            if (!Current.IsValid() && !BeginNext())
            {
                Complete();
                return false;
            }

            const double Elapsed = FPlatformTime::Seconds() - StartSeconds;

            // Hard backstop, matching the DLL harness: dump the recorder and
            // give up rather than let a hung scenario hold the process open. A
            // timeout is reported as a distinct status, not as a plain failure.
            if (Elapsed > Current->DeadlineSeconds() * 2.0)
            {
                Recorder.Record(TEXT("watchdog"), TEXT("deadline exceeded 2x"));
                DumpTrace();

                FScenarioRecord Record;
                Record.Name = Current->Name();
                Record.bTimedOut = true;
                Record.Result.bPassed = false;
                Record.Result.FailReason = FString::Printf(
                    TEXT("watchdog: exceeded 2x deadline (%.1fs)"), Current->DeadlineSeconds());
                Record.Result.ElapsedSeconds = Elapsed;
                Records.Add(MoveTemp(Record));

                UE_LOG(LogConvaiTests, Error, TEXT("CONVAI_TESTS [TIMEOUT] %s"), Current->Name());
                Current.Reset();
                return true;
            }

            if (!Current->Poll(DeltaSeconds))
            {
                return true;
            }

            FScenarioRecord Record;
            Record.Name = Current->Name();
            Record.Result = Current->Finish();
            Record.Result.ElapsedSeconds = Elapsed;

            if (!Record.Result.bPassed || Record.Result.Findings.Num() > 0)
            {
                DumpTrace();
            }

            UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS [%s] %s (%.0f ms)"),
                   Record.Result.bPassed        ? TEXT("PASS")
                   : Record.Result.bSetupFailed ? TEXT("SETUP-FAILED")
                                                : TEXT("FAIL"),
                   Current->Name(), Elapsed * 1000.0);
            if (!Record.Result.bPassed)
            {
                // Warning, not Error, for a scenario that never started: an
                // Error here is what makes a missing character read as a
                // product defect in every log that greps for one.
                if (Record.Result.bSetupFailed)
                {
                    UE_LOG(LogConvaiTests, Warning, TEXT("CONVAI_TESTS could-not-start=%s"),
                           *Record.Result.FailReason);
                }
                else
                {
                    UE_LOG(LogConvaiTests, Error, TEXT("CONVAI_TESTS reason=%s"),
                           *Record.Result.FailReason);
                }
            }

            Records.Add(MoveTemp(Record));
            Current.Reset();
            return true;
        }

        bool BeginNext()
        {
            if (Pending.Num() == 0)
            {
                return false;
            }

            const TPair<FString, ConvaiTestRegistry::FScenarioFactory> Entry = Pending[0];
            Pending.RemoveAt(0);

            Recorder.Drain();
            Current = Entry.Value();
            StartSeconds = FPlatformTime::Seconds();

            UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS [RUN] %s"), Current->Name());
            Current->Start(World, Recorder);
            return true;
        }

        void DumpTrace() const
        {
            if (Current.IsValid())
            {
                Recorder.DumpToFile(
                    FPaths::Combine(RunFolder(), FString(Current->Name()) + TEXT(".jsonl")));
            }
        }

        void Complete()
        {
            WriteReport(ReportPath, Records);

            int32 Failed = 0;
            int32 SetupFailed = 0;
            for (const FScenarioRecord& Record : Records)
            {
                if (Record.Result.bPassed)
                {
                    continue;
                }
                Record.Result.bSetupFailed ? ++SetupFailed : ++Failed;
            }
            UE_LOG(LogConvaiTests, Display,
                   TEXT("CONVAI_TESTS summary passed=%d failed=%d setup_failed=%d"),
                   Records.Num() - Failed - SetupFailed, Failed, SetupFailed);

            // A filter that matched nothing ran no assertion, so it cannot be a
            // pass. Reported here rather than at Start() because this is where
            // the outcome is decided, and a launch that greps only for the
            // summary line would otherwise read zero scenarios as success.
            if (Records.Num() == 0)
            {
                UE_LOG(LogConvaiTests, Warning, TEXT("CONVAI_TESTS selected no scenarios"));
            }

            if (bQuitWhenDone)
            {
                // The orchestrator branches on this. RequestExit(false) lets the
                // engine shut down cleanly so the report is flushed first. A
                // scenario that could not start still exits non-zero: it is not
                // a product failure, but it is not a pass either.
                const int32 Code =
                    (Records.Num() > 0 && Failed + SetupFailed == 0) ? 0 : 1;
                ConvaiTestExit::Arm(Code);
                FPlatformMisc::RequestExitWithStatus(/*Force=*/false, Code);
            }
        }

        UWorld* World = nullptr;
        FString Filter;
        FString ExactName;
        FString ReportPath;
        bool bQuitWhenDone = false;

        TArray<TPair<FString, ConvaiTestRegistry::FScenarioFactory>> Pending;
        TSharedPtr<FConvaiTestScenario> Current;
        double StartSeconds = 0.0;

        FConvaiTestEventRecorder Recorder;
        TArray<FScenarioRecord> Records;
        FTSTicker::FDelegateHandle TickerHandle;
    };

    // Held for the lifetime of the run; the ticker delegate keeps a weak
    // reference through TSharedFromThis, so something has to own it.
    TSharedPtr<FScenarioRunner> GActiveRunner;

    void RunScenarios(const TArray<FString>& Args, UWorld* World, FOutputDevice&)
    {
        FString Filter;
        FString ExactName;
        FString ReportPath = FPaths::Combine(RunFolder(), TEXT("report.json"));
        bool bQuitWhenDone = false;

        // Commas as well as spaces. -ExecCmds values containing spaces do not
        // survive Windows command-line quoting when the process is launched
        // from Python's subprocess, so the orchestrator passes one
        // space-free token and this splits it.
        TArray<FString> Tokens;
        for (const FString& Arg : Args)
        {
            // ParseIntoArray resets its output, so it needs a scratch array —
            // parsing straight into Tokens would leave only the last argument.
            TArray<FString> Split;
            Arg.ParseIntoArray(Split, TEXT(","), /*InCullEmpty=*/true);
            Tokens.Append(MoveTemp(Split));
        }

        for (const FString& Token : Tokens)
        {
            if (Token.StartsWith(TEXT("-filter=")))
            {
                Filter = Token.RightChop(8);
            }
            else if (Token.StartsWith(TEXT("-scenario=")))
            {
                ExactName = Token.RightChop(10);
            }
            else if (Token.StartsWith(TEXT("-report=")))
            {
                ReportPath = Token.RightChop(8);
            }
            else if (Token.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
            {
                bQuitWhenDone = true;
            }
        }

        if (GActiveRunner.IsValid())
        {
            UE_LOG(LogConvaiTests, Error, TEXT("CONVAI_TESTS a run is already in progress"));
            return;
        }

        GActiveRunner =
            MakeShared<FScenarioRunner>(World, Filter, ExactName, ReportPath, bQuitWhenDone);
        GActiveRunner->Start();
    }

    void ListScenarios(const TArray<FString>&, UWorld*, FOutputDevice&)
    {
        for (const TPair<FString, ConvaiTestRegistry::FScenarioFactory>& Entry :
             ConvaiTestRegistry::All())
        {
            // Constructed only to ask what it needs; nothing is started here.
            const TSharedRef<FConvaiTestScenario> Scenario = Entry.Value();
            UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS scenario=%s needs_character=%d"),
                   *Entry.Key, Scenario->RequiresTestCharacter() ? 1 : 0);
        }
    }

    FAutoConsoleCommandWithWorldArgsAndOutputDevice GRunCommand(
        TEXT("convai.tests.Run"),
        TEXT("Run registered scenarios. Args: [-filter=<substring>] [-scenario=<exact name>] "
             "[-report=<path>] [quit]"),
        FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&RunScenarios));

    FAutoConsoleCommandWithWorldArgsAndOutputDevice GListCommand(
        TEXT("convai.tests.List"), TEXT("List registered scenarios."),
        FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&ListScenarios));
}
