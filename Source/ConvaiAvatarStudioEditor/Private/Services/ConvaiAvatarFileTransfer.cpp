// Copyright Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarFileTransfer.h"
#include "Services/ConvaiAvatarFileTransferPrivate.h"
#include "Packaging/ConvaiAvatarPackagingPrivate.h"
#include "Packaging/ConvaiAvatarProcess.h"
#include "Workspace/ConvaiAvatarWorkspace.h"
#include "Workspace/ConvaiAvatarUploaderWorkspace.h"
#include "Services/ConvaiAvatarDependencies.h"
#include "Algo/AllOf.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <sddl.h>

namespace ConvaiAvatarFileTransferPrivate
{
constexpr int64 MaximumBytes = 10485760000LL;
const TCHAR* WorkerLongestRuntimeFile = TEXT("Transport/Configuration/0000000000000000000000000000000000000000000000000000000000000000.json");

bool ValidateWorkerPathBudget(const FString& Directory, FString& Error)
{
    // Matching precompiled modules need no generated UBT paths or compiler. This
    // check covers actual transfer files (configuration snapshots are longer than
    // job result paths and HTTP DLL paths); bootstrap checks a source build only
    // when the selected dependency has no matching precompiled editor modules.
    const int32 Length = FPaths::ConvertRelativePathToFull(Directory / WorkerLongestRuntimeFile).Len();
    if (Length < MAX_PATH) return true;
    Error = FString::Printf(TEXT("The transfer workspace exceeds the Windows file path limit (%d characters; maximum %d). Shorten the project location by at least %d characters and retry."), Length, MAX_PATH - 1, Length - (MAX_PATH - 1));
    return false;
}

TArray<FString> SweepJobDirectories(const FString& Jobs)
{
    TArray<FString> Names, Left;
    IFileManager::Get().FindFiles(Names, *(Jobs / TEXT("*")), false, true);
    for (const FString& Name : Names)
        if (Name.Len() == 32 && Algo::AllOf(Name, [](TCHAR C) { return FChar::IsHexDigit(C); }))
            Left.Append(ConvaiAvatarPackagingPrivate::DeleteOwnedTree(Jobs / Name, Jobs));
    return Left;
}

bool SafePath(const FString& Path)
{
    if (Path.IsEmpty() || FPaths::IsRelative(Path) || Path.StartsWith(TEXT("\\\\"))) return false;
    for (const TCHAR C : Path) if (C == TEXT('"') || C < 32) return false;
    FString Current = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Current);
    for (;;)
    {
        if (ConvaiAvatarPackagingPrivate::IsReparsePoint(Current)) return false;
        const FString Parent = FPaths::GetPath(Current);
        if (Parent.IsEmpty() || Parent == Current) break;
        Current = Parent;
    }
    return true;
}

bool ReadTransferJson(const FString& Path, TSharedPtr<FJsonObject>& Json)
{
    FString Text;
    return IFileManager::Get().FileSize(*Path) <= 64 * 1024 && FFileHelper::LoadFileToString(Text, *Path) &&
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid();
}

