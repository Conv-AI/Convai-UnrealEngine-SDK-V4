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
#include "Logging/ConvaiEditorConfigLog.h"

bool FConvaiUserInfo::FromJson(const FString &JsonString, FConvaiUserInfo &OutUserInfo)
{
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        return false;
    }

    if (JsonObject->HasField(TEXT("username")))
    {
        OutUserInfo.Username = JsonObject->GetStringField(TEXT("username"));
    }

    if (JsonObject->HasField(TEXT("email")))
    {
        OutUserInfo.Email = JsonObject->GetStringField(TEXT("email"));
    }

    return OutUserInfo.IsValid();
}
