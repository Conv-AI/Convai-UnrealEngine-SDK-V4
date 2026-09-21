/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiUserInfo.cpp
 *
 * Implementation of user information model.
 */

#include "Models/ConvaiUserInfo.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

bool FConvaiUserInfo::FromJson(const FString &JsonString, FConvaiUserInfo &OutUserInfo)
{
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        return false;
    }

    auto GetFirstNonEmptyStringField = [](const TSharedPtr<FJsonObject> &Source, const TArray<FString> &FieldNames) -> FString
    {
        if (!Source.IsValid())
        {
            return TEXT("");
        }

        for (const FString &FieldName : FieldNames)
        {
            FString Value;
            if (Source->TryGetStringField(FieldName, Value) && !Value.IsEmpty())
            {
                return Value;
            }
        }

        return TEXT("");
    };

    TSharedPtr<FJsonObject> CandidateObject = JsonObject;
    TSharedPtr<FJsonObject> NestedUserObject;
    if (JsonObject->HasTypedField<EJson::Object>(TEXT("user")))
    {
        NestedUserObject = JsonObject->GetObjectField(TEXT("user"));
        if (NestedUserObject.IsValid())
        {
            CandidateObject = NestedUserObject;
        }
    }

    const TArray<FString> UsernameFields = {
        TEXT("username"),
        TEXT("user_name"),
        TEXT("display_name"),
        TEXT("name"),
        TEXT("full_name")};

    const TArray<FString> EmailFields = {
        TEXT("email"),
        TEXT("user_email"),
        TEXT("mail")};

    OutUserInfo.Username = GetFirstNonEmptyStringField(CandidateObject, UsernameFields);
    OutUserInfo.Email = GetFirstNonEmptyStringField(CandidateObject, EmailFields);

    if (OutUserInfo.Username.IsEmpty())
    {
        OutUserInfo.Username = GetFirstNonEmptyStringField(JsonObject, UsernameFields);
    }

    if (OutUserInfo.Email.IsEmpty())
    {
        OutUserInfo.Email = GetFirstNonEmptyStringField(JsonObject, EmailFields);
    }

    if (OutUserInfo.Username.IsEmpty() && !OutUserInfo.Email.IsEmpty())
    {
        FString LocalPart;
        if (OutUserInfo.Email.Split(TEXT("@"), &LocalPart, nullptr) && !LocalPart.IsEmpty())
        {
            OutUserInfo.Username = LocalPart;
        }
    }

    return OutUserInfo.IsValid();
}
