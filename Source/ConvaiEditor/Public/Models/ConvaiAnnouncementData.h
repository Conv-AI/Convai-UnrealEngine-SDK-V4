/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiAnnouncementData.h
 *
 * Data models for announcements and changelogs.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

/** Announcement type enumeration. */
enum class EAnnouncementType : uint8
{
    YouTube,
    Blog,
    Update,
    Feature,
    News,
    Unknown
};

/** Data model for a single announcement item. */
struct FConvaiAnnouncementItem
{
    FString ID;
    EAnnouncementType Type;
    FString Title;
    FString Description;
    FString URL;
    FString ThumbnailURL;
    FDateTime Date;
    int32 Priority;
    TArray<FString> Tags;
    TArray<FString> TargetPlatforms;
    FString MinVersion;
    FString MaxVersion;

    FConvaiAnnouncementItem()
        : Type(EAnnouncementType::Unknown), Date(FDateTime::MinValue()), Priority(999)
    {
    }

    bool IsValid() const
    {
        return !ID.IsEmpty() && !Title.IsEmpty() && !URL.IsEmpty();
    }

    static EAnnouncementType ParseType(const FString &TypeString)
    {
        if (TypeString.Equals(TEXT("youtube"), ESearchCase::IgnoreCase))
            return EAnnouncementType::YouTube;
        if (TypeString.Equals(TEXT("blog"), ESearchCase::IgnoreCase))
            return EAnnouncementType::Blog;
        if (TypeString.Equals(TEXT("update"), ESearchCase::IgnoreCase))
            return EAnnouncementType::Update;
        if (TypeString.Equals(TEXT("feature"), ESearchCase::IgnoreCase))
            return EAnnouncementType::Feature;
        if (TypeString.Equals(TEXT("news"), ESearchCase::IgnoreCase))
            return EAnnouncementType::News;

        return EAnnouncementType::Unknown;
    }

    static FString TypeToString(EAnnouncementType Type)
    {
        switch (Type)
        {
        case EAnnouncementType::YouTube:
            return TEXT("youtube");
        case EAnnouncementType::Blog:
            return TEXT("blog");
        case EAnnouncementType::Update:
            return TEXT("update");
        case EAnnouncementType::Feature:
            return TEXT("feature");
        case EAnnouncementType::News:
            return TEXT("news");
        default:
            return TEXT("unknown");
        }
    }

    static FConvaiAnnouncementItem FromJson(const TSharedPtr<FJsonObject> &JsonObject)
    {
        if (!JsonObject.IsValid())
        {
            return FConvaiAnnouncementItem();
        }

        FConvaiAnnouncementItem Item;

        Item.ID = JsonObject->GetStringField(TEXT("id"));
        Item.Title = JsonObject->GetStringField(TEXT("title"));
        Item.URL = JsonObject->GetStringField(TEXT("url"));

        FString TypeString;
        if (JsonObject->TryGetStringField(TEXT("type"), TypeString))
        {
            Item.Type = ParseType(TypeString);
        }

        JsonObject->TryGetStringField(TEXT("description"), Item.Description);
        JsonObject->TryGetStringField(TEXT("thumbnailUrl"), Item.ThumbnailURL);
        JsonObject->TryGetNumberField(TEXT("priority"), Item.Priority);

        FString DateString;
        if (JsonObject->TryGetStringField(TEXT("date"), DateString))
        {
            FDateTime::ParseIso8601(*DateString, Item.Date);
        }

        const TArray<TSharedPtr<FJsonValue>> *TagsArray;
        if (JsonObject->TryGetArrayField(TEXT("tags"), TagsArray))
        {
            for (const auto &TagValue : *TagsArray)
            {
                FString Tag;
                if (TagValue->TryGetString(Tag))
                {
                    Item.Tags.Add(Tag);
                }
            }
        }

        const TArray<TSharedPtr<FJsonValue>> *PlatformsArray;
        if (JsonObject->TryGetArrayField(TEXT("targetPlatforms"), PlatformsArray))
        {
            for (const auto &PlatformValue : *PlatformsArray)
            {
                FString Platform;
                if (PlatformValue->TryGetString(Platform))
                {
                    Item.TargetPlatforms.Add(Platform);
                }
            }
        }

        JsonObject->TryGetStringField(TEXT("minVersion"), Item.MinVersion);
        JsonObject->TryGetStringField(TEXT("maxVersion"), Item.MaxVersion);

        return Item;
    }

