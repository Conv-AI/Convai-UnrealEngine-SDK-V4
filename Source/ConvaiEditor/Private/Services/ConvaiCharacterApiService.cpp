/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiCharacterApiService.cpp
 *
 * Implementation of character API service for Convai character management.
 */

#include "Services/ConvaiCharacterApiService.h"
#include "Json.h"
#include "Async/Async.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/ConfigurationService.h"
#include "ConvaiEditor.h"
#include "Async/HttpAsyncOperation.h"

FConvaiCharacterApiService::FConvaiCharacterApiService()
    : ApiKey(TEXT(""))
{
}

FConvaiCharacterApiService::FConvaiCharacterApiService(const FString &InApiKey)
    : ApiKey(InApiKey)
{
}

FConvaiCharacterApiService::~FConvaiCharacterApiService() {}

void FConvaiCharacterApiService::Startup()
{
    if (ApiKey.IsEmpty())
    {
        auto ConfigResult = FConvaiDIContainerManager::Get().Resolve<IConfigurationService>();
        if (ConfigResult.IsSuccess() && ConfigResult.GetValue().IsValid())
        {
            ApiKey = ConfigResult.GetValue()->GetApiKey();
        }
        else
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("ConvaiCharacterApiService: Failed to load API key"));
        }
    }

    ConvaiEditor::FCircuitBreakerConfig CircuitConfig;
    CircuitConfig.Name = TEXT("ConvaiCharacterAPI");
    CircuitConfig.FailureThreshold = 3;
    CircuitConfig.SuccessThreshold = 2;
    CircuitConfig.OpenTimeoutSeconds = 45.0f;
    CircuitConfig.bEnableLogging = false;
    CircuitBreaker = MakeShared<ConvaiEditor::FCircuitBreaker>(CircuitConfig);

    ConvaiEditor::FRetryPolicyConfig RetryConfig;
    RetryConfig.Name = TEXT("ConvaiCharacterAPI");
    RetryConfig.MaxAttempts = 2;
    RetryConfig.BaseDelaySeconds = 1.5f;
    RetryConfig.MaxDelaySeconds = 10.0f;
    RetryConfig.Strategy = ConvaiEditor::ERetryStrategy::Exponential;
    RetryConfig.bEnableJitter = true;
    RetryConfig.bEnableLogging = false;
    RetryConfig.ShouldRetryPredicate = ConvaiEditor::RetryPredicates::OnlyTransientErrors;
    RetryPolicy = MakeShared<ConvaiEditor::FRetryPolicy>(RetryConfig);
}

void FConvaiCharacterApiService::Shutdown()
{
    UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiCharacterApiService: Shutting down..."));

    // Clear cache
    InvalidateCache();

    // Reset circuit breaker and retry policy
    CircuitBreaker.Reset();
    RetryPolicy.Reset();

    UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiCharacterApiService: Shutdown complete"));
}

void FConvaiCharacterApiService::SetApiKey(const FString &InApiKey)
{
    ApiKey = InApiKey;
}

TFuture<TOptional<FConvaiCharacterMetadata>> FConvaiCharacterApiService::FetchCharacterMetadataAsync(const FString &CharacterID)
{
    {
        FScopeLock Lock(&CacheMutex);
        if (const FConvaiCharacterMetadata *Cached = MetadataCache.Find(CharacterID))
        {
            return Async(EAsyncExecution::TaskGraphMainThread, [=]()
                         { return TOptional<FConvaiCharacterMetadata>(*Cached); });
        }
    }

    if (CircuitBreaker && CircuitBreaker->IsOpen())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Character API temporarily unavailable - circuit breaker open"));
        return Async(EAsyncExecution::TaskGraphMainThread, []()
                     { return TOptional<FConvaiCharacterMetadata>(); });
    }

    TSharedPtr<FJsonObject> JsonPayload = MakeShared<FJsonObject>();
    JsonPayload->SetStringField(TEXT("charID"), CharacterID);
    FString PayloadStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PayloadStr);
    FJsonSerializer::Serialize(JsonPayload.ToSharedRef(), Writer);

    ConvaiEditor::FHttpAsyncRequest HttpRequest(TEXT("https://api.convai.com/character/get"));
    HttpRequest.WithVerb(TEXT("POST"))
        .WithHeader(TEXT("Content-Type"), TEXT("application/json"))
        .WithHeader(TEXT("CONVAI-API-KEY"), ApiKey)
        .WithBody(PayloadStr)
        .WithTimeout(30.0f);

    TSharedPtr<ConvaiEditor::FAsyncOperation<ConvaiEditor::FHttpAsyncResponse>> AsyncOp;

    if (CircuitBreaker.IsValid() && RetryPolicy.IsValid())
    {
        AsyncOp = ConvaiEditor::FHttpAsyncOperation::CreateWithProtection(
            HttpRequest,
            CircuitBreaker,
            RetryPolicy,
            nullptr);
    }
    else
    {
        AsyncOp = ConvaiEditor::FHttpAsyncOperation::Create(HttpRequest, nullptr);
    }

    TSharedPtr<TPromise<TOptional<FConvaiCharacterMetadata>>> Promise = MakeShared<TPromise<TOptional<FConvaiCharacterMetadata>>>();
    TFuture<TOptional<FConvaiCharacterMetadata>> Future = Promise->GetFuture();

    AsyncOp->OnComplete([this, CharacterID, Promise, AsyncOp](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
                        {
        if (!Result.IsSuccess())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("Character metadata request failed"));
            Promise->SetValue(TOptional<FConvaiCharacterMetadata>());
            return;
        }

        const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();
        
        if (!HttpResponse.IsSuccess())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("Failed to fetch character metadata. HTTP %d"), HttpResponse.ResponseCode);
            Promise->SetValue(TOptional<FConvaiCharacterMetadata>());
            return;
        }

        TSharedPtr<FJsonObject> JsonObject;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse.Body);
        
        if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("JSON parsing failed for character metadata response"));
            Promise->SetValue(TOptional<FConvaiCharacterMetadata>());
            return;
        }

        FString Name = JsonObject->GetStringField(TEXT("character_name"));
        bool bNarrative = JsonObject->GetBoolField(TEXT("is_narrative_driven"));
        bool bLTM = false;
        
        if (JsonObject->HasTypedField<EJson::Object>(TEXT("memory_settings")))
        {
            const TSharedPtr<FJsonObject>* MemorySettings;
            if (JsonObject->TryGetObjectField(TEXT("memory_settings"), MemorySettings) && 
                MemorySettings && (*MemorySettings)->HasField(TEXT("enabled")))
            {
                bLTM = (*MemorySettings)->GetBoolField(TEXT("enabled"));
            }
        }

        FConvaiCharacterMetadata Metadata(CharacterID, Name, bNarrative, bLTM);
        {
            FScopeLock Lock(&CacheMutex);
            MetadataCache.Add(CharacterID, Metadata);
        }

        Promise->SetValue(Metadata); });

    AsyncOp->Start();

    return Future;
}

void FConvaiCharacterApiService::InvalidateCache()
{
    FScopeLock Lock(&CacheMutex);
    MetadataCache.Empty();
}