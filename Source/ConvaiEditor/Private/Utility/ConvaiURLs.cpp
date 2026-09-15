/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiURLs.cpp
 *
 * Implementation of centralized URL management for ConvaiEditor plugin.
 * Integrated with Convai plugin's URL system for consistency.
 */

#include "Utility/ConvaiURLs.h"

namespace
{
    const FString DashboardURL = TEXT("https://convai.com");
    const FString DocumentationURL = TEXT("https://docs.convai.com");
    const FString ForumURL = TEXT("https://forum.convai.com");
    const FString YouTubeURL = TEXT("https://www.youtube.com/@convai");
    const FString ExperiencesURL = TEXT("https://x.convai.com");
    const FString APIDocumentationURL = TEXT("https://docs.convai.com/api-docs");

    const FString APIBaseURL = TEXT("https://api.convai.com");
    const FString APIBetaURL = TEXT("https://beta-api.convai.com");

    const FString CharacterListEndpoint = TEXT("character/list");
    const FString CharacterDetailsEndpoint = TEXT("character/details");
    const FString VoiceListEndpoint = TEXT("voice/list");
    const FString ExperienceSessionEndpoint = TEXT("xp/sessions/detail");
    const FString APIValidationEndpoint = TEXT("user/user-api-usage");

    const FString ContentJsDelivrBasePath = TEXT("https://cdn.jsdelivr.net/gh/Conv-AI/convai-plugin-content@main");
    const FString ContentRawBasePath = TEXT("https://raw.githubusercontent.com/Conv-AI/convai-plugin-content/main");

    const FString AnnouncementsCommonRawURL = ContentRawBasePath + TEXT("/announcements-common.json");
    const FString AnnouncementsCommonCdnURL = ContentJsDelivrBasePath + TEXT("/announcements-common.json");

    const FString AnnouncementsUnrealRawURL = ContentRawBasePath + TEXT("/announcements-unreal.json");
    const FString AnnouncementsUnrealCdnURL = ContentJsDelivrBasePath + TEXT("/announcements-unreal.json");

    const FString ChangelogsUnrealRawURL = ContentRawBasePath + TEXT("/changelogs-unreal.json");
    const FString ChangelogsUnrealCdnURL = ContentJsDelivrBasePath + TEXT("/changelogs-unreal.json");
}

const FString &FConvaiURLs::GetDashboardURL()
{
    return DashboardURL;
}

const FString &FConvaiURLs::GetDocumentationURL()
{
    return DocumentationURL;
}

const FString &FConvaiURLs::GetForumURL()
{
    return ForumURL;
}

const FString &FConvaiURLs::GetYouTubeURL()
{
    return YouTubeURL;
}

const FString &FConvaiURLs::GetExperiencesURL()
{
    return ExperiencesURL;
}

const FString &FConvaiURLs::GetAPIDocumentationURL()
{
    return APIDocumentationURL;
}

const FString &FConvaiURLs::GetAPIBaseURL()
{
    return APIBaseURL;
}

FString FConvaiURLs::GetAPIValidationURL()
{
    return BuildFullURL(APIValidationEndpoint);
}

FString FConvaiURLs::GetCharacterListURL()
{
    return BuildFullURL(CharacterListEndpoint);
}

FString FConvaiURLs::GetCharacterDetailsURL()
{
    return BuildFullURL(CharacterDetailsEndpoint);
}

FString FConvaiURLs::GetVoiceListURL()
{
    return BuildFullURL(VoiceListEndpoint);
}

FString FConvaiURLs::GetExperienceSessionURL()
{
    return BuildFullURL(ExperienceSessionEndpoint);
}

FString FConvaiURLs::GetUserAPIUsageURL()
{
    return BuildFullURL(TEXT("/user/user-api-usage"));
}

FString FConvaiURLs::GetUserProfileURL()
{
    return BuildFullURL(TEXT("/user/profile"));
}

FString FConvaiURLs::GetUsageHistoryURL()
{
    return BuildFullURL(TEXT("/user/usage-history"));
}

TArray<FString> FConvaiURLs::GetAnnouncementsCommonFeedURLs()
{
    TArray<FString> URLs;
    URLs.Add(AnnouncementsCommonRawURL);
    URLs.Add(AnnouncementsCommonCdnURL);
    return URLs;
}

TArray<FString> FConvaiURLs::GetAnnouncementsUnrealFeedURLs()
{
    TArray<FString> URLs;
    URLs.Add(AnnouncementsUnrealRawURL);
    URLs.Add(AnnouncementsUnrealCdnURL);
    return URLs;
}

TArray<FString> FConvaiURLs::GetChangelogUnrealFeedURLs()
{
    TArray<FString> URLs;
    URLs.Add(ChangelogsUnrealRawURL);
    URLs.Add(ChangelogsUnrealCdnURL);
    return URLs;
}

TArray<FString> FConvaiURLs::GetAnnouncementsFeedURLs()
{
    TArray<FString> URLs;

    const TArray<FString> CommonURLs = GetAnnouncementsCommonFeedURLs();
    if (CommonURLs.Num() > 0)
    {
        URLs.Add(CommonURLs[0]);
    }

    const TArray<FString> UnrealURLs = GetAnnouncementsUnrealFeedURLs();
    if (UnrealURLs.Num() > 0)
    {
        URLs.Add(UnrealURLs[0]);
    }

    return URLs;
}

TArray<FString> FConvaiURLs::GetChangelogsFeedURLs()
{
    TArray<FString> URLs;

    const TArray<FString> UnrealURLs = GetChangelogUnrealFeedURLs();
    if (UnrealURLs.Num() > 0)
    {
        URLs.Add(UnrealURLs[0]);
    }

    return URLs;
}

FString FConvaiURLs::BuildFullURL(const FString &EndpointPath, bool bUseBeta)
{
    FString BaseURL = GetBaseURL(bUseBeta);

    if (BaseURL.EndsWith(TEXT("/")))
    {
        BaseURL.RemoveFromEnd(TEXT("/"));
    }

    if (EndpointPath.StartsWith(TEXT("/")))
    {
        return BaseURL + EndpointPath;
    }
    else
    {
        return BaseURL + TEXT("/") + EndpointPath;
    }
}

FString FConvaiURLs::GetBaseURL(bool bUseBeta)
{
    return bUseBeta ? APIBetaURL : APIBaseURL;
}
