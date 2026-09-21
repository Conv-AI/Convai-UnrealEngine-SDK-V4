// Copyright Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarRemoteConfiguration.h"
#include "Utility/ConvaiEditorHttpCompat.h"
#include "Config/ConvaiAvatarProjectConfiguration.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    constexpr int32 MaxConfigurationBytes = 1024 * 1024;
    const FString RepositoryPrefix = TEXT("https://raw.githubusercontent.com/Conv-AI/Convai-UnrealEngine-ModdingTool/");
    struct FConfigurationBytes { FCriticalSection Mutex; TArray<uint8> Bytes; bool bTooLarge = false; };
    bool VersionNumber(const FString& Value)
    {
        TArray<FString> Parts; Value.ParseIntoArray(Parts, TEXT("."));
        if(Parts.Num()!=2 || Parts[0].IsEmpty() || Parts[1].IsEmpty() || Parts[0].Len()>2 || Parts[1].Len()>2)return false;
        for(const auto& Part:Parts)for(const TCHAR C:Part)if(!FChar::IsDigit(C))return false;
        return true;
    }
    FString JsonString(const TSharedPtr<FJsonObject>& Object, const FString& Key)
    { FString Result; if(Object) Object->TryGetStringField(Key,Result); return Result; }
    TSharedPtr<FJsonObject> JsonObject(const TSharedPtr<FJsonObject>& Object, const FString& Key)
    { const TSharedPtr<FJsonObject>* Child=nullptr; return Object && Object->TryGetObjectField(Key,Child) ? *Child : nullptr; }
    FString PluginNames(const TSharedPtr<FJsonObject>& Settings,const FString& Key)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;TArray<FString> Names;
        if(Settings && Settings->TryGetArrayField(Key,Values))for(const auto& Value:*Values)
        { FString Name;if(Value && Value->TryGetString(Name) && Name.Len()<=80)Names.Add(Name); }
        return Names.IsEmpty()?TEXT("Not provided"):FString::Join(Names,TEXT(", "));
    }
}

FString FConvaiAvatarRemoteConfiguration::GetBaseUrl()
{
    FString Branch=FPlatformMisc::GetEnvironmentVariable(TEXT("CONVAI_MODDING_CONFIG_BRANCH"));
    if(Branch.IsEmpty())Branch=TEXT("staging");
    if(Branch.Len()>200 || Branch.Contains(TEXT("..")) || Branch.StartsWith(TEXT("/")) || Branch.EndsWith(TEXT("/")))return {};
    for(const TCHAR C:Branch)if(!FChar::IsAlnum(C) && C!=TEXT('_') && C!=TEXT('-') && C!=TEXT('.') && C!=TEXT('/'))return {};
    return RepositoryPrefix+Branch+TEXT("/");
}

FString FConvaiAvatarRemoteConfiguration::GetLocalFile(const FString& Relative)
{
    const FString Directory=FPlatformMisc::GetEnvironmentVariable(TEXT("CONVAI_MODDING_CONFIG_DIR"));
    if(Directory.IsEmpty())return {};
    FString Path=FPaths::ConvertRelativePathToFull(Directory/Relative);
    if(!IFileManager::Get().FileExists(*Path) && Relative.StartsWith(TEXT("resources/")))Path=FPaths::ConvertRelativePathToFull(Directory/FPaths::GetCleanFilename(Relative));
    return Path;
}

bool FConvaiAvatarRemoteConfiguration::IsEngineMismatch(const FString& ActualVersion,const FString& SupportedVersion)
{ return VersionNumber(ActualVersion) && VersionNumber(SupportedVersion) && ActualVersion!=SupportedVersion; }

