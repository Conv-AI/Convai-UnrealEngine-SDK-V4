// Copyright Convai Inc. All Rights Reserved.
#include "Packaging/ConvaiAvatarPackaging.h"
#include "Packaging/ConvaiAvatarBaseContent.h"
#include "Packaging/ConvaiAvatarProcess.h"
#include "Config/ConvaiAvatarProjectConfiguration.h"
#include "Workspace/ConvaiAvatarUploaderWorkspace.h"
#include "Services/ConvaiAvatarDependencies.h"
#include "Async/Async.h"
#include "Algo/AllOf.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Misc/CoreMisc.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "PluginDescriptor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace
{
FString FullPath(FString Path)
{
    Path = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeDirectoryName(Path);
    FPaths::CollapseRelativeDirectories(Path);
    return Path;
}
bool IsBelow(const FString& Path, const FString& Root)
{
    return FullPath(Path).StartsWith(FullPath(Root) + TEXT("/"), ESearchCase::IgnoreCase);
}
bool IsProcessPath(const FString& Path)
{
    if (Path.IsEmpty()) return false;
    for (TCHAR C : Path) if (C == TEXT('"') || C < 32) return false;
    return true;
}
bool IsRedirected(const FString& Path)
{
    const DWORD Attributes = GetFileAttributesW(*Path);
    return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
bool LocalDirectory(const FString& Directory, FString& Error)
{
    const FString Root = FullPath(FConvaiAvatarWorkspace::GetProxyDirectory());
    const FString Target = FullPath(Directory);
    if (Target != Root && !IsBelow(Target, Root)) { Error = TEXT("Packaging tried to write outside its uploader cache."); return false; }
    TArray<FString> Parts;
    Target.Mid(Root.Len()).ParseIntoArray(Parts, TEXT("/"), true);
    FString Current = Root;
    if (IsRedirected(Current)) { Error = TEXT("The uploader cache cannot be a redirected directory."); return false; }
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    if (!PF.DirectoryExists(*Current) && !PF.CreateDirectoryTree(*Current)) { Error = TEXT("Could not create the uploader cache."); return false; }
    for (const FString& Part : Parts)
    {
        Current /= Part;
        if (IsRedirected(Current)) { Error = TEXT("A packaging directory is redirected: ") + Current; return false; }
        if (!PF.DirectoryExists(*Current) && !PF.CreateDirectory(*Current)) { Error = TEXT("Could not create packaging directory: ") + Current; return false; }
    }
    return true;
}
bool ReadPackagingJson(const FString& Path, TSharedPtr<FJsonObject>& Json)
{
    FString Text;
    return FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid();
}
bool SaveJson(const TSharedRef<FJsonObject>& Json, const FString& Path)
{
    if (IsRedirected(Path)) return false;
    FString Text;
    return FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text)) && FFileHelper::SaveStringToFile(Text, *Path);
}
bool Sha256(const TArray<uint8>& Bytes, FString& Hash)
{
    HCRYPTPROV Provider = 0; HCRYPTHASH Context = 0;
    if (!CryptAcquireContextW(&Provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    uint8 Digest[32]{}; DWORD Size = UE_ARRAY_COUNT(Digest);
    const bool Success = CryptCreateHash(Provider, CALG_SHA_256, 0, 0, &Context) &&
        CryptHashData(Context, Bytes.GetData(), Bytes.Num(), 0) && CryptGetHashParam(Context, HP_HASHVAL, Digest, &Size, 0) && Size == 32;
    if (Context) CryptDestroyHash(Context);
    CryptReleaseContext(Provider, 0);
    if (Success) Hash = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
    return Success;
}
bool ReadSnapshot(const FString& Root, const FString& Path, const FString& ExpectedHash, FString& Json, FString& Error)
{
    if (!IsBelow(Path, Root) || FPaths::IsRelative(Path) || ExpectedHash.Len() != 64 ||
        !Algo::AllOf(ExpectedHash, [](TCHAR C) { return FChar::IsHexDigit(C); }))
    { Error = TEXT("A captured uploader configuration has an invalid path or checksum. Check for updates again."); return false; }
    for (FString Current = FullPath(Path); !Current.IsEmpty();)
    {
        if (IsRedirected(Current)) { Error = TEXT("A captured uploader configuration points elsewhere. Restore a local uploader folder and retry."); return false; }
        const FString Parent = FPaths::GetPath(Current); if (Parent == Current) break; Current = Parent;
    }
    TArray<uint8> Bytes; FString ActualHash;
    if (IFileManager::Get().FileSize(*Path) < 1 || IFileManager::Get().FileSize(*Path) > 1024 * 1024 ||
        !FFileHelper::LoadFileToArray(Bytes, *Path) || !Sha256(Bytes, ActualHash) || !ActualHash.Equals(ExpectedHash, ESearchCase::IgnoreCase))
    { Error = TEXT("A captured uploader configuration is missing or has changed. Check for updates again before preparing the avatar."); return false; }
    FFileHelper::BufferToString(Json, Bytes.GetData(), Bytes.Num());
    return true;
}
bool ReadPluginNames(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, TArray<FString>& Names)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object->TryGetArrayField(Key, Values) || Values->Num() > 64) return false;
    for (const auto& Value : *Values)
    {
        FString Name;
        if (!Value || !Value->TryGetString(Name) || Name.IsEmpty() || Name.Len() > 128 ||
            !Algo::AllOf(Name, [](TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('_'); }) || Names.Contains(Name)) return false;
        Names.Add(Name);
    }
    return true;
}
bool WalkLocalFiles(const FString& Root, TArray<FString>& Files, FString& Error, const std::atomic<bool>* Cancelled = nullptr)
{
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    TArray<FString> Queue {FullPath(Root)};
    for (int32 Index = 0; Index < Queue.Num(); ++Index)
    {
        if (Cancelled && Cancelled->load()) { Error = TEXT("Packaging cancelled."); return false; }
        if (IsRedirected(Queue[Index])) { Error = TEXT("A plugin dependency contains a redirected folder: ") + Queue[Index]; return false; }
        bool bSafe = true;
        const bool bVisited = PF.IterateDirectory(*Queue[Index], [&](const TCHAR* Path, bool bDirectory)
        {
            if (IsRedirected(Path)) { Error = FString(TEXT("A plugin dependency contains a junction or symlink: ")) + Path; bSafe = false; return false; }
            if (bDirectory) Queue.Add(Path); else Files.Add(Path);
            return true;
        });
        if (!bVisited || !bSafe) { if (Error.IsEmpty()) Error = TEXT("Could not read plugin directory: ") + Queue[Index]; return false; }
    }
    return true;
}
bool RelativeFile(const FString& File, const FString& Root, FString& Relative)
{
    if (!IsBelow(File, Root)) return false;
    Relative = FullPath(File).Mid(FullPath(Root).Len() + 1);
    return !Relative.IsEmpty() && !Relative.Contains(TEXT("..")) && !Relative.Contains(TEXT(":"));
}
bool CopyRuntimePlugin(const FString& Source, const FString& Dest, const std::atomic<bool>& Cancelled, FString& Error)
{
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    if (!LocalDirectory(Dest, Error)) return false;
    // Only deployment directories are traversed. Checkout/build caches are not copied.
    TArray<FString> Files;
    TArray<FString> Descriptors;
    IFileManager::Get().FindFiles(Descriptors, *(FullPath(Source) / TEXT("*.uplugin")), true, false);
    if (Descriptors.Num() != 1) { Error = TEXT("A dependency must contain exactly one plugin descriptor: ") + Source; return false; }
    Files.Add(FullPath(Source) / Descriptors[0]);
    const TCHAR* Directories[] = {TEXT("Binaries"), TEXT("Content"), TEXT("Source"), TEXT("Config"), TEXT("Resources"), TEXT("Shaders")};
    for (const TCHAR* Directory : Directories)
    {
        const FString Child = FullPath(Source) / Directory;
        if (PF.DirectoryExists(*Child) && !WalkLocalFiles(Child, Files, Error, &Cancelled)) return false;
    }
    const FString ManifestPath = Dest / TEXT(".convai-plugin-snapshot.json");
    TSharedPtr<FJsonObject> Previous;
    ReadPackagingJson(ManifestPath, Previous);
    TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
    for (const FString& File : Files)
    {
        if (Cancelled.load()) { Error = TEXT("Packaging cancelled."); return false; }
        FString Relative;
        if (!RelativeFile(File, Source, Relative) || IsRedirected(File)) { Error = TEXT("Unsafe plugin snapshot path."); return false; }
        const FString Target = Dest / Relative;
        if (!LocalDirectory(FPaths::GetPath(Target), Error) || IsRedirected(Target)) { if (Error.IsEmpty()) Error = TEXT("A cached plugin file is redirected."); return false; }
        const FString Hash = LexToString(FMD5Hash::HashFile(*File));
        FString OldHash;
        const bool bUnchanged = Previous && Previous->TryGetStringField(Relative, OldHash) && OldHash == Hash &&
            PF.FileExists(*Target) && PF.FileSize(*Target) == PF.FileSize(*File) && LexToString(FMD5Hash::HashFile(*Target)) == Hash;
        if (!bUnchanged && !PF.CopyFile(*Target, *File)) { Error = TEXT("Could not copy an uploader plugin dependency: ") + Relative; return false; }
        Manifest->SetStringField(Relative, Hash);
    }
    if (Previous)
    {
        for (const auto& Pair : Previous->Values)
        {
            if (Manifest->HasField(Pair.Key)) continue;
            const FString Key(*Pair.Key);
            const FString Target = Dest / Key;
            FString Relative;
            if (!RelativeFile(Target, Dest, Relative) || Relative != Key || !LocalDirectory(FPaths::GetPath(Target), Error) || IsRedirected(Target))
            { if (Error.IsEmpty()) Error = TEXT("Unsafe obsolete plugin snapshot path."); return false; }
            // Only a previously recorded, now-obsolete owned file. Never a recursive deletion.
            if (PF.FileExists(*Target) && !PF.DeleteFile(*Target)) { Error = TEXT("Could not remove obsolete cached plugin file: ") + Relative; return false; }
        }
    }
    if (!SaveJson(Manifest, ManifestPath)) { Error = TEXT("Could not save plugin snapshot record."); return false; }
    return true;
}
bool CheckEditorBinaries(const TSharedRef<IPlugin>& Plugin, const FString& EngineBuildId, FString& Error)
{
    TArray<FString> Needed;
    for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
        if (Module.IsCompiledInCurrentConfiguration()) Needed.Add(Module.Name.ToString());
    if (Needed.IsEmpty()) return true;
    TSharedPtr<FJsonObject> Manifest;
    const FString Binaries = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries/Win64"));
    FString BuildId;
    const TSharedPtr<FJsonObject>* Modules = nullptr;
    if (!ReadPackagingJson(Binaries / TEXT("UnrealEditor.modules"), Manifest) || !Manifest->TryGetStringField(TEXT("BuildId"), BuildId) ||
        BuildId != EngineBuildId || !Manifest->TryGetObjectField(TEXT("Modules"), Modules))
    { Error = TEXT("Build the host project against this Unreal installation before packaging. Editor binaries for ") + Plugin->GetName() + TEXT(" are missing or have a different build ID."); return false; }
    for (const FString& Name : Needed)
    {
        FString Binary;
        if (!(*Modules)->TryGetStringField(Name, Binary) || !IsBelow(Binaries / Binary, Binaries) || !FPaths::FileExists(Binaries / Binary))
        { Error = TEXT("Required editor module binary is missing: ") + Name + TEXT(". Build the host project before packaging."); return false; }
    }
    return true;
}
}

