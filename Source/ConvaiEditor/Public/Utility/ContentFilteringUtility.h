/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ContentFilteringUtility.h
 *
 * Semantic versioning and content filtering utilities.
 */

#pragma once

#include "CoreMinimal.h"
#include "Models/ConvaiAnnouncementData.h"

/** Semantic versioning parser and comparator */
struct FSemanticVersion
{
    int32 Major;
    int32 Minor;
    int32 Patch;

    /** Default constructor (invalid version) */
    FSemanticVersion()
        : Major(-1), Minor(-1), Patch(-1)
    {
    }

    /** Explicit constructor */
    FSemanticVersion(int32 InMajor, int32 InMinor = 0, int32 InPatch = 0)
        : Major(InMajor), Minor(InMinor), Patch(InPatch)
    {
    }

    /** Parse semantic version from string */
    static FSemanticVersion Parse(const FString &VersionString)
    {
        if (VersionString.IsEmpty())
        {
            return FSemanticVersion();
        }

        TArray<FString> Parts;
        VersionString.ParseIntoArray(Parts, TEXT("."));

        if (Parts.Num() == 0)
        {
            return FSemanticVersion();
        }

        FSemanticVersion Version;
        Version.Major = Parts.Num() > 0 ? FCString::Atoi(*Parts[0]) : 0;
        Version.Minor = Parts.Num() > 1 ? FCString::Atoi(*Parts[1]) : 0;
        Version.Patch = Parts.Num() > 2 ? FCString::Atoi(*Parts[2]) : 0;

        return Version;
    }

    /** Check if version is valid */
    bool IsValid() const
    {
        return Major >= 0;
    }

    /** Convert to string */
    FString ToString() const
    {
        if (!IsValid())
        {
            return TEXT("Invalid");
        }
        return FString::Printf(TEXT("%d.%d.%d"), Major, Minor, Patch);
    }

    bool operator==(const FSemanticVersion &Other) const
    {
        return Major == Other.Major && Minor == Other.Minor && Patch == Other.Patch;
    }

    bool operator!=(const FSemanticVersion &Other) const
    {
        return !(*this == Other);
    }

    bool operator<(const FSemanticVersion &Other) const
    {
        if (Major != Other.Major)
            return Major < Other.Major;
        if (Minor != Other.Minor)
            return Minor < Other.Minor;
        return Patch < Other.Patch;
    }

    bool operator<=(const FSemanticVersion &Other) const
    {
        return *this < Other || *this == Other;
    }

    bool operator>(const FSemanticVersion &Other) const
    {
        return !(*this <= Other);
    }

    bool operator>=(const FSemanticVersion &Other) const
    {
        return !(*this < Other);
    }
};

/** Static utility for platform detection and identification */
class FPlatformInfo
{
public:
    /** Get current platform identifier */
    static FString GetCurrentPlatform()
    {
#if defined(__UNREAL__)
        return TEXT("unreal");
#elif defined(UNITY_EDITOR) || defined(UNITY_STANDALONE)
        return TEXT("unity");
#elif defined(GODOT_VERSION)
        return TEXT("godot");
#else
        return TEXT("unknown");
#endif
    }

    /** Get current platform version */
    static FSemanticVersion GetCurrentPlatformVersion()
    {
#if defined(ENGINE_MAJOR_VERSION) && defined(ENGINE_MINOR_VERSION) && defined(ENGINE_PATCH_VERSION)
        return FSemanticVersion(ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION, ENGINE_PATCH_VERSION);
#elif defined(ENGINE_MAJOR_VERSION) && defined(ENGINE_MINOR_VERSION)
        return FSemanticVersion(ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION, 0);
#else
        FString BuildVersion = FEngineVersion::Current().ToString();
        return FSemanticVersion::Parse(BuildVersion);
#endif
    }

    /** Check if content is for current platform */
    static bool IsForCurrentPlatform(const TArray<FString> &TargetPlatforms)
    {
        if (TargetPlatforms.Num() == 0)
        {
            return true;
        }

        if (TargetPlatforms.Contains(TEXT("all")))
        {
            return true;
        }

        FString CurrentPlatform = GetCurrentPlatform();
        return TargetPlatforms.Contains(CurrentPlatform);
    }

    /** Check if current version is within specified range */
    static bool IsVersionInRange(const FString &MinVersion, const FString &MaxVersion)
    {
        FSemanticVersion CurrentVersion = GetCurrentPlatformVersion();

        if (!CurrentVersion.IsValid())
        {
            return true;
        }

        if (!MinVersion.IsEmpty())
        {
            FSemanticVersion Min = FSemanticVersion::Parse(MinVersion);
            if (Min.IsValid() && CurrentVersion < Min)
            {
                return false;
            }
        }

        if (!MaxVersion.IsEmpty())
        {
            FSemanticVersion Max = FSemanticVersion::Parse(MaxVersion);
            if (Max.IsValid() && CurrentVersion > Max)
            {
                return false;
            }
        }

        return true;
    }
};

/** Interface for content filtering strategies */
class IContentFilter
{
public:
    virtual ~IContentFilter() = default;

    /** Test if an announcement passes this filter */
    virtual bool PassesFilter(const FConvaiAnnouncementItem &Item) const = 0;

    /** Test if a changelog passes this filter */
    virtual bool PassesFilter(const FConvaiChangelogItem &Item) const = 0;

    /** Get filter name */
    virtual FString GetFilterName() const = 0;
};