bool FConvaiAvatarRemoteConfiguration::ParseMetadata(const FString& ConfigJson,const FString& VersionJson,FConvaiAvatarRemoteConfigurationData& Out,FString& Error)
{
    Error.Reset();TSharedPtr<FJsonObject> Config,Version;
    if(ConfigJson.Len()>MaxConfigurationBytes || VersionJson.Len()>MaxConfigurationBytes ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ConfigJson),Config) || !Config ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(VersionJson),Version) || !Version)
    {Error=TEXT("The published uploader configuration could not be read.");return false;}
    Out.CurrentEngineVersion=JsonString(Version,TEXT("current-ue-version"));
    Out.MigrationTargetVersion=JsonString(Version,TEXT("target-ue-version"));
    if(!VersionNumber(Out.CurrentEngineVersion) || !VersionNumber(Out.MigrationTargetVersion))
    {Error=TEXT("The published current and migration engine versions are unavailable.");return false;}
    const auto Settings=JsonObject(Config,TEXT("project_settings"));
    const auto Cross=JsonObject(Config,TEXT("cross_compilation"));
    const FString Toolchain=JsonString(JsonObject(Cross,TEXT("toolchain_versions")),Out.CurrentEngineVersion.Replace(TEXT("."),TEXT("_")));
    Out.DependencySummary=TEXT("Required uploader plugins: ")+PluginNames(Settings,TEXT("required_plugins"))+
        TEXT("\nMetaHuman support plugins: ")+PluginNames(Settings,TEXT("metahuman_plugins"))+
        TEXT("\nLinux toolchain: ")+(Toolchain.IsEmpty()?TEXT("Not provided"):Toolchain)+
        TEXT(". Installation is not part of this workflow.")+
        TEXT("\nFor other avatar types, including Reallusion, use an avatar you have already prepared in your project.");
    Out.ConfigJson=ConfigJson;Out.VersionJson=VersionJson;return true;
}

FConvaiAvatarRemoteConfiguration::~FConvaiAvatarRemoteConfiguration(){Cancel();}
void FConvaiAvatarRemoteConfiguration::Cancel()
{
    ++Generation;Completion={};
    if(Timeout.IsValid()){FTSTicker::GetCoreTicker().RemoveTicker(Timeout);Timeout.Reset();}
    if(Request){Request->OnProcessRequestComplete().Unbind();Request->CancelRequest();Request.Reset();}
}
void FConvaiAvatarRemoteConfiguration::Finish(FString Error)
{auto Callback=MoveTemp(Completion);auto Result=MoveTemp(Data);Cancel();if(Callback)Callback(MoveTemp(Result),MoveTemp(Error));}
void FConvaiAvatarRemoteConfiguration::Fetch(FCompletion InCompletion)
{
    check(IsInGameThread());Cancel();Completion=MoveTemp(InCompletion);Data={};Step=0;
    if(GetBaseUrl().IsEmpty()){Finish(TEXT("The configuration branch override is invalid."));return;}
    Data.SourceNotice=GetLocalFile(TEXT("Version.json")).IsEmpty()?TEXT("Fresh public configuration: ")+GetBaseUrl():TEXT("Using the explicit local configuration override.");
    Next();
}
void FConvaiAvatarRemoteConfiguration::AcceptDocument(FString Json)
{
    if(Step==0)
    {
        Data.ConfigJson=MoveTemp(Json);TSharedPtr<FJsonObject> Root;
        if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Data.ConfigJson),Root)||!Root){Finish(TEXT("The published uploader configuration could not be read."));return;}
        Data.ProfileSourceUrl=JsonString(JsonObject(Root,TEXT("cloud_avatars")),TEXT("project_profile_url"));
        if(Data.ProfileSourceUrl.IsEmpty())Data.ProfileSourceUrl=GetBaseUrl()+TEXT("resources/avatar_studio_project_profile.json");
    }
    else if(Step==1)
    {
        FString Error;if(!ParseMetadata(Data.ConfigJson,Json,Data,Error)){Finish(Error);return;}
    }
    else
    {
        FString Error;if(!FConvaiAvatarProjectConfiguration::ValidateProfile(Json,Error)){Finish(Error);return;}
        Data.ProfileJson=MoveTemp(Json);Finish({});return;
    }
    ++Step;Next();
}
void FConvaiAvatarRemoteConfiguration::Next()
{
    const FString Relative=Step==0?TEXT("resources/modding_tool_config.json"):(Step==1?TEXT("Version.json"):TEXT("resources/avatar_studio_project_profile.json"));
    const FString Local=GetLocalFile(Relative);
    if(!Local.IsEmpty())
    {
        FString Json;const int64 Size=IFileManager::Get().FileSize(*Local);
        if(Size<=0 || Size>MaxConfigurationBytes || !FFileHelper::LoadFileToString(Json,*Local)){Finish(TEXT("The local configuration override is incomplete or unreadable. Its files were kept."));return;}
        if(Step==2)Data.ProfileSourceUrl=TEXT("Local configuration override");
        AcceptDocument(MoveTemp(Json));return;
    }
    const FString Url=Step==2?Data.ProfileSourceUrl:GetBaseUrl()+Relative;
    if(!Url.StartsWith(RepositoryPrefix,ESearchCase::CaseSensitive) || Url.Contains(TEXT("?")) || Url.Contains(TEXT("#")) || Url.Contains(TEXT(" ")))
    {Finish(TEXT("The project profile must come from Convai's public configuration repository."));return;}
    Request=FHttpModule::Get().CreateRequest();Request->SetURL(Url);Request->SetVerb(TEXT("GET"));
    Request->SetHeader(TEXT("Accept"),TEXT("application/json"));Request->SetHeader(TEXT("Cache-Control"),TEXT("no-cache"));
    Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);Request->SetTimeout(12.f);
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
    Request->SetActivityTimeout(8.f);