namespace ConvaiAvatarPackagingPrivate
{
bool ResolveUtilityPlugins(const FConvaiAvatarPackagingPolicy& Policy, bool bMetaHuman,
    const TMap<FString, FPluginDescriptor>& EnginePlugins, TSet<FString>& Names, FString& Error)
{
    Names.Reset(); Error.Reset(); TArray<FString> Queue;
    if (Policy.RequiredPlugins.Contains(TEXT("JsonBlueprintUtilities"))) Queue.Add(TEXT("JsonBlueprintUtilities"));
    if (bMetaHuman) Queue.Append(Policy.MetaHumanPlugins);
    for (int32 Index = 0; Index < Queue.Num(); ++Index)
    {
        const FString Name = Queue[Index];
        const FPluginDescriptor* Descriptor = EnginePlugins.Find(Name);
        if (!Descriptor) { Error = TEXT("Install the required Unreal Engine plugin before preparing this avatar: ") + Name; return false; }
        Names.Add(Name);
        for (const FPluginReferenceDescriptor& Dependency : Descriptor->Plugins)
            if (Dependency.bEnabled && (!Dependency.bOptional || EnginePlugins.Contains(Dependency.Name))) Queue.AddUnique(Dependency.Name);
    }
    return true;
}
}

FConvaiAvatarPackagingCapabilities FConvaiAvatarPackaging::GetCapabilities()
{
    check(IsInGameThread());
    FConvaiAvatarPackagingCapabilities Result;
    const auto Convai = IPluginManager::Get().FindPlugin(TEXT("ConvAI"));
    if (!Convai || !Convai->IsEnabled())
    {
        Result.bSourceAvailable = false;
        Result.SourceReason = Result.WindowsReason = Result.LinuxReason = TEXT("Install and enable Convai to prepare avatar uploads.");
        return Result;
    }
    const bool bTools = FPaths::FileExists(FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealEditor-Cmd.exe")) &&
        FPaths::FileExists(FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealPak.exe"));
    if (!bTools)
    {
        Result.WindowsReason = Result.LinuxReason = TEXT("This Unreal installation is missing its editor cook or UnrealPak tool. Repair the engine installation to package avatars.");
        return Result;
    }
    ITargetPlatformManagerModule& Manager = GetTargetPlatformManagerRef();
    Result.bWindowsAvailable = Manager.FindTargetPlatform(TEXT("Windows")) != nullptr;
    if (!Result.bWindowsAvailable) Result.WindowsReason = TEXT("Windows cooking support is not installed for this Unreal Engine.");
    // These are the Linux runtime inputs named by Convai.Build.cs. A cook target
    // alone must not advertise a platform whose distributed native SDK is absent.
    const FString LinuxLibraries = Convai->GetBaseDir() / TEXT("Source/ThirdParty/ConvaiWebRTC/lib/release/linux");
    if (!FPaths::FileExists(LinuxLibraries / TEXT("libconvai_client.so")) || !FPaths::FileExists(LinuxLibraries / TEXT("libconvai_http_helper.so")))
    {
        Result.LinuxReason = TEXT("This Convai installation does not include the Linux native libraries. Install a Convai release with Linux support before selecting Linux.");
        return Result;
    }
    ITargetPlatform* Linux = Manager.FindTargetPlatform(TEXT("Linux"));
    if (!Linux)
    {
        Result.LinuxReason = TEXT("Linux cooking support is not installed for this Unreal Engine. Add the Linux target platform in the engine installation options.");
        return Result;
    }
    FString Documentation;
    if (!Linux->IsSdkInstalled(true, Documentation))
    {
        Result.LinuxReason = TEXT("The Linux cross-compilation toolchain is unavailable. Install the toolchain matching this Unreal Engine and restart the editor.");
        return Result;
    }
    // UE's Linux SDK probe accepts any existing MULTIARCH root. Require an
    // actual x86-64 compiler as well, rather than accepting an empty directory.
    const FString Multiarch = FPlatformMisc::GetEnvironmentVariable(TEXT("LINUX_MULTIARCH_ROOT"));
    const FString Legacy = FPlatformMisc::GetEnvironmentVariable(TEXT("LINUX_ROOT"));
    if (!FPaths::FileExists(Multiarch / TEXT("x86_64-unknown-linux-gnu/bin/clang++.exe")) &&
        !FPaths::FileExists(Legacy / TEXT("bin/clang++.exe")))
    {
        Result.LinuxReason = TEXT("The configured Linux toolchain has no x86-64 compiler. Repair the toolchain for this Unreal Engine and restart the editor.");
        return Result;
    }
    TArray<FName> Formats;
    Linux->GetAllTargetedShaderFormats(Formats);
    if (Formats.IsEmpty())
    {
        Result.LinuxReason = TEXT("No Linux shader format is configured for cooking in this project.");
        return Result;
    }
    for (FName Format : Formats)
    {
        if (!Manager.FindShaderFormat(Format))
        {
            Result.LinuxReason = TEXT("The engine is missing the Linux shader compiler for ") + Format.ToString() + TEXT(". Repair Linux platform support before packaging.");
            return Result;
        }
    }
    Result.bLinuxAvailable = true;
    return Result;
}

bool FConvaiAvatarPackaging::ValidateOptions(const FConvaiAvatarPackageOptions& Options, const FConvaiAvatarPackagingCapabilities& Capabilities, FString& OutError)
{
    OutError.Reset();
    if (!Options.bIncludeSource && !Options.bIncludeWindows && !Options.bIncludeLinux)
        OutError = TEXT("Select at least one artifact to upload: Source, Windows, or Linux.");
    else if (Options.bIncludeSource && !Capabilities.bSourceAvailable) OutError = Capabilities.SourceReason;
    else if (Options.bIncludeWindows && !Capabilities.bWindowsAvailable) OutError = Capabilities.WindowsReason;
    else if (Options.bIncludeLinux && !Capabilities.bLinuxAvailable) OutError = Capabilities.LinuxReason;
    else return true;
    if (OutError.IsEmpty()) OutError = TEXT("A selected upload artifact is unavailable on this installation.");
    return false;
}

bool FConvaiAvatarPackaging::ReadConfigurationSnapshot(const FString& UploaderRoot, const FString& ResolutionFile,
    FConvaiAvatarPackagingPolicy& Policy, FString& Error)
{
    Policy = {}; Error.Reset();
    TSharedPtr<FJsonObject> Resolution;
    if (!IsBelow(ResolutionFile, UploaderRoot / TEXT("Transport")) || IsRedirected(ResolutionFile) ||
        IFileManager::Get().FileSize(*ResolutionFile) > 64 * 1024 || !ReadPackagingJson(ResolutionFile, Resolution))
    { Error = TEXT("The uploader configuration snapshot could not be read. Check for updates again."); return false; }
    FString ModdingText, PolicyText;
    if (!Resolution->TryGetStringField(TEXT("project_profile_path"), Policy.ProfileFile) ||
        !Resolution->TryGetStringField(TEXT("project_profile_sha256"), Policy.ProfileSha256) ||
        !Resolution->TryGetStringField(TEXT("asset_uploader_policy_path"), Policy.UploadPolicyFile) ||
        !Resolution->TryGetStringField(TEXT("asset_uploader_policy_sha256"), Policy.UploadPolicySha256) ||
        !Resolution->TryGetStringField(TEXT("modding_config_path"), Policy.ModdingConfigFile) ||
        !Resolution->TryGetStringField(TEXT("modding_config_sha256"), Policy.ModdingConfigSha256))
    { Error = TEXT("The update check did not include the project profile and upload settings. Update the published uploader configuration and retry."); return false; }
    const FString TransportRoot = UploaderRoot / TEXT("Transport");
    if (!ReadSnapshot(TransportRoot, Policy.ProfileFile, Policy.ProfileSha256, Policy.ProfileJson, Error) ||
        !ReadSnapshot(TransportRoot, Policy.UploadPolicyFile, Policy.UploadPolicySha256, PolicyText, Error) ||
        !ReadSnapshot(TransportRoot, Policy.ModdingConfigFile, Policy.ModdingConfigSha256, ModdingText, Error) ||
        !FConvaiAvatarProjectConfiguration::ValidateProfile(Policy.ProfileJson, Error)) return false;
    TSharedPtr<FJsonObject> Modding, Upload;
    const TSharedPtr<FJsonObject>* Engine = nullptr;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PolicyText), Upload) || !Upload ||
        !Upload->TryGetObjectField(TEXT("unreal-engine"), Engine))
    { Error = TEXT("The captured upload policy is missing its Unreal platform settings."); return false; }
    for (const FString& Platform : {FString(TEXT("Windows")), FString(TEXT("Linux"))})
    {
        const TSharedPtr<FJsonObject>* PlatformPolicy = nullptr; FString Configuration; bool bEnabled = false;
        if (!(*Engine)->TryGetObjectField(Platform.ToLower(), PlatformPolicy)) continue;
        if (!(*PlatformPolicy)->TryGetBoolField(TEXT("should-package"), bEnabled) ||
            !(*PlatformPolicy)->TryGetStringField(TEXT("configuration"), Configuration) ||
            !FConvaiAvatarProjectConfiguration::IsSupportedConfiguration(Configuration))
        { Error = TEXT("The published ") + Platform + TEXT(" build configuration is missing or unsupported."); return false; }
        Policy.PlatformConfigurations.Add(Platform, Configuration);
    }
    if (!FConvaiAvatarDioramaLimits::Parse(PolicyText, Policy.DioramaLimits, Error)) return false;
    Policy.bHasDioramaLimits = !Policy.DioramaLimits.Rules.IsEmpty();
    const TSharedPtr<FJsonObject>* ProjectSettings = nullptr;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ModdingText), Modding) || !Modding ||
        !Modding->TryGetObjectField(TEXT("project_settings"), ProjectSettings) ||
        !ReadPluginNames(*ProjectSettings, TEXT("required_plugins"), Policy.RequiredPlugins) ||
        !ReadPluginNames(*ProjectSettings, TEXT("metahuman_plugins"), Policy.MetaHumanPlugins))
    { Error = TEXT("The published uploader plugin requirements are invalid."); return false; }
    // V1 infrastructure replaces the old PakManager; none of these utility entries are avatar runtime dependencies.
    for (const FString& Name : Policy.RequiredPlugins)
        if (Name != TEXT("ConvAI") && Name != TEXT("ConvaiHTTP") && Name != TEXT("ConvaiPakManager") && Name != TEXT("JsonBlueprintUtilities"))
        { Error = TEXT("The published uploader requires an infrastructure plugin that this version cannot install automatically: ") + Name; return false; }
    const TSharedPtr<FJsonObject>* Cross = nullptr; const TSharedPtr<FJsonObject>* Versions = nullptr; const TSharedPtr<FJsonObject>* Urls = nullptr;
    const FString EngineKey = FString::Printf(TEXT("%d_%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
    if (Modding->TryGetObjectField(TEXT("cross_compilation"), Cross) && (*Cross)->TryGetObjectField(TEXT("toolchain_versions"), Versions) &&
        (*Cross)->TryGetObjectField(TEXT("toolchain_download_urls"), Urls) && (*Versions)->TryGetStringField(EngineKey, Policy.LinuxToolchainVersion))
    {
        if (Policy.LinuxToolchainVersion.Len() > 128 || !(*Urls)->TryGetStringField(Policy.LinuxToolchainVersion, Policy.LinuxToolchainUrl) ||
            !Policy.LinuxToolchainUrl.StartsWith(TEXT("https://cdn.unrealengine.com/CrossToolchain_Linux/")) ||
            Policy.LinuxToolchainUrl.Contains(TEXT("..")) || Policy.LinuxToolchainUrl.Contains(TEXT("?")) || Policy.LinuxToolchainUrl.Len() > 512)
        { Error = TEXT("The published Linux toolchain reference is invalid. No toolchain was downloaded or installed."); return false; }
    }
    const TSharedPtr<FJsonObject>* Drive = nullptr;
    if (Modding->TryGetObjectField(TEXT("google_drive"), Drive) && (*Drive)->TryGetStringField(TEXT("convai_reallusion_content"), Policy.ReallusionDriveId))
        if (Policy.ReallusionDriveId.Len() > 200 || !Algo::AllOf(Policy.ReallusionDriveId, [](TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-'); }))
        { Error = TEXT("The published Reallusion content reference is invalid."); return false; }
    return true;
}

FString FConvaiAvatarPackaging::BuildCookArguments(const FString& Project, const FString& ContentDir, const FString& Log, const FString& Platform, const TArray<FString>& Maps)
{
    if (!IsProcessPath(Project) || !IsProcessPath(ContentDir) || !IsProcessPath(Log) || (Platform != TEXT("Windows") && Platform != TEXT("Linux"))) return {};
    const FString Output = GetCookOutputDirectory(Log);
    FString Arguments = FString::Printf(TEXT("\"%s\" -run=Cook -TargetPlatform=%s -CookDir=\"%s\" -OutputDir=\"%s\" -NoDefaultMaps -NoAlwaysCookMaps -unattended -RenderOffscreen -nosplash -nop4 -UTF8Output -stdout -abslog=\"%s\""), *Project, *Platform, *ContentDir, *Output, *Log);
    for (const FString& Map : Maps)
    {
        if (!IsProcessPath(Map) || !FPackageName::IsValidTextForLongPackageName(Map) || Map.Contains(TEXT("+")) ||
            Map.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1) == INDEX_NONE) return {};
        Arguments += FString::Printf(TEXT(" -Map=\"%s\""), *Map);
    }
    return Arguments;
}

FString FConvaiAvatarPackaging::GetCookOutputDirectory(const FString& Log)
{
    return FullPath(FPaths::GetPath(Log) / TEXT("Cooked/[Platform]"));
}
FString FConvaiAvatarPackaging::GetRuntimeProjectName()
{
    return TEXT("AvatarStudioUploader");
}
FString FConvaiAvatarPackaging::GetCookedAvatarDirectory(const FString& CookRoot, const FString& PluginName)
{
    if (!FConvaiAvatarWorkspace::IsSafePluginName(PluginName)) return {};
    const FString PhysicalName = FPaths::GetBaseFilename(ConvaiAvatarUploaderWorkspace::ProjectPath(FConvaiAvatarWorkspace::GetProxyDirectory()));
    return FullPath(CookRoot / PhysicalName / TEXT("Plugins/ConvaiAvatars") / PluginName / TEXT("Content"));
}
bool FConvaiAvatarPackaging::MakePakResponseLine(const FString& CookRoot, const FString& PluginName, const FString& File, FString& OutLine)
{
    OutLine.Reset();
    FString Relative;
    const FString ContentRoot = GetCookedAvatarDirectory(CookRoot, PluginName);
    if (ContentRoot.IsEmpty() || !IsProcessPath(File) || !RelativeFile(File, ContentRoot, Relative)) return false;
    const FString VirtualFile = GetRuntimeProjectName() / TEXT("Plugins/ConvaiAvatars") / PluginName / TEXT("Content") / Relative;
    OutLine = FString::Printf(TEXT("\"%s\" \"../../../%s\" -compress\n"), *FullPath(File), *VirtualFile);
    return true;
}

bool FConvaiAvatarPackaging::CalculateInputFingerprint(const FConvaiAvatarPreparedAsset& Asset, FString& OutFingerprint, FString& OutError)
{
    OutError.Reset();
    OutFingerprint.Reset();
    const FString Expected = FullPath(FConvaiAvatarWorkspace::GetAvatarsDirectory() / Asset.PluginName);
    if (!FConvaiAvatarWorkspace::IsSafePluginName(Asset.PluginName) || !FullPath(Asset.PluginDirectory).Equals(Expected, ESearchCase::IgnoreCase) || IsRedirected(Expected))
    { OutError = TEXT("The avatar fingerprint must read its canonical local plugin."); return false; }
    TArray<FString> Files;
    if (!WalkLocalFiles(Expected / TEXT("Content"), Files, OutError)) return false;
    Files.Add(Expected / (Asset.PluginName + TEXT(".uplugin")));
    Files.Sort();
    FMD5 Combined;
    TArray<uint8> Scratch;
    Scratch.SetNumUninitialized(64 * 1024);
    for (const FString& File : Files)
    {
        if (IsRedirected(File)) { OutError = TEXT("An avatar source file is redirected."); return false; }
        FString Relative;
        if (!RelativeFile(File, Expected, Relative)) { OutError = TEXT("An avatar fingerprint input escapes the plugin."); return false; }
        const int64 BeforeSize = IFileManager::Get().FileSize(*File);
        const FDateTime BeforeTime = IFileManager::Get().GetTimeStamp(*File);
        const FMD5Hash Hash = FMD5Hash::HashFile(*File, &Scratch);
        if (BeforeSize < 0 || !Hash.IsValid() || IFileManager::Get().FileSize(*File) != BeforeSize || IFileManager::Get().GetTimeStamp(*File) != BeforeTime)
        { OutError = TEXT("An avatar source file changed while checking its revision. Save the avatar and try again."); return false; }
        const FString Record = Relative + TEXT("\n") + LexToString(BeforeSize) + TEXT("\n") + LexToString(Hash) + TEXT("\n");
        FTCHARToUTF8 Bytes(*Record);
        Combined.Update(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
    }
    // A save can add/remove a file after the first directory walk. Catch that before publishing the fingerprint.
    TArray<FString> AfterFiles;
    if (!WalkLocalFiles(Expected / TEXT("Content"), AfterFiles, OutError)) return false;
    AfterFiles.Add(Expected / (Asset.PluginName + TEXT(".uplugin")));
    AfterFiles.Sort();
    if (AfterFiles != Files) { OutError = TEXT("The avatar file set changed while checking its revision. Save the avatar and try again."); return false; }
    FMD5Hash Fingerprint;
    Fingerprint.Set(Combined);
    OutFingerprint = LexToString(Fingerprint);
    return true;
}

bool FConvaiAvatarPackaging::ValidatePackagedInputs(const FConvaiAvatarPreparedAsset& Asset, const FString& ExpectedFingerprint, FString& OutError)
{
    OutError.Reset();
    FString Current;
    if (ExpectedFingerprint.IsEmpty() || !CalculateInputFingerprint(Asset, Current, OutError))
    { if (OutError.IsEmpty()) OutError = TEXT("The prepared avatar upload has no valid source revision fingerprint."); return false; }
    if (Current != ExpectedFingerprint)
    { OutError = TEXT("The avatar changed while preparing this upload. Save your changes and prepare the upload again so all selected artifacts use the same revision."); return false; }
    return true;
}

bool FConvaiAvatarPackaging::PrepareProxy(const FConvaiAvatarPreparedAsset& Asset, const FString& ConvaiDirectory,
    const TMap<FString,FString>& PluginDirectories, const TMap<FString,FString>& BaseContentFiles,
    const FConvaiAvatarPackagingPolicy& Policy, const TMap<FString, FPluginDescriptor>& EnginePlugins, FString& Project, FString& Error)
{
    if (!FConvaiAvatarWorkspace::EnsureProxyLink(Asset, Error)) return false;
    Project = ConvaiAvatarUploaderWorkspace::ProjectPath(Asset.ProxyDirectory);
    const FString Marker = Asset.ProxyDirectory / TEXT(".convai-workspace.json");
    if (!LocalDirectory(Asset.ProxyDirectory / TEXT("Config"), Error) || !LocalDirectory(Asset.ProxyDirectory / TEXT("Content"), Error)) return false;
    if (!CopyRuntimePlugin(ConvaiDirectory, Asset.ProxyDirectory / TEXT("Plugins/Convai"), Cancelled, Error)) return false;
    if (!ConvaiAvatarBaseContent::Snapshot(FPaths::ProjectContentDir(), Asset.ProxyDirectory, Asset.PluginName, BaseContentFiles, Cancelled, Error)) return false;
    auto Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("FileVersion"), 3);
    Root->SetStringField(TEXT("EngineAssociation"), FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
    Root->SetStringField(TEXT("Description"), TEXT("Managed Cloud Avatars uploader workspace."));
    Root->SetBoolField(TEXT("DisableEnginePluginsByDefault"), true);
    TArray<TSharedPtr<FJsonValue>> Plugins;
    TSet<FString> Names;
    Names.Add(TEXT("ConvAI")); Names.Add(Asset.PluginName);
    // Transport is editor infrastructure, never an avatar dependency. Preserve
    // it after download-first bootstrap, without building it for Source-only.
    if (FPaths::FileExists(Asset.ProxyDirectory / TEXT("Plugins/ConvaiHTTP/ConvaiHTTP.uplugin"))) Names.Add(TEXT("ConvaiHTTP"));
    TSet<FString> UtilityNames;
    if (!ConvaiAvatarPackagingPrivate::ResolveUtilityPlugins(Policy, Asset.bIsMetaHuman, EnginePlugins, UtilityNames, Error)) return false;
    Names.Append(UtilityNames);
    for (const auto& Pair : PluginDirectories) Names.Add(Pair.Key);
    for (const FString& Name : Names)
    {
        const FString* Directory = PluginDirectories.Find(Name);
        if (Directory && !Directory->IsEmpty() && Name != TEXT("ConvAI") && Name != Asset.PluginName)
        {
            if (!CopyRuntimePlugin(*Directory, Asset.ProxyDirectory / TEXT("Plugins") / Name, Cancelled, Error)) return false;
        }
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("Name"), Name); Item->SetBoolField(TEXT("Enabled"), true);
        Plugins.Add(MakeShared<FJsonValueObject>(Item));
    }
    // One-level descriptor reads are deliberate; never recurse through avatar junctions.
    // Historical uploads can have arbitrary plugin names, not just the current generated prefix.
    TArray<FString> AvatarFolders;
    const FString AvatarRoot = Asset.ProxyDirectory / TEXT("Plugins/ConvaiAvatars");
    IFileManager::Get().FindFiles(AvatarFolders, *(AvatarRoot / TEXT("*")), false, true);
    for (const FString& Folder : AvatarFolders)
    {
        TArray<FString> Descriptors;
        IFileManager::Get().FindFiles(Descriptors, *(AvatarRoot / Folder / TEXT("*.uplugin")), true, false);
        for (const FString& Descriptor : Descriptors)
        {
            const FString Name = FPaths::GetBaseFilename(Descriptor);
            if (Name == Asset.PluginName) continue;
            if (Names.Contains(Name)) { Error = TEXT("The selected avatar depends on another cached avatar plugin: ") + Name; return false; }
            auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("Name"), Name); Item->SetBoolField(TEXT("Enabled"), false);
            Plugins.Add(MakeShared<FJsonValueObject>(Item));
        }
    }
    // A dependency used by an older cook must not become enabled implicitly in this one.
    TArray<FString> CachedFolders;
    IFileManager::Get().FindFiles(CachedFolders, *(Asset.ProxyDirectory / TEXT("Plugins/*")), false, true);
    for (const FString& Folder : CachedFolders)
    {
        if (Folder == TEXT("ConvaiAvatars")) continue;
        const FString Directory = Asset.ProxyDirectory / TEXT("Plugins") / Folder;
        if (IsRedirected(Directory)) { Error = TEXT("A runtime plugin cache folder is redirected."); return false; }
        TArray<FString> Descriptors;
        IFileManager::Get().FindFiles(Descriptors, *(Directory / TEXT("*.uplugin")), true, false);
        for (const FString& Descriptor : Descriptors)
        {
            const FString Name = FPaths::GetBaseFilename(Descriptor);
            if (Names.Contains(Name)) continue;
            auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("Name"), Name); Item->SetBoolField(TEXT("Enabled"), false);
            Plugins.Add(MakeShared<FJsonValueObject>(Item));
        }
    }
    Root->SetArrayField(TEXT("Plugins"), Plugins);
    // Claim the already validated generated workspace before creating its descriptor.
    // A later descriptor/config failure remains retryable instead of appearing unmanaged.
    TSharedPtr<FJsonObject> Metadata;
    if (!ReadPackagingJson(Marker, Metadata)) { Error = TEXT("Could not read uploader workspace ownership."); return false; }
    Metadata->SetStringField(TEXT("selected_plugin"), Asset.PluginName);
    if (!SaveJson(Metadata.ToSharedRef(), Marker)) { Error = TEXT("Could not save uploader workspace ownership."); return false; }
    Project = ConvaiAvatarUploaderWorkspace::ProjectPath(Asset.ProxyDirectory);
    if (!SaveJson(Root, Project)) { Error = TEXT("Could not write the uploader project."); return false; }
    // Use the same reviewed renderer/RHI/packaging values as the configuration preview.
    // Patch owned keys only so cached plugin/data sections survive repeated prepares.
    const FString Configuration = Policy.PlatformConfigurations.FindRef(TEXT("Windows"));
    if (!FConvaiAvatarProjectConfiguration::ApplyProxyProfile(Asset.ProxyDirectory, Error, Asset.PluginName, Policy.ProfileJson, Configuration)) return false;
    // The existing completed-manifest fingerprint binds portable settings to the same preparation as support content.
    const FString BaseManifestPath = Asset.ProxyDirectory / TEXT("ConvaiAvatarBaseContent.json");
    TSharedPtr<FJsonObject> BaseManifest;
    if (!ReadPackagingJson(BaseManifestPath, BaseManifest)) { Error = TEXT("Could not bind the project profile to this avatar's source snapshot."); return false; }
    BaseManifest->SetStringField(TEXT("project_profile_json"), Policy.ProfileJson);
    BaseManifest->SetStringField(TEXT("project_configuration"), Configuration);
    if (!SaveJson(BaseManifest.ToSharedRef(), BaseManifestPath)) { Error = TEXT("Could not save the avatar's captured project profile."); return false; }
    return true;
}

bool FConvaiAvatarPackaging::Run(const FString& Exe, const FString& Args, const FString& Log, FString& Error)
{
    if (Cancelled.load()) { Error = TEXT("Packaging cancelled."); return false; }
    if (!FPaths::FileExists(Exe) || Args.IsEmpty()) { Error = TEXT("The Unreal packaging executable or its arguments are unavailable."); return false; }
    void* Read = nullptr; void* Write = nullptr;
    if (!FPlatformProcess::CreatePipe(Read, Write)) { Error = TEXT("Could not create the packaging output pipe."); return false; }
    FConvaiAvatarProcess Child;
    if (!Child.Start(Exe, Args, Write, Error)) { FPlatformProcess::ClosePipe(Read, Write); return false; }
    FProcHandle& Proc = Child.GetHandle();
    while (FPlatformProcess::IsProcRunning(Proc))
    {
        const FString Chunk = FPlatformProcess::ReadPipe(Read);
        if (!Chunk.IsEmpty()) FFileHelper::SaveStringToFile(Chunk, *Log, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
        if (Cancelled.load()) { Child.Terminate(); break; }
        FPlatformProcess::Sleep(0.1f);
    }
    const FString Tail = FPlatformProcess::ReadPipe(Read);
    if (!Tail.IsEmpty()) FFileHelper::SaveStringToFile(Tail, *Log, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
    int32 ExitCode = -1; FPlatformProcess::GetProcReturnCode(Proc, &ExitCode);
    FPlatformProcess::ClosePipe(Read, Write);
    if (Cancelled.load()) { Error = TEXT("Packaging cancelled. Your prepared avatar is available for another attempt."); return false; }
    if (ExitCode != 0) { Error = FString::Printf(TEXT("Packaging exited with code %d. See %s"), ExitCode, *Log); return false; }
    return true;
}

void FConvaiAvatarPackaging::Start(const FConvaiAvatarPreparedAsset& Asset, FCompletion Completion, FStatus Status)
{
    Start(Asset, FConvaiAvatarPackageOptions(), MoveTemp(Completion), MoveTemp(Status));
}

void FConvaiAvatarPackaging::Start(const FConvaiAvatarPreparedAsset& Asset, const FConvaiAvatarPackageOptions& Options, FCompletion Completion, FStatus Status)
{
    check(IsInGameThread());
    if (Worker.IsValid() && !Worker.IsReady()) { Completion({}, TEXT("Another packaging operation is still running.")); return; }
    Cancelled.store(false);
    FString Error;
    if (!ValidateOptions(Options, GetCapabilities(), Error)) { Completion({}, Error); return; }
    const bool bCook = Options.bIncludeWindows || Options.bIncludeLinux;
    FConvaiAvatarPreparedAsset CheckedAsset = Asset;
    if (!FConvaiAvatarWorkspace::ValidatePreparedAsset(CheckedAsset, Error) || !FConvaiAvatarWorkspace::EnsureProxyLink(CheckedAsset, Error)) { Completion({}, Error); return; }
    if (CheckedAsset.Diorama.IsSet())
    {
        const FString Mount = TEXT("/") + CheckedAsset.PluginName + TEXT("/");
        if (!CheckedAsset.Diorama->Level.StartsWith(Mount, ESearchCase::CaseSensitive) ||
            !IsProcessPath(CheckedAsset.Diorama->Level) || !FPackageName::IsValidTextForLongPackageName(CheckedAsset.Diorama->Level) ||
            CheckedAsset.Diorama->Level.Contains(TEXT("+")))
        { Completion({}, TEXT("The diorama level must be a valid package inside the selected avatar plugin.")); return; }
    }
    const auto Convai = IPluginManager::Get().FindPlugin(TEXT("ConvAI"));
    if (!Convai) { Completion({}, TEXT("The installed Convai plugin could not be located.")); return; }
    TMap<FString,FString> BaseContentFiles;
    TArray<FString> BasePluginDependencies;
    if (!ConvaiAvatarBaseContent::Gather(CheckedAsset, BaseContentFiles, BasePluginDependencies, Cancelled, Error)) { Completion({}, Error); return; }
    TSharedPtr<FJsonObject> EngineManifest;
    FString EngineBuildId;
    if (bCook && (!ReadPackagingJson(FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealEditor.modules"), EngineManifest) || !EngineManifest->TryGetStringField(TEXT("BuildId"), EngineBuildId)))
    { Completion({}, TEXT("Could not read this Unreal installation's editor build ID.")); return; }
    TMap<FString,FString> Dependencies;
    TArray<FString> Queue = CheckedAsset.RequiredPlugins;
    Queue.Append(BasePluginDependencies);
    Queue.AddUnique(TEXT("ConvAI"));
    TSet<FString> Seen;
    for (int32 Index = 0; Index < Queue.Num(); ++Index)
    {
        const FString Name = Queue[Index];
        if (Seen.Contains(Name)) continue;
        Seen.Add(Name);
        const auto Plugin = IPluginManager::Get().FindPlugin(Name);
        if (!Plugin || !Plugin->IsEnabled()) { Completion({}, TEXT("Install and enable the required plugin before packaging: ") + Name); return; }
        const bool bProjectPlugin = Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project;
        if (bCook && bProjectPlugin && !CheckEditorBinaries(Plugin.ToSharedRef(), EngineBuildId, Error)) { Completion({}, Error); return; }
        Dependencies.Add(Name, bProjectPlugin ? Plugin->GetBaseDir() : FString());
        for (const FPluginReferenceDescriptor& Reference : Plugin->GetDescriptor().Plugins)
        {
            if (!Reference.bEnabled) continue;
            if (Reference.bOptional && !IPluginManager::Get().FindPlugin(Reference.Name).IsValid()) continue;
            Queue.AddUnique(Reference.Name);
        }
    }
    const FString ConvaiDirectory = Convai->GetBaseDir();
    TMap<FString, FPluginDescriptor> EnginePlugins;
    for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
        if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Engine) EnginePlugins.Add(Plugin->GetName(), Plugin->GetDescriptor());
    const FString Editor = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealEditor-Cmd.exe"));
    const FString UnrealPak = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealPak.exe"));
    auto Self = AsShared();
    Worker = Async(EAsyncExecution::Thread, [Self, Asset=MoveTemp(CheckedAsset), Options, ConvaiDirectory, Dependencies, EnginePlugins=MoveTemp(EnginePlugins), BaseContentFiles=MoveTemp(BaseContentFiles), Editor, UnrealPak, Completion=MoveTemp(Completion), Status=MoveTemp(Status)]() mutable
    {
        FConvaiAvatarPackageResult Result; FString Error;
        IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
        if (!LocalDirectory(Asset.ProxyDirectory, Error))
        {
            AsyncTask(ENamedThreads::GameThread, [Completion=MoveTemp(Completion), Result, Error]() mutable { Completion(Result, Error); });
            return;
        }
        FConvaiAvatarUploaderLease Lease;
        if (!Lease.Acquire(Asset.ProxyDirectory, Error)) {}
        auto Report = [&Status](FString Text) { AsyncTask(ENamedThreads::GameThread, [Status, Text]() { if (Status) Status(Text); }); };
        const FString Output = Asset.ProxyDirectory / TEXT("Artifacts") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
        if (Error.IsEmpty()) LocalDirectory(Output, Error);
        Result.ArtifactDirectory = Output;
        Result.LogPath = Output / TEXT("packaging.log");
        if (Error.IsEmpty() && !FFileHelper::SaveStringToFile(TEXT("Preparing the selected avatar upload artifacts.\n"), *Result.LogPath))
            Error = TEXT("Could not create the avatar preparation log.");
        if (Error.IsEmpty() && !ConvaiAvatarUploaderWorkspace::EnsureProject(Asset.ProxyDirectory, Error)) {}
        const FString ResolutionDirectory = Asset.ProxyDirectory / TEXT("Transport/Resolutions");
        if (Error.IsEmpty()) LocalDirectory(ResolutionDirectory, Error);
        Result.DependencyResolutionFile = ResolutionDirectory / (FPaths::GetCleanFilename(Output) + TEXT(".json"));
        if (Error.IsEmpty())
        {
            Report(TEXT("Checking uploader dependency updates"));
            if (!ConvaiAvatarDependencies::Resolve(Result.DependencyResolutionFile, Self->Cancelled, Error) && Error.IsEmpty())
                Error = TEXT("Could not resolve the uploader dependency version. Retry after checking your connection.");
        }
        if (Error.IsEmpty()) ReadConfigurationSnapshot(Asset.ProxyDirectory, Result.DependencyResolutionFile, Result.Configuration, Error);
        if (Error.IsEmpty() && Asset.Diorama.IsSet() && !Result.Configuration.bHasDioramaLimits)
            Error = TEXT("The published policy carries no diorama limits. Packaging is blocked.");
        if (Error.IsEmpty() && (Options.bIncludeWindows || Options.bIncludeLinux))
        {
            Report(TEXT("Preparing the uploader's resolved HTTP version"));
            if (!ConvaiAvatarDependencies::Prepare(Result.DependencyResolutionFile, Self->Cancelled, Error) && Error.IsEmpty())
                Error = TEXT("The uploader's resolved HTTP version could not be prepared. Retry after checking the build log.");
        }
        Report(TEXT("Preparing the uploader workspace"));
        if (Error.IsEmpty() && !Self->PrepareProxy(Asset, ConvaiDirectory, Dependencies, BaseContentFiles, Result.Configuration, EnginePlugins, Result.ProjectPath, Error)) {}
        if (Error.IsEmpty())
        {
            const FString Details = FString::Printf(TEXT("Captured profile SHA256: %s\nCaptured upload policy SHA256: %s\nCaptured V0 metadata SHA256: %s\nV0 infrastructure mapping: ConvAI uses installed SDK; ConvaiHTTP is uploader-only; ConvaiPakManager is replaced by Cloud Avatars; JsonBlueprintUtilities is uploader-only.\nMetaHuman requirements applied: %s\nLinux toolchain reference: %s (metadata only; no installation).\nReallusion content reference: %s (manual if required; no immutable checksum or automatic installation).\n"),
                *Result.Configuration.ProfileSha256, *Result.Configuration.UploadPolicySha256, *Result.Configuration.ModdingConfigSha256,
                Asset.bIsMetaHuman ? *FString::Join(Result.Configuration.MetaHumanPlugins, TEXT(", ")) : TEXT("not applicable"),
                *Result.Configuration.LinuxToolchainVersion, Result.Configuration.ReallusionDriveId.IsEmpty() ? TEXT("not published") : TEXT("available"));
            FFileHelper::SaveStringToFile(Details, *Result.LogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
        }
        if (Error.IsEmpty() && Self->Cancelled.load()) Error = TEXT("Packaging cancelled.");
        const FString BaseManifestPath = Asset.ProxyDirectory / TEXT("ConvaiAvatarBaseContent.json");
        if (Error.IsEmpty())
        {
            TSharedPtr<FJsonObject> BaseManifest;
            bool bComplete = false;
            FString Selected;
            const FMD5Hash Hash = FMD5Hash::HashFile(*BaseManifestPath);
            if (!Hash.IsValid() || !ReadPackagingJson(BaseManifestPath, BaseManifest) || !BaseManifest->TryGetBoolField(TEXT("complete"), bComplete) ||
                !bComplete || !BaseManifest->TryGetStringField(TEXT("selected_plugin"), Selected) || Selected != Asset.PluginName)
                Error = TEXT("The shared avatar support snapshot is incomplete or belongs to a different avatar.");
            else Result.BaseContentManifestHash = LexToString(Hash);
        }
        if (Error.IsEmpty()) CalculateInputFingerprint(Asset, Result.InputFingerprint, Error);
        TArray<FString> Platforms;
        if (Options.bIncludeWindows) Platforms.Add(TEXT("Windows"));
        if (Options.bIncludeLinux) Platforms.Add(TEXT("Linux"));
        if (Platforms.IsEmpty()) Report(TEXT("Preparing Source without cooking"));
        for (const FString& Platform : Platforms)
        {
            if (!Error.IsEmpty()) break;
            const FString Configuration = Result.Configuration.PlatformConfigurations.FindRef(Platform);
            FString CompressionArguments;
            if (Configuration.IsEmpty()) { Error = TEXT("The published upload policy is missing the selected ") + Platform + TEXT(" build configuration."); break; }
            if (!FConvaiAvatarProjectConfiguration::GetPakCompressionArguments(Result.Configuration.ProfileJson, Configuration, CompressionArguments, Error) ||
                !FConvaiAvatarProjectConfiguration::ApplyProxyProfile(Asset.ProxyDirectory, Error, Asset.PluginName, Result.Configuration.ProfileJson, Configuration)) break;
            const FString ConfigurationLog = Platform + TEXT(" requested runtime configuration: ") + Configuration +
                TEXT(". Content cook uses UnrealEditor; no Shipping SDK or game binary is built. Pak codec:") + CompressionArguments + TEXT("\n");
            FFileHelper::SaveStringToFile(ConfigurationLog, *Result.LogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
            const FString PlatformOutput = Output / Platform;
            if (!LocalDirectory(PlatformOutput, Error)) break;
            Report(TEXT("Cooking the selected avatar for ") + Platform);
            TArray<FString> Maps;
            if (Asset.Diorama.IsSet()) Maps.Add(Asset.Diorama->Level);
            Self->Run(Editor, BuildCookArguments(Result.ProjectPath, Asset.ProxyPluginDirectory / TEXT("Content"), PlatformOutput / TEXT("cook.log"), Platform, Maps), Result.LogPath, Error);
            if (!Error.IsEmpty()) break;
            const FString CookRoot = GetCookOutputDirectory(PlatformOutput / TEXT("cook.log")).Replace(TEXT("[Platform]"), *Platform);
            const FString CookContent = GetCookedAvatarDirectory(CookRoot, Asset.PluginName);
            TArray<FString> Files;
            if (!PF.DirectoryExists(*CookContent) || !WalkLocalFiles(CookContent, Files, Error, &Self->Cancelled))
            { if (Error.IsEmpty()) Error = TEXT("The fresh cook produced no selected-avatar directory. See the packaging log."); }
            if (Files.IsEmpty()) Error = TEXT("The cook produced no files for the selected avatar. See the packaging log.");
            const FString Mount = TEXT("/") + Asset.PluginName + TEXT("/");
            const FString EntryFile = CookContent / (Asset.EntryPoint.GetLongPackageName().Mid(Mount.Len()) + TEXT(".uasset"));
            if (Error.IsEmpty() && !PF.FileExists(*EntryFile)) Error = TEXT("The cook did not produce the avatar entry Blueprint. Packaging is blocked.");
            if (Error.IsEmpty() && Asset.Diorama.IsSet())
            {
                const FString DioramaFile = CookContent / (Asset.Diorama->Level.Mid(Mount.Len()) + TEXT(".umap"));
                if (!PF.FileExists(*DioramaFile)) Error = TEXT("The cook did not produce the diorama level. Packaging is blocked.");
            }
            Files.Sort();
            FString Response;
            for (const FString& File : Files)
            {
                FString Line;
                if (!MakePakResponseLine(CookRoot, Asset.PluginName, File, Line)) { Error = TEXT("The cooked output contains an unsafe pak path."); break; }
                Response += Line;
            }
            const FString ResponsePath = PlatformOutput / TEXT("pak-files.txt");
            const FString PakPath = PlatformOutput / (Asset.PluginName + TEXT(".pak"));
            if (Error.IsEmpty() && !FFileHelper::SaveStringToFile(Response, *ResponsePath)) Error = TEXT("Could not write the pak file list.");
            if (Error.IsEmpty()) { Report(TEXT("Creating the ") + Platform + TEXT(" avatar pak")); Self->Run(UnrealPak, FString::Printf(TEXT("\"%s\" -Create=\"%s\""), *PakPath, *ResponsePath) + CompressionArguments, Result.LogPath, Error); }
            if (Error.IsEmpty() && PF.FileSize(*PakPath) <= 0) Error = TEXT("Packaging did not produce a usable pak.");
            if (Error.IsEmpty()) Self->Run(UnrealPak, FString::Printf(TEXT("\"%s\" -Test"), *PakPath), Result.LogPath, Error);
            if (Error.IsEmpty()) ValidatePackagedInputs(Asset, Result.InputFingerprint, Error);
            if (Error.IsEmpty() && LexToString(FMD5Hash::HashFile(*BaseManifestPath)) != Result.BaseContentManifestHash)
                Error = TEXT("The shared avatar support snapshot changed while packaging. Package again before uploading.");
            if (Error.IsEmpty())
            {
                Result.PakPaths.Add(Platform, PakPath);
                if (Platform == TEXT("Windows")) Result.PakPath = PakPath;
            }
        }
        // Source-only has exactly the same archive binding as a cooked run.
        // Recheck after preparation even when there are no external processes.
        if (Error.IsEmpty() && Self->Cancelled.load()) Error = TEXT("Avatar preparation cancelled.");
        if (Error.IsEmpty()) ValidatePackagedInputs(Asset, Result.InputFingerprint, Error);
        if (Error.IsEmpty() && LexToString(FMD5Hash::HashFile(*BaseManifestPath)) != Result.BaseContentManifestHash)
            Error = TEXT("Shared avatar support changed during preparation. Prepare the upload again.");
        Lease.Release();
        AsyncTask(ENamedThreads::GameThread, [Completion=MoveTemp(Completion),Result,Error]() mutable { Completion(Result, Error); });
    });
}
void FConvaiAvatarPackaging::Cancel() { Cancelled.store(true); }
void FConvaiAvatarPackaging::Shutdown() { Cancel(); if (Worker.IsValid()) Worker.Wait(); }
