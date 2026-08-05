
#include "Utility/Log/ConvaiLogger.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformFile.h"
#include "Misc/App.h"
#include "HAL/PlatformProcess.h" // for FPlatformProcess
#include "HAL/Event.h"           // for FEvent methods

FConvaiLogger &FConvaiLogger::Get()
{
    static FConvaiLogger Instance;
    return Instance;
}

FConvaiLogger::FConvaiLogger()
    : Thread(nullptr), WakeEvent(FPlatformProcess::GetSynchEventFromPool(false)), bStopping(false)
{
    StartThread();
}

FConvaiLogger::~FConvaiLogger()
{
    ShutdownThread();
    if (WakeEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
    }
}

void FConvaiLogger::StartThread()
{
    LogFilePath = CreateLogFilePath();

    // Start the logger thread as before
    Thread = FRunnableThread::Create(
        this,
        TEXT("ConvaiLoggerThread"),
        0,
        TPri_BelowNormal);
}

void FConvaiLogger::ShutdownThread()
{
    bStopping = true;
    if (WakeEvent)
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
    while (!bStopping)
    {
        WakeEvent->Wait(500);

        TArray<FString> Batch;
        FString Msg;
        while (MessageQueue.Dequeue(Msg))
        {
            Batch.Add(Msg);
        }

        if (Batch.Num())
        {
            FString Combined = FString::Join(Batch, TEXT("\n")) + TEXT("\n");
            IPlatformFile &Plat = FPlatformFileManager::Get().GetPlatformFile();
            if (IFileHandle *Handle = Plat.OpenWrite(*LogFilePath, /*bAppend=*/true))
            {
                FTCHARToUTF8 Converter(*Combined);
                Handle->Write(reinterpret_cast<const uint8 *>(Converter.Get()), Converter.Length());
                Handle->Flush();
                delete Handle;
            }
        }
    }

    {
        TArray<FString> FinalBatch;
        FString Rem;
        while (MessageQueue.Dequeue(Rem))
        {
            FinalBatch.Add(Rem);
        }
        if (FinalBatch.Num())
        {
            const FString Combined = FString::Join(FinalBatch, TEXT("\n")) + TEXT("\n");
            IPlatformFile &Plat = FPlatformFileManager::Get().GetPlatformFile();
            if (IFileHandle *Handle = Plat.OpenWrite(*LogFilePath, /*bAppend=*/true))
            {
                const FTCHARToUTF8 Converter(*Combined);
                Handle->Write(reinterpret_cast<const uint8 *>(Converter.Get()), Converter.Length());
                Handle->Flush();
                delete Handle;
            }
        }
    }

    return 0;
}

void FConvaiLogger::Stop()
{
    bStopping = true;
}

FString FConvaiLogger::CreateLogFilePath(const FString& ExtraSuffix, const FString& OverridePort,
    const FString& OverrideDir)
{
    // 1) Directory
    const FString LogDir = OverrideDir.IsEmpty()
        ? FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved"), TEXT("ConvaiLogs"))
        : OverrideDir;

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
    const FString Formatted = FString::Printf(
        TEXT("[%s] %s"),
        *FDateTime::Now().ToString(TEXT("%H:%M:%S")),
        *Message);
    MessageQueue.Enqueue(Formatted);
    if (WakeEvent)
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

    switch (Verbosity)
    {
    case EC_LogLevel::Verbose:
        CONVAI_LOG(LogTemp, Verbose, TEXT("%s"), *FullMessage);
        break;
    case EC_LogLevel::Log:
        CONVAI_LOG(LogTemp, Log, TEXT("%s"), *FullMessage);
        break;
    case EC_LogLevel::Warning:
        CONVAI_LOG(LogTemp, Warning, TEXT("%s"), *FullMessage);
        break;
    case EC_LogLevel::Error:
        CONVAI_LOG(LogTemp, Error, TEXT("%s"), *FullMessage);
        break;
    case EC_LogLevel::Fatal:
        CONVAI_LOG(LogTemp, Fatal, TEXT("%s"), *FullMessage);
        break;
    default:
        CONVAI_LOG(LogTemp, Log, TEXT("%s"), *FullMessage);
        break;
    }

    FConvaiLogger::Get().Log(FullMessage);
}
