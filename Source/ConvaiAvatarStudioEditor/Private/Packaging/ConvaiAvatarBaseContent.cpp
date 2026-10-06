// Copyright Convai Inc. All Rights Reserved.
#include "Packaging/ConvaiAvatarBaseContent.h"
#include "Misc/EngineVersionComparison.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopedSlowTask.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "Windows/WindowsHWrapper.h"

DEFINE_LOG_CATEGORY_STATIC(LogConvaiAvatarSupport, Log, All);

namespace ConvaiAvatarBaseContent
{
namespace
{
FString Full(FString Path)
{
    Path = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeDirectoryName(Path);
    FPaths::CollapseRelativeDirectories(Path);
    return Path;
}
bool Below(const FString& Path, const FString& Root)
{
    return Full(Path).StartsWith(Full(Root) + TEXT("/"), ESearchCase::IgnoreCase);
}
bool NoRedirects(FString Path, const FString& Root, FString& Error)
{
    Path = Full(Path);
    const FString Boundary = Full(Root);
    if (Path != Boundary && !Below(Path, Boundary)) { Error = TEXT("A base-content path escapes its managed directory."); return false; }
    for (;;)
    {
        const DWORD Attributes = GetFileAttributesW(*Path);
        if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { Error = TEXT("Shared avatar support cannot use a redirected file or folder: ") + Path; return false; }
        if (Path == Boundary) return true;
        Path = FPaths::GetPath(Path);
    }
}
bool ValidRelative(const FString& Path)
{
    if (Path.IsEmpty() || Path.StartsWith(TEXT("/")) || Path.Contains(TEXT("\\")) || Path.Contains(TEXT(":"))) return false;
    TArray<FString> Parts;
    Path.ParseIntoArray(Parts, TEXT("/"), false);
    for (const FString& Part : Parts)
    {
        if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" "))) return false;
        for (TCHAR C : Part) if (C < 32 || C == TEXT('"') || C == TEXT('*') || C == TEXT('?') || C == TEXT('|') || C == TEXT('<') || C == TEXT('>')) return false;
    }
    const FString Extension = FPaths::GetExtension(Path).ToLower();
    return Extension == TEXT("uasset") || Extension == TEXT("uexp") || Extension == TEXT("ubulk") || Extension == TEXT("uptnl");
}
bool Read(const FString& Path, TSharedPtr<FJsonObject>& Json)
{
    FString Text;
    return FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid();
}
bool Write(const FString& Path, const TSharedRef<FJsonObject>& Json)
{
    FString Text;
    const FString Temporary = Path + TEXT(".tmp");
    FString Error;
    if (!NoRedirects(Temporary, FConvaiAvatarWorkspace::GetProxyDirectory(), Error)) return false;
    return FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text)) &&
        FFileHelper::SaveStringToFile(Text, *Temporary) && IFileManager::Get().Move(*Path, *Temporary, true, true);
}
}