#endif
    auto Bytes=MakeShared<FConfigurationBytes,ESPMode::ThreadSafe>();
    if(!ConvaiEditorHttpCompat::SetReceiveStream(*Request, [Bytes](void* Buffer,int64& Length)
    {FScopeLock Lock(&Bytes->Mutex);if(Length<0 || Length>MaxConfigurationBytes-Bytes->Bytes.Num()){Bytes->bTooLarge=true;Length=0;return;}if(Length>0)Bytes->Bytes.Append(static_cast<const uint8*>(Buffer),static_cast<int32>(Length));}))
    {Finish(TEXT("The public configuration request could not start."));return;}
    const TWeakPtr<FConvaiAvatarRemoteConfiguration> Weak=AsShared();const uint64 Op=Generation;
    Request->OnProcessRequestComplete().BindLambda([Weak,Bytes,Op](FHttpRequestPtr CompletedRequest,FHttpResponsePtr Response,bool bSuccess)
    {
        const auto Self=Weak.Pin();if(!Self || Self->Generation!=Op || Self->Request!=CompletedRequest)return;
        // The value parameter keeps the executing delegate alive through this callback.
        // Finish must not self-unbind it; Next may install a different active request.
        Self->Request.Reset();
        if(Self->Timeout.IsValid()){FTSTicker::GetCoreTicker().RemoveTicker(Self->Timeout);Self->Timeout.Reset();}
        FString Json,Error;
        {FScopeLock Lock(&Bytes->Mutex);if(Bytes->bTooLarge)Error=TEXT("The published configuration exceeds its size limit.");
        else if(!bSuccess || !Response || Response->GetResponseCode()!=200 || Bytes->Bytes.IsEmpty())Error=TEXT("The published project configuration is unavailable. Refresh to retry.");
        else{const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes->Bytes.GetData()),Bytes->Bytes.Num());Json=FString(Text.Length(),Text.Get());}}
        if(!Error.IsEmpty()){Self->Finish(Error);return;}Self->AcceptDocument(MoveTemp(Json));
    });
    Timeout=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak,Op](float)
    {if(const auto Self=Weak.Pin();Self && Self->Generation==Op)Self->Finish(TEXT("Reading the public configuration timed out. Refresh to retry."));return false;}),12.f);
    if(!Request->ProcessRequest() && Generation==Op)Finish(TEXT("The public configuration request could not start."));
}