    TSharedPtr<FJsonObject> ToJson() const
    {
        TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

        JsonObject->SetStringField(TEXT("id"), ID);
        JsonObject->SetStringField(TEXT("type"), TypeToString(Type));
        JsonObject->SetStringField(TEXT("title"), Title);
        JsonObject->SetStringField(TEXT("description"), Description);
        JsonObject->SetStringField(TEXT("url"), URL);
        JsonObject->SetStringField(TEXT("thumbnailUrl"), ThumbnailURL);
        JsonObject->SetStringField(TEXT("date"), Date.ToIso8601());
        JsonObject->SetNumberField(TEXT("priority"), Priority);

        TArray<TSharedPtr<FJsonValue>> TagsArray;
        for (const FString &Tag : Tags)
        {
            TagsArray.Add(MakeShared<FJsonValueString>(Tag));
        }
        JsonObject->SetArrayField(TEXT("tags"), TagsArray);

        TArray<TSharedPtr<FJsonValue>> PlatformsArray;
        for (const FString &Platform : TargetPlatforms)
        {
            PlatformsArray.Add(MakeShared<FJsonValueString>(Platform));
        }
        JsonObject->SetArrayField(TEXT("targetPlatforms"), PlatformsArray);

        if (!MinVersion.IsEmpty())
        {
            JsonObject->SetStringField(TEXT("minVersion"), MinVersion);
        }
        if (!MaxVersion.IsEmpty())
        {
            JsonObject->SetStringField(TEXT("maxVersion"), MaxVersion);
        }

        return JsonObject;
    }

    bool operator<(const FConvaiAnnouncementItem &Other) const
    {
        if (Priority != Other.Priority)
        {
            return Priority < Other.Priority;
        }
        return Date > Other.Date;
    }
};

/** Data model for complete announcement feed. */
struct FConvaiAnnouncementFeed
{
    FString Version;
    FDateTime LastUpdated;
    TArray<FConvaiAnnouncementItem> Announcements;

    FConvaiAnnouncementFeed()
        : Version(TEXT("1.0")), LastUpdated(FDateTime::MinValue())
    {
    }

    bool IsValid() const
    {
        if (LastUpdated == FDateTime::MinValue() || Announcements.Num() == 0)
        {
            return false;
        }

        for (const auto &Item : Announcements)
        {
            if (Item.IsValid())
            {
                return true;
            }
        }

        return false;
    }

    TArray<FConvaiAnnouncementItem> GetSortedAnnouncements() const
    {
        TArray<FConvaiAnnouncementItem> Sorted = Announcements;
        Sorted.Sort();
        return Sorted;
    }

    static FConvaiAnnouncementFeed FromJson(const TSharedPtr<FJsonObject> &JsonObject)
    {
        if (!JsonObject.IsValid())
        {
            return FConvaiAnnouncementFeed();
        }

        FConvaiAnnouncementFeed Feed;

        JsonObject->TryGetStringField(TEXT("version"), Feed.Version);

        FString UpdatedString;
        if (JsonObject->TryGetStringField(TEXT("lastUpdated"), UpdatedString))
        {
            FDateTime::ParseIso8601(*UpdatedString, Feed.LastUpdated);
        }

        const TArray<TSharedPtr<FJsonValue>> *AnnouncementsArray;
        if (JsonObject->TryGetArrayField(TEXT("announcements"), AnnouncementsArray))
        {
            for (const auto &Value : *AnnouncementsArray)
            {
                const TSharedPtr<FJsonObject> *ItemObject;
                if (Value->TryGetObject(ItemObject))
                {
                    FConvaiAnnouncementItem Item = FConvaiAnnouncementItem::FromJson(*ItemObject);
                    if (Item.IsValid())
                    {
                        Feed.Announcements.Add(Item);
                    }
                }
            }
        }

        return Feed;
    }

