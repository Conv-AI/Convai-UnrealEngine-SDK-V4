// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Workspace/ConvaiAvatarWorkspace.h"
#include "Services/ConvaiAvatarUploadDefaults.h"
#include <atomic>
struct FPluginDescriptor;

struct FConvaiAvatarPackageOptions
{
    bool bIncludeSource = true;
    bool bIncludeWindows = true;
    bool bIncludeLinux = false;
};

struct FConvaiAvatarPackagingCapabilities
{
    bool bSourceAvailable = true;
    bool bWindowsAvailable = false;
    bool bLinuxAvailable = false;
    FString SourceReason;
    FString WindowsReason;
    FString LinuxReason;
};

/** Validated, immutable remote configuration captured once for an upload operation. No executable config. */
struct FConvaiAvatarPackagingPolicy
{
    FString ProfileJson;
    FString ProfileFile;
    FString UploadPolicyFile;
    FString ModdingConfigFile;
    FString ProfileSha256;
    FString UploadPolicySha256;
    FString ModdingConfigSha256;
    TMap<FString, FString> PlatformConfigurations;
    FConvaiAvatarDioramaLimits DioramaLimits;
    bool bHasDioramaLimits = false;
    TArray<FString> RequiredPlugins;
    TArray<FString> MetaHumanPlugins;
    FString LinuxToolchainVersion;
    FString LinuxToolchainUrl;
    FString ReallusionDriveId;
};

struct FConvaiAvatarPackageResult
{
    /** Compatibility field: only the successfully verified Windows pak. */
    FString PakPath;
    /** Successfully verified paks keyed by the exact cook platform: Windows or Linux. */
    TMap<FString, FString> PakPaths;
    /** Per-run root; source archives also use this when no pak was requested. */
    FString ArtifactDirectory;
    FString ProjectPath;
    FString LogPath;
    /** Descriptor + canonical content bytes captured before any requested cooks. Also set for source-only preparation. */
    FString InputFingerprint;
    /** Exact completed base-content manifest; bind source archiving and requested cooks to this context. */
    FString BaseContentManifestHash;
    /** One resolved remote dependency version for every artifact transfer in this upload. */
    FString DependencyResolutionFile;
    FConvaiAvatarPackagingPolicy Configuration;
};

/** One asynchronous preparation at a time; only generated uploader files are changed. */
class FConvaiAvatarPackaging : public TSharedFromThis<FConvaiAvatarPackaging,ESPMode::ThreadSafe>
{
public:
    using FCompletion = TFunction<void(FConvaiAvatarPackageResult, FString)>;
    using FStatus = TFunction<void(FString)>;
    void Start(const FConvaiAvatarPreparedAsset& Asset, FCompletion Completion, FStatus Status);
    void Start(const FConvaiAvatarPreparedAsset& Asset, const FConvaiAvatarPackageOptions& Options, FCompletion Completion, FStatus Status);
    void Cancel();
    void Shutdown();
    /** Query on the game thread. Availability is a prerequisite check, not a successful cook claim. */
    static FConvaiAvatarPackagingCapabilities GetCapabilities();
    static bool ValidateOptions(const FConvaiAvatarPackageOptions& Options, const FConvaiAvatarPackagingCapabilities& Capabilities, FString& OutError);
    static bool ReadConfigurationSnapshot(const FString& UploaderRoot, const FString& ResolutionFile,
        FConvaiAvatarPackagingPolicy& OutPolicy, FString& OutError);
    static FString BuildCookArguments(const FString& Project, const FString& ContentDir, const FString& Log, const FString& Platform = TEXT("Windows"), const TArray<FString>& Maps = {});
    static FString GetCookOutputDirectory(const FString& Log);
    /** Persisted pak/source-wrapper identity, independent of the short local project name. */
    static FString GetRuntimeProjectName();
    static FString GetCookedAvatarDirectory(const FString& CookRoot, const FString& PluginName);
    static bool MakePakResponseLine(const FString& CookRoot, const FString& PluginName, const FString& File, FString& OutLine);
    static bool CalculateInputFingerprint(const FConvaiAvatarPreparedAsset& Asset, FString& OutFingerprint, FString& OutError);
    /** Safe to call on an archive/upload worker; does not access editor objects. */
    static bool ValidatePackagedInputs(const FConvaiAvatarPreparedAsset& Asset, const FString& ExpectedFingerprint, FString& OutError);
private:
    bool Run(const FString& Exe, const FString& Args, const FString& Log, FString& Error);
    bool PrepareProxy(const FConvaiAvatarPreparedAsset& Asset, const FString& ConvaiDirectory,
        const TMap<FString,FString>& PluginDirectories, const TMap<FString,FString>& BaseContentFiles,
        const FConvaiAvatarPackagingPolicy& Policy, const TMap<FString, FPluginDescriptor>& EnginePlugins,
        FString& Project, FString& Error);
    std::atomic<bool> Cancelled{false};
    TFuture<void> Worker;
};
