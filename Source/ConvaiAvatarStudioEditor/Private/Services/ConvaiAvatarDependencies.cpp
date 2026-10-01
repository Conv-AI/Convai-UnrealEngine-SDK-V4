// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarDependencies.h"
#include "Packaging/ConvaiAvatarProcess.h"
#include "Workspace/ConvaiAvatarWorkspace.h"
#include "Workspace/ConvaiAvatarUploaderWorkspace.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Windows/WindowsHWrapper.h"

namespace ConvaiAvatarDependencies
{
bool ValidateInstallation(const FString& PluginDirectory, FString& Error)
{
    Error.Reset();
    if (PluginDirectory.IsEmpty())
    {
        Error = TEXT("The Convai plugin installation could not be found. Restore the complete plugin from the same SDK release, then retry.");
        return false;
    }
    const TCHAR* RequiredFiles[] = {
        TEXT("Resources/AvatarStudio/Transport/Resolve-Dependencies.ps1"),
        TEXT("Resources/AvatarStudio/Transport/Prepare-Transport.ps1"),
        TEXT("Resources/AvatarStudio/Archive.ps1")
    };
    for (const TCHAR* RelativeFile : RequiredFiles)
    {
        const FString File = PluginDirectory / RelativeFile;
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*File, FILEREAD_Silent));
        if (!Reader || Reader->TotalSize() <= 0)
        {
            Error = FString::Printf(TEXT("Cloud Avatars is missing a required file or cannot read it: %s. Restore the complete Convai plugin from the same SDK release, then retry."), RelativeFile);
            return false;
        }
    }
    return true;
}

namespace
{
bool RunStep(const FString& ResolutionFile, const std::atomic<bool>& Cancelled, FString& Error, bool bPrepare)
{
    Error.Reset();
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("ConvAI"));
    const FString Root = FPaths::ConvertRelativePathToFull(FConvaiAvatarWorkspace::GetProxyDirectory());
    if (!Plugin || ResolutionFile.Contains(TEXT("\"")) || !FPaths::IsUnderDirectory(ResolutionFile, Root / TEXT("Transport")))
    { Error = TEXT("The uploader update check could not be started. Reopen Cloud Avatars and retry."); return false; }
    if (!ValidateInstallation(Plugin->GetBaseDir(), Error)) return false;
    const FString ResultFile = bPrepare ? ResolutionFile + TEXT(".prepare.json") : ResolutionFile;
    const FString Script = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/AvatarStudio/Transport") /
        (bPrepare ? TEXT("Prepare-Transport.ps1") : TEXT("Resolve-Dependencies.ps1")));
    TCHAR SystemDirectory[MAX_PATH + 1]{}; GetSystemDirectoryW(SystemDirectory, UE_ARRAY_COUNT(SystemDirectory));
    const FString PowerShell = FString(SystemDirectory) / TEXT("WindowsPowerShell/v1.0/powershell.exe");
    FString Args = FString::Printf(TEXT("-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%s\" %s \"%s\" -EngineDirectory \"%s\" -ResultFile \"%s\""),
        *Script, bPrepare ? TEXT("-TransportDirectory") : TEXT("-UploaderDirectory"),
        *(bPrepare ? Root / TEXT("Transport") : Root), *FPaths::ConvertRelativePathToFull(FPaths::EngineDir()), *ResultFile);
    if (bPrepare) Args += FString::Printf(TEXT(" -ResolutionFile \"%s\" -WorkspaceLockHeld"), *ResolutionFile);
    void* Read = nullptr; void* Write = nullptr;
    if (!FPlatformProcess::CreatePipe(Read, Write)) { Error = TEXT("The uploader update check could not be started. Retry after closing other uploader jobs."); return false; }
    ON_SCOPE_EXIT { FPlatformProcess::ClosePipe(Read, Write); };
    FConvaiAvatarProcess Process;
    if (Cancelled.load()) { Error = TEXT("Uploader update check cancelled."); return false; }
    if (!Process.Start(PowerShell, Args, Write, Error)) return false;
    const double Deadline = FPlatformTime::Seconds() + (bPrepare ? 30 : 10) * 60;
    while (FPlatformProcess::IsProcRunning(Process.GetHandle()))
    {
        FPlatformProcess::ReadPipe(Read);
        if (Cancelled.load() || FPlatformTime::Seconds() > Deadline)
        {
            Process.Terminate();
            Error = Cancelled.load() ? TEXT("Uploader preparation cancelled.") : bPrepare
                ? TEXT("Preparing the uploader took too long. Open the uploader build log, check the reported problem, and retry.")
                : TEXT("Checking uploader updates timed out. Check your connection to GitHub and retry.");
            return false;
        }
        FPlatformProcess::Sleep(0.1f);
    }
    FPlatformProcess::ReadPipe(Read);
    int32 Code = -1; FPlatformProcess::GetProcReturnCode(Process.GetHandle(), &Code);
    TSharedPtr<FJsonObject> Json; FString Text; bool bSuccess = false;
    if (IFileManager::Get().FileSize(*ResultFile) >= 0 && IFileManager::Get().FileSize(*ResultFile) <= 64 * 1024 &&
        FFileHelper::LoadFileToString(Text, *ResultFile) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json &&
        Json->TryGetBoolField(TEXT("success"), bSuccess))
    {
        if (Code == 0 && bSuccess)
        {
            FString Project;
            if (!bPrepare || (Json->TryGetStringField(TEXT("project"), Project) &&
                FPaths::IsSamePath(Project, ConvaiAvatarUploaderWorkspace::ProjectPath(Root)))) return true;
            Error = TEXT("The uploader preparation returned a different project. Reopen Cloud Avatars and retry.");
            return false;
        }
        Json->TryGetStringField(TEXT("error"), Error);
    }
    if (Error.IsEmpty())
    {
        // Never emit child output here: GitHub redirects may contain signed URLs.
        // A missing result can be a local launch/script failure, not a network error.
        const bool bResultExists = IFileManager::Get().FileExists(*ResultFile);
        UE_LOG(LogTemp, Warning, TEXT("Cloud Avatars uploader helper failed: step=%s exit_code=%d result_file=%s usable_result=false"),
            bPrepare ? TEXT("prepare") : TEXT("resolve"), Code, bResultExists ? TEXT("present") : TEXT("missing"));
        Error = FString::Printf(TEXT("The uploader could not %s. Open the Cloud Avatars log for details, then retry. Diagnostic: exit code %d; result file %s."),
            bPrepare ? TEXT("finish preparing") : TEXT("check for updates"), Code,
            bResultExists ? TEXT("unreadable or incomplete") : TEXT("missing"));
    }
    return false;
}
}
bool Resolve(const FString& ResultFile, const std::atomic<bool>& Cancelled, FString& Error)
{
    return RunStep(ResultFile, Cancelled, Error, false);
}
bool Prepare(const FString& ResolutionFile, const std::atomic<bool>& Cancelled, FString& Error)
{
    return RunStep(ResolutionFile, Cancelled, Error, true);
}
}
