
#include "Utility/Log/ConvaiLogger.h"
#include "../../../Convai.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "HAL/PlatformProcess.h" // for FPlatformProcess
#include "HAL/Event.h"           // for FEvent methods
#include "Misc/ScopeLock.h"
#include "Misc/ScopeTryLock.h"

namespace
{
    constexpr int64 MaxLogFileBytes = 50ll * 1024 * 1024;
    /** Start-up in a packaged build keeps this many ConvaiLogs files. */
    constexpr int32 KeptLogFiles = 20;
    constexpr double FlushIntervalSeconds = 1.0;
}

FConvaiLogger &FConvaiLogger::Get()
{
    static FConvaiLogger Instance;
    return Instance;
}

// The editor keeps every file: a run.py sweep writes dozens a day, and logs days
// old get read.
FConvaiLogger::FConvaiLogger()
    : FConvaiLogger(DefaultFolder(), MaxLogFileBytes, /*bPruneOldFiles*/ !WITH_EDITOR, /*bStartThread*/ true)
{
}

FConvaiLogger::FConvaiLogger(const FString& InFolder, int64 InMaxFileBytes, bool bPruneOldFiles, bool bStartThread)
    : Thread(nullptr)
    , WakeEvent(FPlatformProcess::GetSynchEventFromPool(false))
    , bStopping(false)
    , Folder(InFolder)
    , MaxFileBytes(InMaxFileBytes)
{
    if (bPruneOldFiles)
    {
        PruneOldFiles();
    }
    LogFilePath = CreateLogFilePath(TEXT(""), TEXT(""), Folder);
    if (bStartThread)
    {
        // Null where the platform has no threads (-nothreading); Log then writes
        // each line itself.
        Thread = FRunnableThread::Create(this, TEXT("ConvaiLoggerThread"), 0, TPri_BelowNormal);
    }
}

FConvaiLogger::~FConvaiLogger()
{
    Close();
    FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
}

void FConvaiLogger::Close()
{
    // First, so a line arriving during the teardown does not take the
    // thread-less branch (Thread is null once ShutdownThread has run) and
    // reopen the file.
    bClosed = true;
    ShutdownThread();
    // A modular build's exit kills every other thread before static
    // destruction, perhaps the writer while it holds the lock. Then only the
    // flush is skipped: destroying File still closes the file.
    FScopeTryLock Lock(&FileMutex);
    if (Lock.IsLocked())
    {
        FlushLocked(/*bFlushNow*/ true);
        File.Reset();
    }
}

FString FConvaiLogger::DefaultFolder()
{
    // Not ProjectDir()/Saved: an installed game (desktop Shipping) keeps Saved in
    // the user's folder, and its install folder may be read-only.
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ConvaiLogs"));
}

void FConvaiLogger::ShutdownThread()
{
    bStopping = true;
    WakeEvent->Trigger();
    if (Thread)
    {
        Thread->WaitForCompletion();
        delete Thread;
        Thread = nullptr;
    }
}

uint32 FConvaiLogger::Run()
{
    // Stopping is read before the queue is drained, so the last pass writes
    // every line queued before Stop.
    for (bool bLastPass = false; !bLastPass;)
    {
        WakeEvent->Wait(500);
        bLastPass = bStopping;
        WriteQueued(/*bFlushNow*/ bLastPass);
    }
    return 0;
}

void FConvaiLogger::Stop()
{
    bStopping = true;
}

FString FConvaiLogger::GetLogFilePath()
{
    FScopeLock Lock(&FileMutex);
    return LogFilePath;
}

