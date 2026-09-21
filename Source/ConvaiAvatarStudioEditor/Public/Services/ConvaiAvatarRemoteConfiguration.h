// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpRequest.h"

struct FConvaiAvatarRemoteConfigurationData
{
    FString ConfigJson;
    FString VersionJson;
    FString ProfileJson;
    FString ProfileSourceUrl;
    FString CurrentEngineVersion;
    FString MigrationTargetVersion;
    FString DependencySummary;
    FString SourceNotice;
};

/** Public configuration only. No account state, credentials, plugin installation, or engine changes. */
class FConvaiAvatarRemoteConfiguration : public TSharedFromThis<FConvaiAvatarRemoteConfiguration>
{
public:
    using FCompletion = TFunction<void(FConvaiAvatarRemoteConfigurationData, FString)>;
    ~FConvaiAvatarRemoteConfiguration();
    void Fetch(FCompletion Completion);
    void Cancel();
    static FString GetBaseUrl();
    static FString GetLocalFile(const FString& Relative);
    static bool ParseMetadata(const FString& ConfigJson, const FString& VersionJson, FConvaiAvatarRemoteConfigurationData& Out, FString& Error);
    static bool IsEngineMismatch(const FString& ActualVersion, const FString& SupportedVersion);
private:
    void Next();
    void AcceptDocument(FString Json);
    void Finish(FString Error);
    FHttpRequestPtr Request;
    FCompletion Completion;
    FConvaiAvatarRemoteConfigurationData Data;
    FTSTicker::FDelegateHandle Timeout;
    uint64 Generation = 0;
    int32 Step = 0;
};
