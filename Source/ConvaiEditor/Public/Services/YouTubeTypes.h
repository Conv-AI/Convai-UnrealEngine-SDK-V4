/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * YouTubeTypes.h
 *
 * Data structures for YouTube video information.
 */

#pragma once

#include "CoreMinimal.h"

/**
 * YouTube video information.
 */
struct CONVAIEDITOR_API FYouTubeVideoInfo
{
    FString Title;
    FString Description;
    FString VideoURL;
    FString ThumbnailURL;
    FString VideoID;
    FDateTime PublicationDate;
    FString Duration;
    FString Author;

    FYouTubeVideoInfo()
        : PublicationDate(FDateTime::Now())
    {
    }

    bool IsValid() const
    {
        return !Title.IsEmpty() && !VideoURL.IsEmpty() && !ThumbnailURL.IsEmpty();
    }

    bool operator==(const FYouTubeVideoInfo &Other) const
    {
        return VideoID == Other.VideoID;
    }

    bool operator!=(const FYouTubeVideoInfo &Other) const
    {
        return !(*this == Other);
    }
};