void FConvaiLogger::PruneOldFiles() const
{
    IFileManager& Files = IFileManager::Get();
    TArray<FString> Names;
    Files.FindFiles(Names, *FPaths::Combine(Folder, TEXT("*.log")), /*Files*/ true, /*Directories*/ false);

    TArray<TPair<FDateTime, FString>> NewestFirst;
    for (const FString& Name : Names)
    {
        const FString Path = FPaths::Combine(Folder, Name);
        NewestFirst.Emplace(Files.GetTimeStamp(*Path), Path);
    }
    NewestFirst.Sort([](const TPair<FDateTime, FString>& A, const TPair<FDateTime, FString>& B) { return A.Key > B.Key; });

    // Another instance, such as a Pixel Streaming server on another port, may
    // still be writing a recent file.
    const FDateTime InUseAfter = FDateTime::UtcNow() - FTimespan::FromMinutes(10);
    for (int32 Index = KeptLogFiles; Index < NewestFirst.Num(); ++Index)
    {
        if (NewestFirst[Index].Key < InUseAfter)
        {
            Files.Delete(*NewestFirst[Index].Value);
        }
    }
}

void FConvaiLogger::WriteQueued(bool bFlushNow)
{
    FString Text;
    for (FQueuedLine Line; MessageQueue.Dequeue(Line);)
    {
        AppendLine(Text, Line.Time, Line.Message);
    }
    FScopeLock Lock(&FileMutex);
    WriteLocked(Text);
    FlushLocked(bFlushNow);
}

void FConvaiLogger::AppendLine(FString& Out, const FDateTime& Time, const FString& Message)
{
    Out += FString::Printf(TEXT("[%s] %s\n"), *Time.ToString(TEXT("%H:%M:%S")), *Message);
}

void FConvaiLogger::WriteLocked(const FString& Text)
{
    if (Text.IsEmpty())
    {
        return;
    }
    if (File && FileBytes >= MaxFileBytes)
    {
        File->Flush();
        File.Reset();
        LogFilePath = CreateLogFilePath(TEXT(""), TEXT(""), Folder);
    }
    if (!File)
    {
        IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
        const auto Open = [this, &Platform]
        {
#if WITH_TESTS
            ++FileOpensForTests;
#endif
            // Read shared, so support and the editor's log export can copy the
            // file while it is being written.
            File.Reset(Platform.OpenWrite(*LogFilePath, /*bAppend*/ true, /*bAllowRead*/ true));
        };
        Open();
        // CreateLogFilePath only checks that a name is free, and the file is
        // created at the first write: a process started in the same second
        // (multi-process PIE, servers without -PixelStreamingPort) may hold it.
        if (!File && Platform.FileExists(*LogFilePath))
        {
            LogFilePath = CreateLogFilePath(TEXT(""), TEXT(""), Folder);
            Open();
        }
        if (!File)
        {
            if (!bReportedOpenFailure)
            {
                bReportedOpenFailure = true;
                // Not CONVAI_LOG, which would queue into this logger.
                UE_LOG(LogConvai, Warning, TEXT("ConvaiLogs could not open %s; its lines are dropped"), *LogFilePath);
            }
            return;
        }
        FileBytes = File->Size();
    }
    const FTCHARToUTF8 Utf8(*Text);
    File->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    FileBytes += Utf8.Length();
    bUnflushed = true;
}

void FConvaiLogger::FlushLocked(bool bFlushNow)
{
    const double Now = FPlatformTime::Seconds();
    if (File && bUnflushed && (bFlushNow || Now >= NextFlushSeconds))
    {
        File->Flush();
        bUnflushed = false;
        NextFlushSeconds = Now + FlushIntervalSeconds;
    }
}

