/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiURLs.h
 *
 * Centralized URL management for ConvaiEditor plugin.
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Centralized URL management for ConvaiEditor plugin.
 */
class CONVAIEDITOR_API FConvaiURLs
{
public:
    //----------------------------------------
    // UI Navigation URLs
    //----------------------------------------

    /** Main Convai dashboard URL */
    static const FString &GetDashboardURL();

    /** Convai documentation URL */
    static const FString &GetDocumentationURL();

    /** Convai forum URL */
    static const FString &GetForumURL();

    /** Convai YouTube channel URL */
    static const FString &GetYouTubeURL();

    /** Convai experiences platform URL */
    static const FString &GetExperiencesURL();

    /** Convai API documentation URL */
    static const FString &GetAPIDocumentationURL();

    //----------------------------------------
    // API URLs (Integrated with Convai Plugin)
    //----------------------------------------

    /** Convai API base URL */
    static const FString &GetAPIBaseURL();

    /** API key validation endpoint */
    static FString GetAPIValidationURL();

    /** Character list endpoint */
    static FString GetCharacterListURL();

    /** Character details endpoint */
    static FString GetCharacterDetailsURL();

    /** Voice list endpoint */
    static FString GetVoiceListURL();

    /** Experience session endpoint */
    static FString GetExperienceSessionURL();

    /** User API usage endpoint */
    static FString GetUserAPIUsageURL();

    /** (Future) User profile endpoint */
    static FString GetUserProfileURL();

    /** (Future) Usage history endpoint */
    static FString GetUsageHistoryURL();

    //----------------------------------------
    // Content URLs
    //----------------------------------------

    /** Get announcements feed URLs */
    static TArray<FString> GetAnnouncementsFeedURLs();

    /** Get changelogs feed URLs */
    static TArray<FString> GetChangelogsFeedURLs();

    //----------------------------------------
    // URL Construction
    //----------------------------------------

    /** Build full URL with endpoint path */
    static FString BuildFullURL(const FString &EndpointPath, bool bUseBeta = false);

    /** Get base URL */
    static FString GetBaseURL(bool bUseBeta = false);

private:
    FConvaiURLs() = delete;
    ~FConvaiURLs() = delete;
};