/** Filters content by target platform */
class FPlatformFilter : public IContentFilter
{
public:
    virtual bool PassesFilter(const FConvaiAnnouncementItem &Item) const override
    {
        return FPlatformInfo::IsForCurrentPlatform(Item.TargetPlatforms);
    }

    virtual bool PassesFilter(const FConvaiChangelogItem &Item) const override
    {
        return FPlatformInfo::IsForCurrentPlatform(Item.TargetPlatforms);
    }

    virtual FString GetFilterName() const override
    {
        return TEXT("PlatformFilter");
    }
};

/** Filters content by version range */
class FVersionRangeFilter : public IContentFilter
{
public:
    virtual bool PassesFilter(const FConvaiAnnouncementItem &Item) const override
    {
        return FPlatformInfo::IsVersionInRange(Item.MinVersion, Item.MaxVersion);
    }

    virtual bool PassesFilter(const FConvaiChangelogItem &Item) const override
    {
        return true;
    }

    virtual FString GetFilterName() const override
    {
        return TEXT("VersionRangeFilter");
    }
};

/** Filters content by tags */
class FTagFilter : public IContentFilter
{
public:
    TArray<FString> RequiredTags;

    FTagFilter(const TArray<FString> &InRequiredTags)
        : RequiredTags(InRequiredTags)
    {
    }

    virtual bool PassesFilter(const FConvaiAnnouncementItem &Item) const override
    {
        if (RequiredTags.Num() == 0)
        {
            return true;
        }

        for (const FString &Tag : RequiredTags)
        {
            if (Item.Tags.Contains(Tag))
            {
                return true;
            }
        }

        return false;
    }

    virtual bool PassesFilter(const FConvaiChangelogItem &Item) const override
    {
        return true;
    }

    virtual FString GetFilterName() const override
    {
        return TEXT("TagFilter");
    }
};

/** Chain of filters that applies all filters in sequence */
class FContentFilterChain : public IContentFilter
{
private:
    TArray<TSharedPtr<IContentFilter>> Filters;

public:
    /** Add a filter to the chain */
    void AddFilter(TSharedPtr<IContentFilter> Filter)
    {
        if (Filter.IsValid())
        {
            Filters.Add(Filter);
        }
    }

    /** Remove all filters */
    void ClearFilters()
    {
        Filters.Empty();
    }

    /** Get number of filters in chain */
    int32 GetFilterCount() const
    {
        return Filters.Num();
    }

    virtual bool PassesFilter(const FConvaiAnnouncementItem &Item) const override
    {
        for (const TSharedPtr<IContentFilter> &Filter : Filters)
        {
            if (Filter.IsValid() && !Filter->PassesFilter(Item))
            {
                return false;
            }
        }
        return true;
    }

    virtual bool PassesFilter(const FConvaiChangelogItem &Item) const override
    {
        for (const TSharedPtr<IContentFilter> &Filter : Filters)
        {
            if (Filter.IsValid() && !Filter->PassesFilter(Item))
            {
                return false;
            }
        }
        return true;
    }

    virtual FString GetFilterName() const override
    {
        TArray<FString> FilterNames;
        for (const TSharedPtr<IContentFilter> &Filter : Filters)
        {
            if (Filter.IsValid())
            {
                FilterNames.Add(Filter->GetFilterName());
            }
        }
        return FString::Printf(TEXT("FilterChain(%s)"), *FString::Join(FilterNames, TEXT(", ")));
    }
};

/** Main utility class for content filtering operations */
class FContentFilteringUtility
{
public:
    /** Filter announcements with default filters */
    static TArray<FConvaiAnnouncementItem> FilterAnnouncements(const TArray<FConvaiAnnouncementItem> &Items)
    {
        FContentFilterChain FilterChain;
        FilterChain.AddFilter(MakeShared<FPlatformFilter>());
        FilterChain.AddFilter(MakeShared<FVersionRangeFilter>());

        return FilterAnnouncementsWithChain(Items, FilterChain);
    }

    /** Filter announcements with custom filter chain */
    static TArray<FConvaiAnnouncementItem> FilterAnnouncementsWithChain(
        const TArray<FConvaiAnnouncementItem> &Items,
        const FContentFilterChain &FilterChain)
    {
        TArray<FConvaiAnnouncementItem> Filtered;

        for (const FConvaiAnnouncementItem &Item : Items)
        {
            if (FilterChain.PassesFilter(Item))
            {
                Filtered.Add(Item);
            }
        }

        return Filtered;
    }

    /** Filter changelogs with default filters */
    static TArray<FConvaiChangelogItem> FilterChangelogs(const TArray<FConvaiChangelogItem> &Items)
    {
        FContentFilterChain FilterChain;
        FilterChain.AddFilter(MakeShared<FPlatformFilter>());

        return FilterChangelogsWithChain(Items, FilterChain);
    }

    /** Filter changelogs with custom filter chain */
    static TArray<FConvaiChangelogItem> FilterChangelogsWithChain(
        const TArray<FConvaiChangelogItem> &Items,
        const FContentFilterChain &FilterChain)
    {
        TArray<FConvaiChangelogItem> Filtered;

        for (const FConvaiChangelogItem &Item : Items)
        {
            if (FilterChain.PassesFilter(Item))
            {
                Filtered.Add(Item);
            }
        }

        return Filtered;
    }

    /** Get current platform information */
    static FString GetPlatformInfoString()
    {
        FString Platform = FPlatformInfo::GetCurrentPlatform();
        FSemanticVersion Version = FPlatformInfo::GetCurrentPlatformVersion();

        return FString::Printf(TEXT("Platform: %s, Version: %s"), *Platform, *Version.ToString());
    }
};
