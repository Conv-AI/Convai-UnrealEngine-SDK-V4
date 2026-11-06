
#include "RestAPI/ConvaiLTMProxy.h"
#include "RestAPI/ConvaiURL.h"
#include "ConvaiDefinitions.h"
#include "Utility/Log/ConvaiLogger.h"

DEFINE_LOG_CATEGORY(LTMHttpLogs);

UConvaiCreateSpeakerID* UConvaiCreateSpeakerID::ConvaiCreateSpeakerIDProxy(FString SpeakerName, FString DeviceId, UObject* WorldContextObject)
{
    UConvaiCreateSpeakerID* Proxy = NewObject<UConvaiCreateSpeakerID>();
    Proxy->SetAPIRequestWorldContext(WorldContextObject);
    Proxy->URL = UConvaiURL::GetEndpoint(EConvaiEndpoint::NewSpeaker);
    Proxy->AssociatedSpeakerName = SpeakerName;
    Proxy->AssociatedDeviceId = DeviceId;
    return Proxy;
}

bool UConvaiCreateSpeakerID::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
    if (!Super::ConfigureRequest(Request, ConvaiHttpConstants::POST))
    {
        return false;
    }

    return true;
}

bool UConvaiCreateSpeakerID::AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend)
{
    if (AssociatedSpeakerName.IsEmpty())
    {
        CONVAI_LOG(LTMHttpLogs, Error, TEXT("Speaker name is empty"));
        MarkAPIRequestConfigurationFailure();
        return false;
    }

    ObjectToSend->SetStringField(TEXT("name"), AssociatedSpeakerName);

    if (!AssociatedDeviceId.IsEmpty())
    {
        ObjectToSend->SetStringField(TEXT("deviceId"), AssociatedDeviceId);
    }

    return true;
}

bool UConvaiCreateSpeakerID::ValidateAPIResponse()
{
    TSharedPtr<FJsonObject> JsonObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid()) return false;
    JsonObject->TryGetStringField(TEXT("speaker_id"), AssociatedSpeakerInfo.SpeakerID);
    JsonObject->TryGetStringField(TEXT("name"), AssociatedSpeakerInfo.Name);
    JsonObject->TryGetStringField(TEXT("device_id"), AssociatedSpeakerInfo.DeviceID);
    return true;
}

void UConvaiCreateSpeakerID::HandleSuccess()
{
    Super::HandleSuccess();
    OnSuccess.Broadcast(AssociatedSpeakerInfo);
}

void UConvaiCreateSpeakerID::HandleFailure()
{
    Super::HandleFailure();
    OnFailure.Broadcast(FConvaiSpeakerInfo());
}





UConvaiListSpeakerID* UConvaiListSpeakerID::ConvaiListSpeakerIDProxy(UObject* WorldContextObject)
{
    UConvaiListSpeakerID* Proxy = NewObject<UConvaiListSpeakerID>();
    Proxy->SetAPIRequestWorldContext(WorldContextObject);
    Proxy->URL = UConvaiURL::GetEndpoint(EConvaiEndpoint::SpeakerIDList);
    return Proxy;
}

bool UConvaiListSpeakerID::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
    if (!Super::ConfigureRequest(Request, ConvaiHttpConstants::POST))
    {
        return false;
    }

    return true;
}

bool UConvaiListSpeakerID::ValidateAPIResponse()
{
    return UConvaiLTMUtils::ParseConvaiSpeakerInfoArray(ResponseString, ParsedSpeakerInfoArray);
}

void UConvaiListSpeakerID::HandleSuccess()
{
    Super::HandleSuccess();
    OnSuccess.Broadcast(ParsedSpeakerInfoArray);
}

void UConvaiListSpeakerID::HandleFailure()
{
    Super::HandleFailure();
    OnFailure.Broadcast(TArray<FConvaiSpeakerInfo>());
}





UConvaiDeleteSpeakerID* UConvaiDeleteSpeakerID::ConvaiDeleteSpeakerIDProxy(FString SpeakerID, UObject* WorldContextObject)
{
    UConvaiDeleteSpeakerID* Proxy = NewObject<UConvaiDeleteSpeakerID>();
    Proxy->SetAPIRequestWorldContext(WorldContextObject);
    Proxy->URL = UConvaiURL::GetEndpoint(EConvaiEndpoint::DeleteSpeakerID);
    Proxy->AssociatedSpeakerID = SpeakerID;
    return Proxy;
}

bool UConvaiDeleteSpeakerID::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
    if (!Super::ConfigureRequest(Request, ConvaiHttpConstants::POST))
    {
        return false;
    }

    return true;
}

bool UConvaiDeleteSpeakerID::AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend)
{
    if (AssociatedSpeakerID.IsEmpty())
    {
        MarkAPIRequestConfigurationFailure();
        return false;
    }

    ObjectToSend->SetStringField(TEXT("speakerId"), AssociatedSpeakerID);

    return true;
}

void UConvaiDeleteSpeakerID::HandleSuccess()
{
    Super::HandleSuccess();
    OnSuccess.Broadcast(ResponseString);
}

void UConvaiDeleteSpeakerID::HandleFailure()
{
    Super::HandleFailure();
    OnFailure.Broadcast(TEXT("Http req failed"));
}