/** Inheritance is disabled; only this Windows account and SYSTEM may read URLs. */
FPrivateRequest::FPrivateRequest() : File(INVALID_HANDLE_VALUE) {}
FPrivateRequest::~FPrivateRequest()
{
    if (File != INVALID_HANDLE_VALUE) CloseHandle(File); // DELETE_ON_CLOSE also handles editor crashes.
    if (Descriptor) LocalFree(Descriptor);
}
bool FPrivateRequest::CreateDirectory(const FString& Directory, FString& Error)
{
    HANDLE Token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    { Error = TEXT("Could not protect the transfer request."); return false; }
    ON_SCOPE_EXIT { CloseHandle(Token); };
    DWORD Bytes = 0;
    GetTokenInformation(Token, TokenUser, nullptr, 0, &Bytes);
    TArray<uint8> Storage;
    Storage.SetNumUninitialized(Bytes);
    if (!Bytes || !GetTokenInformation(Token, TokenUser, Storage.GetData(), Bytes, &Bytes))
    { Error = TEXT("Could not identify the Windows account for this transfer."); return false; }
    LPWSTR Sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(Storage.GetData())->User.Sid, &Sid))
    { Error = TEXT("Could not protect the transfer request."); return false; }
    ON_SCOPE_EXIT { LocalFree(Sid); };
    const FString Dacl = FString::Printf(TEXT("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%s)"), Sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(*Dacl, SDDL_REVISION_1, &Descriptor, nullptr))
    { Error = TEXT("Could not protect the transfer request."); return false; }
    SECURITY_ATTRIBUTES Security{sizeof(SECURITY_ATTRIBUTES), Descriptor, FALSE};
    // A unique directory must be newly created: never adopt a preexisting job.
    if (!SafePath(Directory) || !CreateDirectoryW(*Directory, &Security))
    { Error = TEXT("Could not create a private transfer directory."); return false; }
    return true;
}
bool FPrivateRequest::Write(const FString& Path, const FString& Text, FString& Error)
{
    SECURITY_ATTRIBUTES Security{sizeof(SECURITY_ATTRIBUTES), Descriptor, FALSE};
    File = CreateFileW(*Path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        &Security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    const FTCHARToUTF8 Data(*Text);
    DWORD Written = 0;
    if (File == INVALID_HANDLE_VALUE || !WriteFile(File, Data.Get(), Data.Length(), &Written, nullptr) ||
        Written != static_cast<DWORD>(Data.Length()) || !FlushFileBuffers(File))
    { Error = TEXT("Could not save the private transfer request."); return false; }
    return true;
}

bool RunChild(const FString& Exe, const FString& Arguments, const std::atomic<bool>& Cancelled,
    const TFunction<void()>& Poll, FString& Error)
{
    if (Cancelled.load()) { Error = TEXT("Transfer cancelled."); return false; }
    int32 Code = -1;
    // No output callback: storage URLs must never reach host logs.
    switch (FConvaiAvatarProcess::RunToExit(Exe, Arguments, Cancelled, 6 * 60 * 60 + 120, Code, Error, {}, Poll))
    {
    case EConvaiAvatarProcessEnd::NotStarted:
        if (Error.IsEmpty()) Error = TEXT("Could not start the transfer worker.");
        return false;
    case EConvaiAvatarProcessEnd::Cancelled: Error = TEXT("Transfer cancelled."); return false;
    case EConvaiAvatarProcessEnd::TimedOut: Error = TEXT("The transfer worker timed out. Retry after checking your connection."); return false;
    case EConvaiAvatarProcessEnd::Exited: break;
    }
    if (Code != 0) { Error = TEXT("The transfer worker did not complete. Retry, or check its build details in the uploader cache."); return false; }
    return true;
}

FString InterpretTransferResult(const TSharedPtr<FJsonObject>& Result, const FString& ChildError, int64 LocalSize)
{
    if (!Result) return ChildError.IsEmpty() ? FString(TEXT("The transfer worker returned no completion record.")) : ChildError;
    bool bSuccess = false;
    if (!Result->TryGetBoolField(TEXT("success"), bSuccess) || !bSuccess)
    {
        FString Detail;
        if (Result->TryGetStringField(TEXT("error"), Detail) && !Detail.IsEmpty()) return Detail;
        return ChildError.IsEmpty() ? FString(TEXT("The transfer worker reported a failure without a reason. Retry the transfer.")) : ChildError;
    }
    FString BytesText, Md5, Transport;
    uint64 Bytes = 0;
    if (!ChildError.IsEmpty() || !Result->TryGetStringField(TEXT("bytes"), BytesText) || !LexTryParseString(Bytes, *BytesText) || Bytes == 0 || Bytes > MaximumBytes ||
        !Result->TryGetStringField(TEXT("md5"), Md5) || Md5.Len() != 32 || !Algo::AllOf(Md5, [](TCHAR C) { return FChar::IsHexDigit(C); }) ||
        !Result->TryGetStringField(TEXT("transport"), Transport) || Transport != TEXT("CONVAIHTTP-transfer-v3") ||
        LocalSize != static_cast<int64>(Bytes)) return TEXT("The transfer result could not be verified. Refresh the link before retrying.");
    return FString();
}
}

using namespace ConvaiAvatarFileTransferPrivate;

FConvaiAvatarFileTransfer::~FConvaiAvatarFileTransfer() { Shutdown(); }

void FConvaiAvatarFileTransfer::Cancel()
{
    ++Epoch;
    bCancelled.store(true);
}

void FConvaiAvatarFileTransfer::Shutdown()
{
    bShuttingDown.store(true);
    Cancel();
    if (Task.IsValid()) Task.Wait();
}

