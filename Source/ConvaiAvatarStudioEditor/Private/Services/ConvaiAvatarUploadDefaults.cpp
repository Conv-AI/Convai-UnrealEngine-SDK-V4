// Copyright Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarUploadDefaults.h"
#include "Utility/ConvaiEditorHttpCompat.h"
#include "Services/ConvaiAvatarRemoteConfiguration.h"

#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ScopeLock.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    constexpr int32 MaximumPolicyBytes=64*1024;
    const TCHAR* const DioramaLimitNames[] = {
        TEXT("dynamic-lights"), TEXT("shadow-lights"), TEXT("static-meshes"),
        TEXT("triangles"), TEXT("textures"), TEXT("texture-max-size"),
        TEXT("texture-memory-mb"), TEXT("convai-objects"), TEXT("actors")
    };
    static_assert(UE_ARRAY_COUNT(DioramaLimitNames) == static_cast<int32>(EConvaiAvatarDioramaLimit::Count),
        "Every diorama limit needs its policy name");
    struct FPolicyBytes
    {
        FCriticalSection Mutex;
        TArray<uint8> Bytes;
        bool bTooLarge=false;
    };
}

bool FConvaiAvatarDioramaLimits::Parse(const FString& Json, FConvaiAvatarDioramaLimits& OutLimits, FString& OutError)
{
    OutError.Reset();
    TSharedPtr<FJsonObject> Root;
    if (Json.IsEmpty() || Json.Len() > MaximumPolicyBytes ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root)
    { OutError = TEXT("The published diorama limits could not be read."); return false; }

    FConvaiAvatarDioramaLimits Parsed;
    const auto SectionValue = Root->TryGetField(TEXT("limited-environment"));
    if (!SectionValue) { OutLimits = MoveTemp(Parsed); return true; }
    const TSharedPtr<FJsonObject>* Section = nullptr;
    if (SectionValue->Type != EJson::Object || !Root->TryGetObjectField(TEXT("limited-environment"), Section))
    { OutError = TEXT("The published limited-environment section is not an object."); return false; }

    for (const EConvaiAvatarDioramaLimit Limit : TEnumRange<EConvaiAvatarDioramaLimit>())
    {
        const TCHAR* Name = DioramaLimitNames[static_cast<int32>(Limit)];
        const auto EntryValue = (*Section)->TryGetField(Name);
        if (!EntryValue) continue;
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (EntryValue->Type != EJson::Object || !(*Section)->TryGetObjectField(Name, Entry))
        { OutError = FString::Printf(TEXT("The published diorama limit \"%s\" is not an object."), Name); return false; }

        FConvaiAvatarDioramaLimitRule Rule;
        const auto MaxValue = (*Entry)->TryGetField(TEXT("max"));
        if (!MaxValue || MaxValue->Type != EJson::Number || !(*Entry)->TryGetNumberField(TEXT("max"), Rule.Max) || !FMath::IsFinite(Rule.Max))
        { OutError = FString::Printf(TEXT("The published diorama limit \"%s\" has no valid max."), Name); return false; }
        FString Severity;
        const auto SeverityValue = (*Entry)->TryGetField(TEXT("severity"));
        if (!SeverityValue || SeverityValue->Type != EJson::String || !(*Entry)->TryGetStringField(TEXT("severity"), Severity))
        { OutError = FString::Printf(TEXT("The published diorama limit \"%s\" has no valid severity."), Name); return false; }
        if (Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase)) Rule.Severity = EConvaiAvatarDioramaSeverity::Error;
        else if (Severity.Equals(TEXT("warning"), ESearchCase::IgnoreCase)) Rule.Severity = EConvaiAvatarDioramaSeverity::Warning;
        else
        { OutError = FString::Printf(TEXT("The published diorama limit \"%s\" has an unknown severity."), Name); return false; }
        Parsed.Rules.Add(Limit, Rule);
    }
    OutLimits = MoveTemp(Parsed);
    return true;
}