UConvaiGetLTMStatus* UConvaiGetLTMStatus::ConvaiGetLTMStatusProxy(FString CharacterID, UObject* WorldContextObject)
{
    UConvaiGetLTMStatus* Proxy = NewObject<UConvaiGetLTMStatus>();
    Proxy->SetAPIRequestWorldContext(WorldContextObject);
    Proxy->URL = UConvaiURL::GetEndpoint(EConvaiEndpoint::CharacterGet);
    Proxy->AssociatedCharacterID = CharacterID;
    return Proxy;
}

bool UConvaiGetLTMStatus::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
    if (!Super::ConfigureRequest(Request, ConvaiHttpConstants::POST))
    {
        return false;
    }

    return true;
}

bool UConvaiGetLTMStatus::AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend)
{
    if (AssociatedCharacterID.IsEmpty())
    {
        MarkAPIRequestConfigurationFailure();
        return false;
    }

    ObjectToSend->SetStringField(TEXT("charID"), AssociatedCharacterID);
    
    return true;
}

bool UConvaiGetLTMStatus::ValidateAPIResponse()
{
    return UConvaiLTMUtils::GetLTMStatus(ResponseString, bParsedLTMEnabled);
}

void UConvaiGetLTMStatus::HandleSuccess()
{
    Super::HandleSuccess();
    OnSuccess.Broadcast(bParsedLTMEnabled);
}

void UConvaiGetLTMStatus::HandleFailure()
{
    Super::HandleFailure();
    OnFailure.Broadcast(false);
}





UConvaiSetLTMStatus* UConvaiSetLTMStatus::ConvaiSetLTMStatusProxy(FString CharacterID, bool bEnable, UObject* WorldContextObject)
{

    UConvaiSetLTMStatus* Proxy = NewObject<UConvaiSetLTMStatus>();
    Proxy->SetAPIRequestWorldContext(WorldContextObject);
    Proxy->URL = UConvaiURL::GetEndpoint(EConvaiEndpoint::CharacterUpdate);
    Proxy->AssociatedCharacterID = CharacterID;
    Proxy->bAssociatedEnable = bEnable;
    return Proxy;
}

bool UConvaiSetLTMStatus::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
    if (!Super::ConfigureRequest(Request, ConvaiHttpConstants::POST))
    {
        return false;
    }

    return true;
}

bool UConvaiSetLTMStatus::AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend)
{
    if (AssociatedCharacterID.IsEmpty())
    {
        MarkAPIRequestConfigurationFailure();
        return false;
    }

    ObjectToSend->SetStringField(TEXT("charID"), AssociatedCharacterID);

    TSharedPtr<FJsonObject> MemorySettings = MakeShared<FJsonObject>();
    MemorySettings->SetBoolField(TEXT("enabled"), bAssociatedEnable);

    ObjectToSend->SetObjectField(TEXT("memorySettings"), MemorySettings);

    return true;
}

void UConvaiSetLTMStatus::HandleSuccess()
{
    Super::HandleSuccess();
    OnSuccess.Broadcast(ResponseString);
}

void UConvaiSetLTMStatus::HandleFailure()
{
    Super::HandleFailure();
    OnFailure.Broadcast(FailureResponseBody);
}





bool UConvaiLTMUtils::ParseConvaiSpeakerInfoArray(const FString& JsonString, TArray<FConvaiSpeakerInfo>& OutSpeakerInfoArray)
{
    OutSpeakerInfoArray.Empty();

    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    TArray<TSharedPtr<FJsonValue>> JsonArray;

    if (FJsonSerializer::Deserialize(Reader, JsonArray))
    {
        int32 SkippedEntries = 0;
        for (const TSharedPtr<FJsonValue>& JsonValue : JsonArray)
        {
            const TSharedPtr<FJsonObject>* JsonObject = nullptr;
            if (!JsonValue->TryGetObject(JsonObject))
            {
                ++SkippedEntries;
                continue;
            }

            FConvaiSpeakerInfo SpeakerInfo;
            (*JsonObject)->TryGetStringField(TEXT("speaker_id"), SpeakerInfo.SpeakerID);
            (*JsonObject)->TryGetStringField(TEXT("name"), SpeakerInfo.Name);
            (*JsonObject)->TryGetStringField(TEXT("device_id"), SpeakerInfo.DeviceID);

            OutSpeakerInfoArray.Add(SpeakerInfo);
        }

        if (SkippedEntries > 0)
        {
            CONVAI_LOG(LTMHttpLogs, Warning, TEXT("Skipped %d speaker entries that are not JSON objects"), SkippedEntries);
        }
        return true;
    }

    return false;
}

bool UConvaiLTMUtils::GetLTMStatus(const FString& JsonString, bool& bEnabled)
{
    bEnabled = false;

    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    TSharedPtr<FJsonObject> JsonObject;

    if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
    {
        if (JsonObject->HasField(TEXT("memory_settings")))
        {
            TSharedPtr<FJsonObject> MemorySettingsObject = JsonObject->GetObjectField(TEXT("memory_settings"));
            if (MemorySettingsObject.IsValid() && MemorySettingsObject->HasField(TEXT("enabled")))
            {
                bEnabled = MemorySettingsObject->GetBoolField(TEXT("enabled"));
                return true;
            }
        }
    }

    return false;
}