bool Gather(const FConvaiAvatarPreparedAsset& Asset, TMap<FString, FString>& OutFiles,
    TArray<FString>& OutPluginDependencies, const std::atomic<bool>& Cancelled, FString& Error)
{
    check(IsInGameThread());
    OutFiles.Reset();
    OutPluginDependencies.Reset();
    Error.Reset();
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.WaitForCompletion();
    const FString AvatarMount = TEXT("/") + Asset.PluginName + TEXT("/");
    const FName DioramaRoot = Asset.Diorama.IsSet() && Asset.Diorama->Level.StartsWith(AvatarMount, ESearchCase::CaseSensitive)
        ? FName(*Asset.Diorama->Level) : NAME_None;
    TArray<FAssetData> Seeds;
    Registry.GetAssetsByPath(FName(*(TEXT("/") + Asset.PluginName)), Seeds, true, true);
    TArray<FName> Queue;
    for (const FAssetData& Seed : Seeds) Queue.AddUnique(Seed.PackageName);
    Queue.AddUnique(FName(*Asset.EntryPoint.GetLongPackageName()));
    TSet<FName> Seen;
    FScopedSlowTask Progress(0, NSLOCTEXT("ConvaiAvatarStudio", "CheckBaseSupport", "Checking shared avatar support…"));
    Progress.MakeDialogDelayed(0.5f, true);
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
    const UE::AssetRegistry::FDependencyQuery RuntimeDependencies(UE::AssetRegistry::EDependencyQuery::Propagation);
#endif
    for (int32 Index = 0; Index < Queue.Num(); ++Index)
    {
        if (Cancelled.load() || Progress.ShouldCancel()) { Error = TEXT("Packaging cancelled while checking shared avatar support."); return false; }
        const FName Package = Queue[Index];
        if (Seen.Contains(Package)) continue;
        Seen.Add(Package);
        const FString Name = Package.ToString();
        if (Name.StartsWith(TEXT("/Script/")) || Name.StartsWith(TEXT("/Engine/"))) continue;
        if (Seen.Num() > 20000) { Error = TEXT("The shared avatar support references too many packages. Remove unrelated runtime dependencies and try again."); return false; }
        Progress.EnterProgressFrame(0, FText::FromString(TEXT("Checking shared support: ") + Name));
        FString Filename;
        if (!FPackageName::DoesPackageExist(Name, &Filename))
        {
            // Preparation already reviewed exact missing external references. They
            // have no support bytes to copy and must not become plugin requirements.
            // Recheck the disk: a restored asset must follow normal validation, and
            // an unsaved new asset or missing prepared payload is never waived.
            if (UPackage* Loaded = FindPackage(nullptr, *Name); Loaded && Loaded->IsDirty())
            { Error = TEXT("Save the avatar dependency ") + Name + TEXT(" before uploading."); return false; }
            if (!Name.StartsWith(AvatarMount) && Asset.AcknowledgedMissingPackages.Contains(Package))
            {
                UE_LOG(LogConvaiAvatarSupport, Display, TEXT("Skipping acknowledged missing external avatar dependency: %s"), *Name);
                continue;
            }
            Error = Name.StartsWith(AvatarMount)
                ? TEXT("A prepared avatar asset is missing: ") + Name + TEXT(". Restore it before uploading.")
                : TEXT("A required avatar dependency is missing or unavailable: ") + Name + TEXT(". Restore the asset or enable its plugin before uploading.");
            return false;
        }
        TArray<FAssetData> Exports;
        Registry.GetAssetsByPackageName(Package, Exports, true);
        if (Package != DioramaRoot && Exports.ContainsByPredicate([](const FAssetData& Data) { return Data.AssetClassPath == UWorld::StaticClass()->GetClassPathName(); }))
        { Error = TEXT("Shared avatar support references a level: ") + Name + TEXT(". Remove this scene dependency before packaging."); return false; }
        if (Name.StartsWith(TEXT("/Game/")))
        {
            if (UPackage* Loaded = FindPackage(nullptr, *Name); Loaded && Loaded->IsDirty())
            { Error = TEXT("Save the shared avatar support asset ") + Name + TEXT(" before packaging."); return false; }
            if (FPaths::GetExtension(Filename) != TEXT("uasset"))
            { Error = TEXT("The installed Convai content needs a shared support asset that is missing: ") + Name + TEXT(". Restore this MetaHuman common asset in the project before packaging."); return false; }
            Filename = Full(Filename);
            const FString HostContent = Full(FPaths::ProjectContentDir());
            if (!Below(Filename, HostContent) || !NoRedirects(Filename, HostContent, Error)) return false;
            OutFiles.Add(Filename.Mid(HostContent.Len() + 1), Filename);
            for (const TCHAR* Extension : {TEXT("uexp"), TEXT("ubulk"), TEXT("uptnl")})
            {
                const FString Sidecar = FPaths::ChangeExtension(Filename, Extension);
                if (FPaths::FileExists(Sidecar)) OutFiles.Add(Sidecar.Mid(HostContent.Len() + 1), Sidecar);
            }
        }
        else if (!Name.StartsWith(AvatarMount))
        {
            const FString PluginName = Name.Mid(1).Left(Name.Mid(1).Find(TEXT("/")));
            const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
            if (!Plugin || !Plugin->IsEnabled()) { Error = TEXT("Enable the plugin required by shared avatar support: ") + PluginName; return false; }
            OutPluginDependencies.AddUnique(Plugin->GetName());
        }
        TArray<FName> Dependencies;
        // Propagation means Game OR Build in UE: include hard/soft runtime and cooker-transform
        // inputs, while excluding dependencies used only for editor previews. /Script modules and
        // installed engine content stay external.
#if UE_VERSION_OLDER_THAN(5, 5, 0)
        // Game | Build means AND in old query flags. Union two queries for OR.
        Registry.GetDependencies(Package, Dependencies, UE::AssetRegistry::EDependencyCategory::Package,
            UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Game));
        TArray<FName> BuildDependencies;
        Registry.GetDependencies(Package, BuildDependencies, UE::AssetRegistry::EDependencyCategory::Package,
            UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Build));
        for (const FName Dependency : BuildDependencies) Dependencies.AddUnique(Dependency);
#else
        Registry.GetDependencies(Package, Dependencies, UE::AssetRegistry::EDependencyCategory::Package, RuntimeDependencies);
#endif
        Queue.Append(Dependencies);
    }
    return true;
}

