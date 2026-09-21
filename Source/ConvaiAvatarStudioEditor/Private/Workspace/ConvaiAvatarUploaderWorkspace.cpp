// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarUploaderWorkspace.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Windows/WindowsHWrapper.h"

namespace
{
const TCHAR* UploaderOwnerName = TEXT("ConvaiAvatarUploader");
const TCHAR* WorkspaceDescription = TEXT("Managed Cloud Avatars uploader workspace.");
FString Full(FString Path) { Path = FPaths::ConvertRelativePathToFull(Path); FPaths::NormalizeFilename(Path); Path.RemoveFromEnd(TEXT("/")); return Path; }

bool Local(const FString& Path)
{
    if (FPaths::IsRelative(Path) || Path.StartsWith(TEXT("\\\\")) || Path.Contains(TEXT("\""))) return false;
    for (const TCHAR C : Path) if (C < 32) return false;
    FString Part = Full(Path);
    for (;;)
    {
        const DWORD Attributes = GetFileAttributesW(*Part);
        if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const FString Parent = FPaths::GetPath(Part);
        if (Parent.IsEmpty() || Parent == Part) return true;
        Part = Parent;
    }
}

bool Parse(const FString& Text, TSharedPtr<FJsonObject>& Json)
{
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid();
}
bool Read(const FString& Path, TSharedPtr<FJsonObject>& Json, FString* Original = nullptr)
{
    FString Text;
    if (!Local(Path) || IFileManager::Get().FileSize(*Path) < 0 || IFileManager::Get().FileSize(*Path) > 128 * 1024 ||
        !FFileHelper::LoadFileToString(Text, *Path) || !Parse(Text, Json)) return false;
    if (Original) *Original = Text;
    return true;
}
bool ManagedProject(const TSharedPtr<FJsonObject>& Json)
{
    double Version = 0; FString Text;
    return Json && Json->TryGetNumberField(TEXT("FileVersion"), Version) && Version == 3 &&
        Json->TryGetStringField(TEXT("Description"), Text) && Text == WorkspaceDescription;
}
bool WriteText(const FString& Path, const FString& Text)
{
    const FString Temp = Path + TEXT(".tmp");
    return Local(Path) && Local(Temp) &&
        FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) &&
        MoveFileExW(*Temp, *Path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
bool Write(const FString& Path, const TSharedRef<FJsonObject>& Json)
{
    FString Text;
    return FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text)) && WriteText(Path, Text);
}
}

