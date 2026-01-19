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

    const FString ContentBasePath = TEXT("https://cdn.jsdelivr.net/gh/Conv-AI/convai-plugin-content@main");

    const FString AnnouncementsCommonURL = ContentBasePath + TEXT("/announcements-common.json");

    const FString AnnouncementsUnrealURL = ContentBasePath + TEXT("/announcements-unreal.json");
    const FString ChangelogsUnrealURL = ContentBasePath + TEXT("/changelogs-unreal.json");
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

TArray<FString> FConvaiURLs::GetAnnouncementsFeedURLs()
{
    TArray<FString> URLs;

    URLs.Add(AnnouncementsCommonURL);
    URLs.Add(AnnouncementsUnrealURL);

    return URLs;
}

TArray<FString> FConvaiURLs::GetChangelogsFeedURLs()
{
    TArray<FString> URLs;

    URLs.Add(ChangelogsUnrealURL);

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