    TSharedPtr<FJsonObject> ToJson() const
    {
        TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

        JsonObject->SetStringField(TEXT("version"), Version);
        JsonObject->SetStringField(TEXT("lastUpdated"), LastUpdated.ToIso8601());

        TArray<TSharedPtr<FJsonValue>> AnnouncementsArray;
        for (const auto &Item : Announcements)
        {
            AnnouncementsArray.Add(MakeShared<FJsonValueObject>(Item.ToJson()));
        }
        JsonObject->SetArrayField(TEXT("announcements"), AnnouncementsArray);

        return JsonObject;
    }

    FString ToJsonString(bool bPrettyPrint = false) const
    {
        FString OutputString;
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutputString, 0);

        if (!bPrettyPrint)
        {
            TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> CondensedWriter = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
            FJsonSerializer::Serialize(ToJson().ToSharedRef(), CondensedWriter);
        }
        else
        {
            FJsonSerializer::Serialize(ToJson().ToSharedRef(), Writer);
        }

        return OutputString;
    }

    static FConvaiAnnouncementFeed FromJsonString(const FString &JsonString)
    {
        TSharedPtr<FJsonObject> JsonObject;
        TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonString);

        if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
        {
            return FromJson(JsonObject);
        }

        return FConvaiAnnouncementFeed();
    }
};

/** Data model for a single changelog entry. */
struct FConvaiChangelogItem
{
    FString ID;
    FString Version;
    FDateTime Date;
    TArray<FString> Changes;
    FString URL;
    TArray<FString> TargetPlatforms;

    FConvaiChangelogItem()
        : Date(FDateTime::MinValue())
    {
    }

    bool IsValid() const
    {
        return !ID.IsEmpty() && !Version.IsEmpty() && Changes.Num() > 0;
    }

    static FConvaiChangelogItem FromJson(const TSharedPtr<FJsonObject> &JsonObject)
    {
        if (!JsonObject.IsValid())
        {
            return FConvaiChangelogItem();
        }

        FConvaiChangelogItem Item;

        Item.ID = JsonObject->GetStringField(TEXT("id"));
        Item.Version = JsonObject->GetStringField(TEXT("version"));

        JsonObject->TryGetStringField(TEXT("url"), Item.URL);

        FString DateString;
        if (JsonObject->TryGetStringField(TEXT("date"), DateString))
        {
            FDateTime::ParseIso8601(*DateString, Item.Date);
        }

        const TArray<TSharedPtr<FJsonValue>> *ChangesArray;
        if (JsonObject->TryGetArrayField(TEXT("changes"), ChangesArray))
        {
            for (const auto &ChangeValue : *ChangesArray)
            {
                FString Change;
                if (ChangeValue->TryGetString(Change))
                {
                    Item.Changes.Add(Change);
                }
            }
        }

        const TArray<TSharedPtr<FJsonValue>> *PlatformsArray;
        if (JsonObject->TryGetArrayField(TEXT("targetPlatforms"), PlatformsArray))
        {
            for (const auto &PlatformValue : *PlatformsArray)
            {
                FString Platform;
                if (PlatformValue->TryGetString(Platform))
                {
                    Item.TargetPlatforms.Add(Platform);
                }
            }
        }

        return Item;
    }