FString FConvaiLogger::CreateLogFilePath(const FString& ExtraSuffix, const FString& OverridePort,
    const FString& OverrideDir)
{
    // 1) Directory
    const FString LogDir = OverrideDir.IsEmpty() ? DefaultFolder() : OverrideDir;

    IPlatformFile& Plat = FPlatformFileManager::Get().GetPlatformFile();
    Plat.CreateDirectoryTree(*LogDir);

    // 2) Resolve port
    FString Port = OverridePort;
    if (Port.IsEmpty())
    {
        // Fallback to command line value (or "Default" if missing)
        if (!FParse::Value(FCommandLine::Get(), TEXT("PixelStreamingPort="), Port))
        {
            Port = TEXT("Default");
        }
    }

    // 3) Timestamp + base name: ProjectName_YYYYMMDD_HHMMSS
    const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    const FString ProjectName = FPaths::MakeValidFileName(FApp::GetProjectName());

    FString BaseName = FString::Printf(TEXT("%s_%s"), *ProjectName, *Timestamp);

    // 4) Optional [_Port]
    if (!Port.IsEmpty())
    {
        BaseName += FString::Printf(TEXT("_%s"), *FPaths::MakeValidFileName(Port));
    }

    // 5) Optional [_Suffix]
    if (!ExtraSuffix.IsEmpty())
    {
        BaseName += FString::Printf(TEXT("_%s"), *FPaths::MakeValidFileName(ExtraSuffix));
    }

    // 6) Ensure uniqueness: append _1, _2, ... if needed
    FString FileName = BaseName + TEXT(".log");
    FString CandidatePath = FPaths::Combine(LogDir, FileName);

    int32 SuffixIndex = 1;
    while (Plat.FileExists(*CandidatePath))
    {
        FileName = FString::Printf(TEXT("%s_%d.log"), *BaseName, SuffixIndex++);
        CandidatePath = FPaths::Combine(LogDir, FileName);
    }

    return CandidatePath;
}

void FConvaiLogger::Log(const FString &Message)
{
    // Static destruction does not stop other threads: a connect still being
    // torn down, or the native library, can log after it.
    if (bClosed)
    {
        return;
    }
#if WITH_TESTS
    if (bTapOn)
    {
        FScopeLock Lock(&TapMutex);
        if (!TapMarker.IsEmpty() && Message.Contains(TapMarker, ESearchCase::CaseSensitive))
        {
            TapLines.Add(Message);
        }
    }
#endif
    if (!Thread)
    {
        FString Text;
        AppendLine(Text, FDateTime::Now(), Message);
        FScopeLock Lock(&FileMutex);
        WriteLocked(Text);
        FlushLocked(/*bFlushNow*/ false);
        return;
    }
    MessageQueue.Enqueue(FQueuedLine{FDateTime::Now(), Message});
    WakeEvent->Trigger();
}

void UConvaiBlueprintLogger::C_ConvaiLog(UObject *WorldContextObject, EC_LogLevel Verbosity, const FString &Message)
{
    const FString ContextName = WorldContextObject
                                    ? WorldContextObject->GetName()
                                    : TEXT("UnknownContext");

    const UEnum *EnumPtr = StaticEnum<EC_LogLevel>();
    const FString VerbName = EnumPtr
                                 ? EnumPtr->GetNameStringByValue(static_cast<int64>(Verbosity))
                                 : TEXT("UnknownVerbosity");

    const FString FullMessage = FString::Printf(
        TEXT("%s : %s : %s"),
        *ContextName, *VerbName, *Message);

    // ConvaiLogs keeps every Blueprint line, whatever LogTemp's verbosity.
    FConvaiLogger::Get().Log(FullMessage);

    switch (Verbosity)
    {
    case EC_LogLevel::Verbose:
        UE_LOG(LogTemp, Verbose, TEXT("%s"), *FullMessage);
        break;
    case EC_LogLevel::Log:
        UE_LOG(LogTemp, Log, TEXT("%s"), *FullMessage);
        break;
    case EC_LogLevel::Warning:
        UE_LOG(LogTemp, Warning, TEXT("%s"), *FullMessage);
        break;
    case EC_LogLevel::Error:
        UE_LOG(LogTemp, Error, TEXT("%s"), *FullMessage);
        break;
    case EC_LogLevel::Fatal:
        UE_LOG(LogTemp, Fatal, TEXT("%s"), *FullMessage);
        break;
    default:
        UE_LOG(LogTemp, Log, TEXT("%s"), *FullMessage);
        break;
    }
}
