// Copyright Convai Inc. All Rights Reserved.
#include "Packaging/ConvaiAvatarProcess.h"
#include "Misc/ScopeExit.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"

// UE 5.3 targets an older Windows SDK declaration set. The job-list attribute
// is available on every supported Windows 10 host; retain atomic job assignment.
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
#define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

FConvaiAvatarProcess::~FConvaiAvatarProcess()
{
    Terminate();
    if (Job) CloseHandle(Job);
    if (Process.IsValid()) FPlatformProcess::CloseProc(Process);
}

bool FConvaiAvatarProcess::Start(const FString& Executable, const FString& Arguments, void* OutputPipe, FString& Error)
{
    if (Job || Process.IsValid()) { Error = TEXT("A packaging process is already attached."); return false; }
    Job = CreateJobObjectW(nullptr, nullptr);
    if (!Job) { Error = TEXT("Could not create the packaging process group."); return false; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
    // UE's shared Zen/DDC and tracing daemons explicitly request breakaway; they
    // must survive a cook. Ordinary cook/pack/shader workers remain in this job.
    Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)))
    { Error = TEXT("Could not bind packaging to the editor's lifetime."); return false; }

    // Assign the job as part of process creation. Assigning it afterwards leaves a
    // crash window in which a cook can outlive the editor and its workspace lock.
    SIZE_T AttributeBytes = 0;
    const DWORD AttributeCount = OutputPipe ? 2 : 1;
    InitializeProcThreadAttributeList(nullptr, AttributeCount, 0, &AttributeBytes);
    TArray<uint8> AttributeStorage;
    AttributeStorage.SetNumUninitialized(AttributeBytes);
    auto Attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeStorage.GetData());
    if (!InitializeProcThreadAttributeList(Attributes, AttributeCount, 0, &AttributeBytes))
    { Error = TEXT("Could not prepare the packaging process attributes."); return false; }
    ON_SCOPE_EXIT { DeleteProcThreadAttributeList(Attributes); };
    HANDLE JobHandle = Job;
    HANDLE PipeHandle = OutputPipe;
    if (!UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &JobHandle, sizeof(JobHandle), nullptr, nullptr) ||
        (OutputPipe && !UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &PipeHandle, sizeof(PipeHandle), nullptr, nullptr)))
    { Error = TEXT("Could not attach the packaging process group and output pipe."); return false; }
    STARTUPINFOEXW Startup{};
    Startup.StartupInfo.cb = sizeof(Startup);
    Startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | (OutputPipe ? STARTF_USESTDHANDLES : 0);
    Startup.StartupInfo.wShowWindow = SW_HIDE;
    Startup.StartupInfo.hStdOutput = OutputPipe;
    Startup.StartupInfo.hStdError = OutputPipe;
    Startup.lpAttributeList = Attributes;
    PROCESS_INFORMATION Information{};
    FString CommandLine = FString::Printf(TEXT("\"%s\" %s"), *Executable, *Arguments);
    if (!CreateProcessW(*Executable, CommandLine.GetCharArray().GetData(), nullptr, nullptr, OutputPipe != nullptr,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &Startup.StartupInfo, &Information))
    {
        Error = FString::Printf(TEXT("Could not start the packaging tool in its process group (Windows error %lu)."), GetLastError());
        return false;
    }
    CloseHandle(Information.hThread);
    Process = FProcHandle(Information.hProcess);
    return true;
}

void FConvaiAvatarProcess::Terminate()
{
    if (!Job) return;
    TerminateJobObject(Job, 1);
    const double Deadline = FPlatformTime::Seconds() + 10.0;
    if (Process.IsValid()) WaitForSingleObject(Process.Get(), 5000);
    // Cancellation must finish descendants before the caller releases the proxy
    // lock. Kill-on-close also enforces this lifetime after an editor crash.
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
    while (QueryInformationJobObject(Job, JobObjectBasicAccountingInformation, &Accounting, sizeof(Accounting), nullptr) && Accounting.ActiveProcesses &&
        FPlatformTime::Seconds() < Deadline)
        FPlatformProcess::Sleep(0.01f);
}
#include "Windows/HideWindowsPlatformTypes.h"