    TSharedPtr<FJsonObject> ToJson() const
    {
        TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

        JsonObject->SetStringField(TEXT("id"), ID);
        JsonObject->SetStringField(TEXT("version"), Version);
        JsonObject->SetStringField(TEXT("date"), Date.ToIso8601());
        JsonObject->SetStringField(TEXT("url"), URL);

        TArray<TSharedPtr<FJsonValue>> ChangesArray;
        for (const FString &Change : Changes)
        {
            ChangesArray.Add(MakeShared<FJsonValueString>(Change));
        }
        JsonObject->SetArrayField(TEXT("changes"), ChangesArray);

        TArray<TSharedPtr<FJsonValue>> PlatformsArray;
        for (const FString &Platform : TargetPlatforms)
        {
            PlatformsArray.Add(MakeShared<FJsonValueString>(Platform));
        }
        JsonObject->SetArrayField(TEXT("targetPlatforms"), PlatformsArray);

        return JsonObject;
    }

    bool operator<(const FConvaiChangelogItem &Other) const
    {
        return Date > Other.Date;
    }
};

/** Data model for complete changelog feed. */
struct FConvaiChangelogFeed
{
    FString Version;
    FDateTime LastUpdated;
    TArray<FConvaiChangelogItem> Changelogs;

    FConvaiChangelogFeed()
        : Version(TEXT("1.0")), LastUpdated(FDateTime::MinValue())
    {
    }

    bool IsValid() const
    {
        if (LastUpdated == FDateTime::MinValue() || Changelogs.Num() == 0)
        {
            return false;
        }

        for (const auto &Item : Changelogs)
        {
            if (Item.IsValid())
            {
                return true;
            }
        }

        return false;
    }

    TArray<FConvaiChangelogItem> GetSortedChangelogs() const
    {
        TArray<FConvaiChangelogItem> Sorted = Changelogs;
        Sorted.Sort();
        return Sorted;
    }

    static FConvaiChangelogFeed FromJson(const TSharedPtr<FJsonObject> &JsonObject)
    {
        if (!JsonObject.IsValid())
        {
            return FConvaiChangelogFeed();
        }

        FConvaiChangelogFeed Feed;

        JsonObject->TryGetStringField(TEXT("version"), Feed.Version);

        FString UpdatedString;
        if (JsonObject->TryGetStringField(TEXT("lastUpdated"), UpdatedString))
        {
            FDateTime::ParseIso8601(*UpdatedString, Feed.LastUpdated);
        }

        const TArray<TSharedPtr<FJsonValue>> *ChangelogsArray;
        if (JsonObject->TryGetArrayField(TEXT("changelogs"), ChangelogsArray))
        {
            for (const auto &Value : *ChangelogsArray)
            {
                const TSharedPtr<FJsonObject> *ItemObject;
                if (Value->TryGetObject(ItemObject))
                {
                    FConvaiChangelogItem Item = FConvaiChangelogItem::FromJson(*ItemObject);
                    if (Item.IsValid())
                    {
                        Feed.Changelogs.Add(Item);
                    }
                }
            }
        }

        return Feed;
    }

    TSharedPtr<FJsonObject> ToJson() const
    {
        TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

        JsonObject->SetStringField(TEXT("version"), Version);
        JsonObject->SetStringField(TEXT("lastUpdated"), LastUpdated.ToIso8601());

        TArray<TSharedPtr<FJsonValue>> ChangelogsArray;
        for (const auto &Item : Changelogs)
        {
            ChangelogsArray.Add(MakeShared<FJsonValueObject>(Item.ToJson()));
        }
        JsonObject->SetArrayField(TEXT("changelogs"), ChangelogsArray);

        return JsonObject;
    }

    FString ToJsonString(bool bPrettyPrint = false) const
    {
        FString OutputString;
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutputString, 0);

        if (!bPrettyPrint)
        {
            TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> CondensedWriter = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
            FJsonSerializer::Serialize(ToJson().ToSharedRef(), CondensedWriter);
        }
        else
        {
            FJsonSerializer::Serialize(ToJson().ToSharedRef(), Writer);
        }

        return OutputString;
    }

    static FConvaiChangelogFeed FromJsonString(const FString &JsonString)
    {
        TSharedPtr<FJsonObject> JsonObject;
        TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonString);

        if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
        {
            return FromJson(JsonObject);
        }

        return FConvaiChangelogFeed();
    }
};