void FConvaiAvatarFileTransfer::Start(bool bUpload, FString Url, FString FilePath, FCompletion Completion, FProgress Progress, FString ResolutionFile, FProgressDetails ProgressDetails)
{
    check(IsInGameThread());
    if (bShuttingDown.load() || bBusy.exchange(true))
    { if (Completion) Completion(TEXT("Another file transfer is still finishing. Wait a moment and retry.")); return; }
    const uint64 CurrentEpoch = ++Epoch;
    bCancelled.store(false);
    const FString WorkspaceRoot = FPaths::ConvertRelativePathToFull(FConvaiAvatarWorkspace::GetProxyDirectory());
    const FString Directory = WorkspaceRoot / TEXT("Transport");
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ConvAI"));
    const FString ConvaiDirectory = Plugin ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()) : FString();
    const FString Resources = Plugin ? ConvaiDirectory / TEXT("Resources/AvatarStudio/Transport") : FString();
    const FString Engine = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
    TWeakPtr<FConvaiAvatarFileTransfer, ESPMode::ThreadSafe> WeakOwner = AsShared();
    Task = Async(EAsyncExecution::Thread, [this, WeakOwner, CurrentEpoch, bUpload, Url = MoveTemp(Url), FilePath = MoveTemp(FilePath),
        WorkspaceRoot, Directory, ConvaiDirectory, Resources, Engine, ResolutionFile = MoveTemp(ResolutionFile), Completion = MoveTemp(Completion), Progress = MoveTemp(Progress), ProgressDetails = MoveTemp(ProgressDetails)]() mutable
    {
        FString Error;
        const FString Jobs = Directory / TEXT("Jobs");
        const FString JobDirectory = Jobs / FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString RequestPath = JobDirectory / TEXT("request.json");
        FConvaiAvatarUploaderLease WorkspaceLease;
        FPrivateRequest PrivateRequest;
        const bool bAllowedUrl = Url.StartsWith(TEXT("https://storage.googleapis.com/user-assets-storage/"), ESearchCase::IgnoreCase) ||
            Url.StartsWith(TEXT("https://user-assets-storage.storage.googleapis.com/"), ESearchCase::IgnoreCase);
        if (!bAllowedUrl || Url.Contains(TEXT("\n")) || Url.Contains(TEXT("\r")) || !SafePath(FilePath) ||
            !SafePath(Jobs) || !SafePath(Resources) || !SafePath(Engine)) Error = TEXT("The file transfer has an invalid URL or local path.");
        else if (bUpload && (IFileManager::Get().FileSize(*FilePath) <= 0 || IFileManager::Get().FileSize(*FilePath) > MaximumBytes))
            Error = TEXT("Choose a nonempty upload file no larger than 10,485,760,000 bytes.");
        else if (!bUpload && IFileManager::Get().FileExists(*FilePath)) Error = TEXT("The download destination already exists.");
        else if (!ValidateWorkerPathBudget(WorkspaceRoot, Error)) {}
        else if (!WorkspaceLease.Acquire(WorkspaceRoot, Error)) {}
        else if (!ConvaiAvatarUploaderWorkspace::EnsureProject(WorkspaceRoot, Error)) {}
        else if (!IFileManager::Get().MakeDirectory(*Jobs, true)) Error = TEXT("Could not create the transfer workspace.");
        else
        {
            // Best effort, before this job exists. It also removes this editor's own finished jobs.
            ConvaiAvatarPackagingPrivate::WarnScratchKept(SweepJobDirectories(Jobs));
            PrivateRequest.CreateDirectory(JobDirectory, Error);
        }

        if (Error.IsEmpty())
        {
            if (ResolutionFile.IsEmpty())
            {
                ResolutionFile = JobDirectory / TEXT("http-resolution.json");
                ConvaiAvatarDependencies::Resolve(ConvaiDirectory, ResolutionFile, bCancelled, Error);
            }
            else if (!SafePath(ResolutionFile) || !FPaths::IsUnderDirectory(ResolutionFile, Directory))
                Error = TEXT("The upload dependency resolution is outside this project's transfer data. Prepare the upload again.");
        }

        // One bootstrap per transfer: it is the readiness gate for this lease, and a cache hit costs seconds.
        if (Error.IsEmpty()) ConvaiAvatarDependencies::Prepare(ConvaiDirectory, ResolutionFile, bCancelled, Error);
        if (Error.IsEmpty() && bCancelled.load()) Error = TEXT("Transfer cancelled.");
        // Prepare succeeds only when its result names this uploader project.
        const FString WorkerProject = ConvaiAvatarUploaderWorkspace::ProjectPath(WorkspaceRoot);
        if (Error.IsEmpty() && !SafePath(WorkerProject)) Error = TEXT("The transfer worker returned a different uploader project.");
        if (Error.IsEmpty())
        {
            TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
            Request->SetStringField(TEXT("method"), bUpload ? TEXT("PUT") : TEXT("GET"));
            Request->SetStringField(TEXT("url"), Url);
            Request->SetStringField(TEXT("file"), FilePath);
            FString Json;
            FJsonSerializer::Serialize(Request, TJsonWriterFactory<>::Create(&Json));
            PrivateRequest.Write(RequestPath, Json, Error);
        }
        Url.Reset();
        if (Error.IsEmpty())
        {
            const FString Exe = Engine / TEXT("Binaries/Win64/UnrealEditor-Cmd.exe");
            // UE processes explicit enables first, then wildcard disables, before project/default plugins.
            // NoEnginePlugins skips target/project discovery. The restored CA descriptor is kept intact,
            // while only the dependency's HTTP plugin (no SDK or avatar plugin) loads for this commandlet.
            const FString Arguments = FString::Printf(TEXT("\"%s\" -run=ConvaiAvatarTransport -AvatarTransferRequest=\"%s\" -EnablePlugins=ConvaiHTTP -DisablePlugins=* -NoEnginePlugins -unattended -nop4 -nosplash -nullrhi -RenderOffscreen -nosound -LogCmds=\"LogConvaihttp off\""), *WorkerProject, *RequestPath);
            float PreviousProgress = -1.0f;
            int64 PreviousBytes = -1, PreviousTotal = -1;
            auto ReportProgress = [&](const FConvaiAvatarTransferProgress& Details)
            {
                if (!Progress && !ProgressDetails) return;
                const int64 Total = Details.TotalBytes.Get(-1);
                if (PreviousBytes == Details.CompletedBytes && PreviousTotal == Total) return;
                PreviousBytes = Details.CompletedBytes; PreviousTotal = Total;
                const TOptional<float> Fraction = Details.Fraction();
                const bool bReportFraction = Fraction.IsSet() && Fraction.GetValue() != PreviousProgress;
                if (Fraction.IsSet()) PreviousProgress = Fraction.GetValue();
                AsyncTask(ENamedThreads::GameThread, [WeakOwner, CurrentEpoch, Callback = Progress, DetailCallback = ProgressDetails, Details, Fraction, bReportFraction]()
                {
                    const auto Self = WeakOwner.Pin();
                    if (!Self || Self->Epoch.load() != CurrentEpoch || Self->bShuttingDown.load()) return;
                    if (DetailCallback) DetailCallback(Details);
                    // A details callback may synchronously cancel the operation.
                    if (bReportFraction && Callback && Self->Epoch.load() == CurrentEpoch && !Self->bShuttingDown.load()) Callback(Fraction.GetValue());
                });
            };
            RunChild(Exe, Arguments, bCancelled, [&]()
            {
                if (!Progress && !ProgressDetails) return;
                TSharedPtr<FJsonObject> State;
                FConvaiAvatarTransferProgress Details;
                if (ReadTransferJson(RequestPath + TEXT(".progress.json"), State) && FConvaiAvatarTransferProgress::Parse(*State, Details)) ReportProgress(Details);
            }, Error);
            TSharedPtr<FJsonObject> Result;
            if (!ReadTransferJson(RequestPath + TEXT(".result.json"), Result)) Result.Reset();
            const int64 LocalSize = IFileManager::Get().FileSize(*FilePath);
            Error = InterpretTransferResult(Result, Error, LocalSize);
            if (Error.IsEmpty())
            {
                // A download may have no Content-Length; validated completion establishes its total.
                FConvaiAvatarTransferProgress Complete;
                Complete.CompletedBytes = LocalSize; Complete.TotalBytes = LocalSize;
                ReportProgress(Complete);
            }
        }
        // Only this job's known temporary file is removed after its child has exited.
        IFileManager::Get().Delete(*(RequestPath + TEXT(".body.part")), false, true);
        // The child has exited and no shared descriptor/plugin is in use. Release before UI callbacks.
        WorkspaceLease.Release();
        bBusy.store(false);
        AsyncTask(ENamedThreads::GameThread, [WeakOwner, CurrentEpoch, Completion = MoveTemp(Completion), Error]() mutable
        {
            if (const auto Self = WeakOwner.Pin(); Self && Self->Epoch.load() == CurrentEpoch && !Self->bShuttingDown.load() && Completion) Completion(Error);
        });
    });
}



#include "Windows/HideWindowsPlatformTypes.h"