FString FConvaiAvatarUploadDefaults::GetPolicyUrl()
{
    // Follow the same staging line / explicit development override as the operation resolver.
    const FString Base=FConvaiAvatarRemoteConfiguration::GetBaseUrl();
    return Base.IsEmpty()?FString():Base+TEXT("resources/asset_uploader_config.json");
}

bool FConvaiAvatarUploadDefaults::Parse(const FString& Json,FConvaiAvatarPublishedDefaults& OutDefaults,FString& OutError)
{
    OutDefaults={};OutError.Reset();
    if(Json.IsEmpty() || Json.Len()>MaximumPolicyBytes){OutError=TEXT("The published upload defaults are empty or too large.");return false;}
    TSharedPtr<FJsonObject> Root;
    const TSharedPtr<FJsonObject>* Engine=nullptr;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Root) || !Root)
    {OutError=TEXT("The published upload defaults could not be read.");return false;}
    const auto EngineValue=Root->TryGetField(TEXT("unreal-engine"));
    if(!EngineValue || EngineValue->Type!=EJson::Object || !Root->TryGetObjectField(TEXT("unreal-engine"),Engine))
    {OutError=TEXT("The published upload defaults could not be read.");return false;}
    FConvaiAvatarPublishedDefaults Parsed;
    const auto RawValue=Root->TryGetField(TEXT("raw-project-upload"));
    if(RawValue && (RawValue->Type!=EJson::Boolean || !Root->TryGetBoolField(TEXT("raw-project-upload"),Parsed.bIncludeSource)))
    {OutError=TEXT("The published source-upload default is invalid.");return false;}
    for(const FString& Platform:{FString(TEXT("windows")),FString(TEXT("linux"))})
    {
        const auto SectionValue=(*Engine)->TryGetField(Platform);
        if(!SectionValue)continue;
        const TSharedPtr<FJsonObject>* Section=nullptr;
        bool bEnabled=false;FString Configuration;
        if(SectionValue->Type!=EJson::Object || !(*Engine)->TryGetObjectField(Platform,Section))
        {OutError=TEXT("A published platform default is incomplete.");return false;}
        const auto EnabledValue=(*Section)->TryGetField(TEXT("should-package"));
        const auto ConfigurationValue=(*Section)->TryGetField(TEXT("configuration"));
        if(!EnabledValue || EnabledValue->Type!=EJson::Boolean || !(*Section)->TryGetBoolField(TEXT("should-package"),bEnabled) ||
            (bEnabled && (!ConfigurationValue || ConfigurationValue->Type!=EJson::String ||
                !(*Section)->TryGetStringField(TEXT("configuration"),Configuration) || Configuration.TrimStartAndEnd().IsEmpty())))
        {OutError=TEXT("A published platform default is incomplete.");return false;}
        if(Platform==TEXT("windows"))Parsed.bIncludeWindows=bEnabled;else Parsed.bIncludeLinux=bEnabled;
    }
    if(!FConvaiAvatarDioramaLimits::Parse(Json,Parsed.DioramaLimits,OutError))return false;
    Parsed.bHasDioramaLimits=!Parsed.DioramaLimits.Rules.IsEmpty();
    if(!Parsed.bIncludeSource && !Parsed.bIncludeWindows && !Parsed.bIncludeLinux)
    {OutError=TEXT("The published defaults select no upload files.");return false;}
    OutDefaults=Parsed;return true;
}

FConvaiAvatarUploadDefaults::~FConvaiAvatarUploadDefaults(){Cancel();}

void FConvaiAvatarUploadDefaults::Cancel()
{
    ++Generation;
    PendingCompletion={};
    if(Timeout.IsValid()){FTSTicker::GetCoreTicker().RemoveTicker(Timeout);Timeout.Reset();}
    if(Request){Request->OnProcessRequestComplete().Unbind();Request->CancelRequest();Request.Reset();}
}