FConvaiAvatarUploaderLease::~FConvaiAvatarUploaderLease() { Release(); }
void FConvaiAvatarUploaderLease::Release()
{
    if (Handle) { CloseHandle(Handle); Handle = nullptr; }
}
bool FConvaiAvatarUploaderLease::Acquire(const FString& Directory, FString& Error)
{
    Error.Reset();
    if (Handle) { Error = TEXT("This uploader job already holds its workspace. Finish the current operation before starting another."); return false; }
    const FString Root = Full(Directory), Path = Root / TEXT("packaging.lock");
    if (Root == Full(FPaths::ProjectDir()) || !Local(Root) || !Local(Path) || !IFileManager::Get().MakeDirectory(*Root, true))
    { Error = TEXT("The uploader folder is unavailable or points elsewhere. Use a local project folder and retry."); return false; }
    const HANDLE Result = CreateFileW(*Path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (Result == INVALID_HANDLE_VALUE)
    { Error = TEXT("This project's uploader is already in use. Wait for its current job to finish, then retry."); return false; }
    Handle = Result;
    return true;
}

namespace ConvaiAvatarUploaderWorkspace
{
FString ProjectPath(const FString& Root) { return Full(Root) / TEXT("CA.uproject"); }

bool EnsureProject(const FString& Directory, FString& Error)
{
    Error.Reset();
    const FString Root = Full(Directory), Project = ProjectPath(Root);
    const FString MarkerPath = Root / TEXT(".convai-workspace.json");
    const FString Legacy = Root / TEXT("AvatarStudioUploader.uproject");
    const FString LegacyBackup = Root / TEXT("AvatarStudioUploader.legacy-project.json");
    const FString BootstrapBackup = Root / TEXT("CA.bootstrap-backup.json");
    for (const FString& Path : {Root, Project, MarkerPath, Legacy, LegacyBackup, BootstrapBackup})
        if (!Local(Path) || Root == Full(FPaths::ProjectDir()))
        { Error = TEXT("Uploader setup stopped because a managed folder points elsewhere. Keep your avatar files and restore a normal local uploader folder before retrying."); return false; }

    TArray<FString> Projects;
    IFileManager::Get().FindFiles(Projects, *(Root / TEXT("*.uproject")), true, false);
    if (Projects.Num() > 1 || (Projects.Num() == 1 && Projects[0] != TEXT("CA.uproject") && Projects[0] != TEXT("AvatarStudioUploader.uproject")))
    { Error = TEXT("Another Unreal project occupies the uploader folder. Move that project to its own folder before retrying; its files have been kept."); return false; }

    TSharedPtr<FJsonObject> Marker;
    const bool bMarkerExists = IFileManager::Get().FileExists(*MarkerPath);
    double Version = 0; FString RecordedOwner;
    if (bMarkerExists && (!Read(MarkerPath, Marker) || !Marker->TryGetNumberField(TEXT("version"), Version) ||
        (Version != 1 && Version != 2) || (Version == 2 && (!Marker->TryGetStringField(TEXT("owner"), RecordedOwner) || RecordedOwner != UploaderOwnerName))))
    { Error = TEXT("The uploader's ownership record cannot be verified. Keep the uploader folder and its backups, and restore the record before retrying."); return false; }
    if (!bMarkerExists && (!Projects.IsEmpty() || IFileManager::Get().FileExists(*LegacyBackup) || IFileManager::Get().FileExists(*BootstrapBackup)))
    { Error = TEXT("An existing uploader project has no ownership record. Its files were kept; restore the record before retrying."); return false; }
    if (!Projects.IsEmpty() && !IFileManager::Get().FileExists(*BootstrapBackup))
    {
        TSharedPtr<FJsonObject> Existing;
        if (!Read(Root / Projects[0], Existing) || !ManagedProject(Existing))
        { Error = TEXT("The uploader folder contains project settings that this tool did not create. Its files were kept; restore the managed uploader settings before retrying."); return false; }
    }
    if (!Marker) Marker = MakeShared<FJsonObject>();

    // Persist ownership before any project creation. A failed creation is retryable.
    Marker->SetNumberField(TEXT("version"), 2); Marker->SetStringField(TEXT("owner"), UploaderOwnerName);
    if (!Write(MarkerPath, Marker.ToSharedRef())) { Error = TEXT("The uploader setup record could not be saved. Check folder permissions and retry."); return false; }

    if (IFileManager::Get().FileExists(*BootstrapBackup))
    {
        TSharedPtr<FJsonObject> Backup, SavedProject; FString SavedText, BackupOwner; double BackupVersion = 0;
        if (!Read(BootstrapBackup, Backup) || !Backup->TryGetStringField(TEXT("owner"), BackupOwner) || BackupOwner != UploaderOwnerName ||
            !Backup->TryGetNumberField(TEXT("version"), BackupVersion) || BackupVersion != 1 ||
            !Backup->TryGetStringField(TEXT("project_json"), SavedText) || !Parse(SavedText, SavedProject) || !ManagedProject(SavedProject) ||
            IFileManager::Get().FileExists(*Legacy))
        { Error = TEXT("Uploader setup was interrupted, and its saved settings cannot be verified. Keep the uploader folder and backups for recovery."); return false; }
        if (!WriteText(Project, SavedText) || !IFileManager::Get().Delete(*BootstrapBackup, false, false, true))
        { Error = TEXT("The uploader's saved settings could not be restored. Close other uploader processes, check folder permissions, and retry."); return false; }
    }

    TSharedPtr<FJsonObject> Json; FString Original;
    if (IFileManager::Get().FileExists(*Legacy))
    {
        if (!Read(Legacy, Json, &Original) || !ManagedProject(Json) || IFileManager::Get().FileExists(*LegacyBackup))
        { Error = TEXT("The older uploader project cannot be upgraded automatically. Its files and avatar links were kept; check the uploader folder before retrying."); return false; }
        // Rename only the verified generated descriptor, retaining its bytes as a backup.
        if (!MoveFileExW(*Legacy, *LegacyBackup, MOVEFILE_WRITE_THROUGH))
        { Error = TEXT("The older uploader project is in use. Close that project and retry."); return false; }
    }
    if (!IFileManager::Get().FileExists(*Project))
    {
        if (IFileManager::Get().FileExists(*LegacyBackup))
        {
            if (!Read(LegacyBackup, Json, &Original) || !ManagedProject(Json))
            { Error = TEXT("The saved uploader project cannot be read. Keep its backup and restore it before retrying."); return false; }
        }
        else
        {
            Json = MakeShared<FJsonObject>(); Json->SetNumberField(TEXT("FileVersion"), 3);
            Json->SetStringField(TEXT("EngineAssociation"), FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
            Json->SetStringField(TEXT("Description"), WorkspaceDescription); Json->SetBoolField(TEXT("DisableEnginePluginsByDefault"), true);
            Json->SetArrayField(TEXT("Plugins"), {});
            FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Original));
        }
        if (!WriteText(Project, Original)) { Error = TEXT("The uploader project could not be saved. Check folder permissions and retry."); return false; }
    }
    if (!Read(Project, Json) || !ManagedProject(Json))
    { Error = TEXT("The uploader project settings could not be verified. Its files were kept; restore the managed project settings before retrying."); return false; }
    return true;
}
}