bool Snapshot(const FString& HostContent, const FString& ProxyDirectory, const FString& SelectedPlugin,
    const TMap<FString, FString>& Files, const std::atomic<bool>& Cancelled, FString& Error)
{
    const FString Proxy = Full(ProxyDirectory);
    const FString OwnedRoot = Full(FConvaiAvatarWorkspace::GetProxyDirectory());
    if ((Proxy != OwnedRoot && !Below(Proxy, OwnedRoot)) || !FConvaiAvatarWorkspace::IsSafePluginName(SelectedPlugin) || !NoRedirects(Proxy, OwnedRoot, Error))
    { if (Error.IsEmpty()) Error = TEXT("Invalid shared avatar support snapshot destination."); return false; }
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    const FString Content = Proxy / TEXT("Content");
    const FString ManifestPath = Proxy / TEXT("ConvaiAvatarBaseContent.json");
    if (!NoRedirects(Content, OwnedRoot, Error) || !NoRedirects(ManifestPath, OwnedRoot, Error)) return false;
    TSharedPtr<FJsonObject> Previous;
    const TSharedPtr<FJsonObject>* PreviousFiles = nullptr;
    if (PF.FileExists(*ManifestPath) && (!Read(ManifestPath, Previous) || !Previous->TryGetObjectField(TEXT("files"), PreviousFiles)))
    { Error = TEXT("The shared avatar support ownership record is invalid. Preserve it for diagnosis before resetting the uploader cache."); return false; }
    auto DesiredFiles = MakeShared<FJsonObject>();
    auto OwnedFiles = MakeShared<FJsonObject>();
    if (PreviousFiles) OwnedFiles->Values = (*PreviousFiles)->Values;
    for (const auto& Pair : Files)
    {
        if (Cancelled.load()) { Error = TEXT("Packaging cancelled."); return false; }
        const FString Target = Content / Pair.Key;
        if (!ValidRelative(Pair.Key) || !Full(Pair.Value).Equals(Full(HostContent) / Pair.Key, ESearchCase::IgnoreCase) ||
            !NoRedirects(Pair.Value, HostContent, Error) || !NoRedirects(Target, OwnedRoot, Error))
        { if (Error.IsEmpty()) Error = TEXT("Unsafe shared avatar support snapshot path."); return false; }
        if (PF.FileExists(*Target) && (!PreviousFiles || !(*PreviousFiles)->HasField(Pair.Key)))
        { Error = TEXT("An unowned file already occupies the shared avatar support cache: ") + Pair.Key; return false; }
        const FMD5Hash Hash = FMD5Hash::HashFile(*Pair.Value);
        if (!Hash.IsValid()) { Error = TEXT("Could not read shared avatar support: ") + Pair.Key; return false; }
        DesiredFiles->SetStringField(Pair.Key, LexToString(Hash));
        OwnedFiles->SetStringField(Pair.Key, LexToString(Hash));
    }
    auto Manifest = MakeShared<FJsonObject>();
    Manifest->SetNumberField(TEXT("schema_version"), 1);
    Manifest->SetStringField(TEXT("selected_plugin"), SelectedPlugin);
    Manifest->SetBoolField(TEXT("complete"), false);
    Manifest->SetObjectField(TEXT("files"), OwnedFiles);
    if (!PF.CreateDirectoryTree(*Content) || !Write(ManifestPath, Manifest)) { Error = TEXT("Could not record shared avatar support ownership."); return false; }
    // Record intent before copying. A cancelled or failed snapshot remains owned and resumable.
    for (const auto& Pair : Files)
    {
        if (Cancelled.load()) { Error = TEXT("Packaging cancelled."); return false; }
        const FString Target = Content / Pair.Key;
        const FString ExpectedHash = DesiredFiles->GetStringField(Pair.Key);
        if (!PF.CreateDirectoryTree(*FPaths::GetPath(Target))) { Error = TEXT("Could not create shared support folders."); return false; }
        if (!PF.FileExists(*Target) || LexToString(FMD5Hash::HashFile(*Target)) != ExpectedHash)
        {
            if (!PF.CopyFile(*Target, *Pair.Value)) { Error = TEXT("Could not copy shared avatar support: ") + Pair.Key; return false; }
        }
        if (LexToString(FMD5Hash::HashFile(*Pair.Value)) != ExpectedHash || LexToString(FMD5Hash::HashFile(*Target)) != ExpectedHash)
        { Error = TEXT("Shared avatar support changed while copying. Save the project and package again."); return false; }
    }
    if (PreviousFiles) for (const auto& Pair : (*PreviousFiles)->Values)
    {
        const FString Relative(*Pair.Key);
        if (DesiredFiles->HasField(Relative)) continue;
        const FString Target = Content / Relative;
        if (!ValidRelative(Relative) || !NoRedirects(Target, OwnedRoot, Error)) { if (Error.IsEmpty()) Error = TEXT("Unsafe old support snapshot path."); return false; }
        if (PF.FileExists(*Target) && !PF.DeleteFile(*Target)) { Error = TEXT("Could not remove obsolete shared support: ") + Relative; return false; }
    }
    Manifest->SetObjectField(TEXT("files"), DesiredFiles);
    Manifest->SetBoolField(TEXT("complete"), true);
    if (!Write(ManifestPath, Manifest)) { Error = TEXT("Could not finish the shared avatar support snapshot."); return false; }
    return true;
}
}
