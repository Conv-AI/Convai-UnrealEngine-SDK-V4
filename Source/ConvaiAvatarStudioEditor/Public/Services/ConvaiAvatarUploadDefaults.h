// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpRequest.h"
#include "Misc/EnumRange.h"

enum class EConvaiAvatarDioramaSeverity : uint8
{
    Info,
    Warning,
    Error
};

enum class EConvaiAvatarDioramaLimit : uint8
{
    DynamicLights,
    ShadowLights,
    StaticMeshes,
    Triangles,
    Textures,
    TextureMaxSize,
    TextureMemoryMb,
    ConvaiObjects,
    Actors,
    Count
};
ENUM_RANGE_BY_COUNT(EConvaiAvatarDioramaLimit, EConvaiAvatarDioramaLimit::Count)

struct FConvaiAvatarDioramaLimitRule
{
    double Max = 0.0;
    EConvaiAvatarDioramaSeverity Severity = EConvaiAvatarDioramaSeverity::Error;
};

struct FConvaiAvatarDioramaLimits
{
    TMap<EConvaiAvatarDioramaLimit, FConvaiAvatarDioramaLimitRule> Rules;

    /** Missing limits are unchecked; a failed parse leaves the caller's rules untouched. */
    static bool Parse(const FString& Json, FConvaiAvatarDioramaLimits& OutLimits, FString& OutError);
};

struct FConvaiAvatarPublishedDefaults
{
    bool bIncludeSource = false;
    bool bIncludeWindows = false;
    bool bIncludeLinux = false;
    FConvaiAvatarDioramaLimits DioramaLimits;
    bool bHasDioramaLimits = false;
};

/** Reads only Convai's existing public uploader policy. Never receives account credentials. */
class FConvaiAvatarUploadDefaults : public TSharedFromThis<FConvaiAvatarUploadDefaults>
{
public:
    using FCompletion = TFunction<void(FConvaiAvatarPublishedDefaults, FString)>;
    ~FConvaiAvatarUploadDefaults();
    void Fetch(FCompletion Completion);
    void Cancel();
    static FString GetPolicyUrl();
    static bool Parse(const FString& Json,FConvaiAvatarPublishedDefaults& OutDefaults,FString& OutError);
private:
    void Finish(FConvaiAvatarPublishedDefaults Defaults,FString Error);
    FHttpRequestPtr Request;
    FCompletion PendingCompletion;
    FTSTicker::FDelegateHandle Timeout;
    uint64 Generation=0;
};