void FConvaiAvatarUploadDefaults::Finish(FConvaiAvatarPublishedDefaults Defaults,FString Error)
{
    check(IsInGameThread());
    auto Completion=MoveTemp(PendingCompletion);
    Cancel();
    if(Completion)Completion(Defaults,MoveTemp(Error));
}

void FConvaiAvatarUploadDefaults::Fetch(FCompletion Completion)
{
    check(IsInGameThread());Cancel();PendingCompletion=MoveTemp(Completion);
    const FString Local=FConvaiAvatarRemoteConfiguration::GetLocalFile(TEXT("resources/asset_uploader_config.json"));
    if(!Local.IsEmpty())
    {
        FString Json,Error;FConvaiAvatarPublishedDefaults Defaults;
        const int64 Size=IFileManager::Get().FileSize(*Local);
        if(Size<=0 || Size>MaximumPolicyBytes || !FFileHelper::LoadFileToString(Json,*Local))Error=TEXT("The local upload-policy override is incomplete or unreadable.");
        else Parse(Json,Defaults,Error);
        Finish(Defaults,MoveTemp(Error));return;
    }
    if(GetPolicyUrl().IsEmpty()){Finish({},TEXT("The configuration branch override is invalid."));return;}
    Request=FHttpModule::Get().CreateRequest();
    Request->SetURL(GetPolicyUrl());Request->SetVerb(TEXT("GET"));
    Request->SetHeader(TEXT("Accept"),TEXT("application/json"));
    Request->SetHeader(TEXT("Cache-Control"),TEXT("no-cache"));
    Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
    Request->SetTimeout(12.0f);
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
    Request->SetActivityTimeout(8.0f);
#endif
    // The URL is fixed and public. No Convai auth, API key, token, or Assets client is involved.
    auto Bytes=MakeShared<FPolicyBytes,ESPMode::ThreadSafe>();
    if(!ConvaiEditorHttpCompat::SetReceiveStream(*Request, [Bytes](void* Data,int64& Length)
    {
        FScopeLock Lock(&Bytes->Mutex);
        if(Length<0 || Length>MaximumPolicyBytes-Bytes->Bytes.Num()){Bytes->bTooLarge=true;Length=0;return;}
        if(Length>0)Bytes->Bytes.Append(static_cast<const uint8*>(Data),static_cast<int32>(Length));
    }))
    {Finish({},TEXT("The public upload-defaults request could not start."));return;}
    const TWeakPtr<FConvaiAvatarUploadDefaults> Weak=AsShared();
    const uint64 Op=Generation;
    Request->OnProcessRequestComplete().BindLambda([Weak,Bytes,Op](FHttpRequestPtr,FHttpResponsePtr Response,bool bSuccess)
    {
        const auto Self=Weak.Pin();if(!Self || Self->Generation!=Op)return;
        FString Json,Error;
        {
            FScopeLock Lock(&Bytes->Mutex);
            if(Bytes->bTooLarge)Error=TEXT("The published upload defaults exceed their size limit.");
            else if(!bSuccess || !Response || Response->GetResponseCode()!=200)Error=TEXT("Convai's published upload defaults are unavailable.");
            else if(Bytes->Bytes.IsEmpty())Error=TEXT("The published upload defaults are empty.");
            else
            {
                const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Bytes->Bytes.GetData()),Bytes->Bytes.Num());
                Json=FString(Decoded.Length(),Decoded.Get());
            }
        }
        FConvaiAvatarPublishedDefaults Defaults;
        if(Error.IsEmpty())Parse(Json,Defaults,Error);
        Self->Finish(Defaults,MoveTemp(Error));
    });
    Timeout=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak,Op](float)
    {
        if(const auto Self=Weak.Pin();Self && Self->Generation==Op)Self->Finish({},TEXT("Reading the published upload defaults timed out."));
        return false;
    }),12.0f);
    if(!Request->ProcessRequest() && Generation==Op)Finish({},TEXT("The public upload-defaults request could not start."));
}
