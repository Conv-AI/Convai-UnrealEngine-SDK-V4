/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * DecryptionService.cpp
 *
 * Implementation of server-side decryption service.
 */

#include "Services/OAuth/DecryptionService.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "Misc/ScopeLock.h"
#include "Async/Async.h"
#include "HAL/PlatformTime.h"
#include "Async/HttpAsyncOperation.h"

FDecryptionService::FDecryptionService()
    : RequestCounter(0), bIsShuttingDown(false)
{
}

FDecryptionService::~FDecryptionService()
{
    Shutdown();
}

void FDecryptionService::Startup()
{
    Config.bVerboseLogging = false;
    bIsShuttingDown = false;

    ConvaiEditor::FCircuitBreakerConfig CircuitConfig;
    CircuitConfig.Name = TEXT("DecryptionService");
    CircuitConfig.FailureThreshold = 3;
    CircuitConfig.SuccessThreshold = 2;
    CircuitConfig.OpenTimeoutSeconds = 30.0f;
    CircuitConfig.bEnableLogging = false;
    CircuitBreaker = MakeShared<ConvaiEditor::FCircuitBreaker>(CircuitConfig);

    ConvaiEditor::FRetryPolicyConfig RetryConfig;
    RetryConfig.Name = TEXT("DecryptionService");
    RetryConfig.MaxAttempts = 3;
    RetryConfig.BaseDelaySeconds = 1.0f;
    RetryConfig.MaxDelaySeconds = 10.0f;
    RetryConfig.Strategy = ConvaiEditor::ERetryStrategy::Exponential;
    RetryConfig.bEnableJitter = true;
    RetryConfig.bEnableLogging = false;
    RetryConfig.ShouldRetryPredicate = ConvaiEditor::RetryPredicates::OnlyTransientErrors;
    RetryPolicy = MakeShared<ConvaiEditor::FRetryPolicy>(RetryConfig);
}

void FDecryptionService::Shutdown()
{
    bIsShuttingDown = true;
    CancelPendingRequests();

    // Force immediate cleanup without waiting
    FScopeLock Lock(&RequestLock);
    ActiveRequests.Empty();

    // Reset shared pointers to allow proper cleanup
    CircuitBreaker.Reset();
    RetryPolicy.Reset();
}

void FDecryptionService::DecryptAsync(
    const FString &EncryptedData,
    TFunction<void(const FString &)> OnSuccess,
    TFunction<void(const FString &)> OnFailure)
{
    if (bIsShuttingDown)
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("DecryptionService: cannot process request during shutdown"));
        if (OnFailure)
        {
            OnFailure(TEXT("Service is shutting down"));
        }
        return;
    }

    FString ValidationError;
    if (!ValidateEncryptedData(EncryptedData, ValidationError))
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: validation failed"));
        if (OnFailure)
        {
            OnFailure(ValidationError);
        }
        return;
    }

    TSharedPtr<FDecryptionRequest> RequestContext = MakeShared<FDecryptionRequest>();
    RequestContext->EncryptedData = EncryptedData;
    RequestContext->OnSuccess = OnSuccess;
    RequestContext->OnFailure = OnFailure;
    RequestContext->RetryCount = 0;
    RequestContext->RequestStartTime = FDateTime::UtcNow();

    const FString RequestId = GenerateRequestId();

    ExecuteDecryptionRequest(RequestContext);
}

TConvaiResult<FString> FDecryptionService::Decrypt(const FString &EncryptedData)
{
    UE_LOG(LogConvaiEditor, Warning, TEXT("DecryptionService: synchronous Decrypt() called - this will block the thread"));

    FString ValidationError;
    if (!ValidateEncryptedData(EncryptedData, ValidationError))
    {
        return TConvaiResult<FString>::Failure(ValidationError);
    }

    bool bCompleted = false;
    TConvaiResult<FString> Result = TConvaiResult<FString>::Failure(TEXT("Request timed out"));

    DecryptAsync(
        EncryptedData,
        [&Result, &bCompleted](const FString &DecryptedData)
        {
            Result = TConvaiResult<FString>::Success(DecryptedData);
            bCompleted = true;
        },
        [&Result, &bCompleted](const FString &ErrorMessage)
        {
            Result = TConvaiResult<FString>::Failure(ErrorMessage);
            bCompleted = true;
        });

    const double StartTime = FPlatformTime::Seconds();
    const double TimeoutSeconds = Config.TimeoutSeconds;
    const int32 MaxIterations = static_cast<int32>(TimeoutSeconds * 10); // 0.1s sleep intervals
    int32 IterationCount = 0;

    while (!bCompleted)
    {
        // Check for engine shutdown
        if (IsEngineExitRequested())
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("DecryptionService: synchronous request cancelled due to engine shutdown"));
            return TConvaiResult<FString>::Failure(TEXT("Request cancelled due to engine shutdown"));
        }

        if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: synchronous request timed out"));
            return TConvaiResult<FString>::Failure(TEXT("Request timed out"));
        }

        // Safety check: prevent infinite loops
        if (++IterationCount >= MaxIterations)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: exceeded maximum wait iterations"));
            return TConvaiResult<FString>::Failure(TEXT("Request exceeded maximum wait time"));
        }

        FPlatformProcess::Sleep(0.1f);
    }

    return Result;
}

void FDecryptionService::SetConfig(const FDecryptionConfig &NewConfig)
{
    FScopeLock Lock(&RequestLock);
    Config = NewConfig;
}

IDecryptionService::FDecryptionConfig FDecryptionService::GetConfig() const
{
    FScopeLock Lock(&RequestLock);
    return Config;
}

bool FDecryptionService::IsProcessing() const
{
    FScopeLock Lock(&RequestLock);
    return ActiveRequests.Num() > 0;
}

void FDecryptionService::CancelPendingRequests()
{
    FScopeLock Lock(&RequestLock);

    for (TSharedPtr<FDecryptionRequest> &Request : ActiveRequests)
    {
        if (Request.IsValid() && Request->HttpRequest.IsValid())
        {
            Request->HttpRequest->CancelRequest();
        }

        if (Request.IsValid() && Request->OnFailure)
        {
            Request->OnFailure(TEXT("Request cancelled by user"));
        }
    }

    ActiveRequests.Empty();
}

void FDecryptionService::ExecuteDecryptionRequest(TSharedPtr<FDecryptionRequest> RequestContext)
{
    if (!RequestContext.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: invalid request context"));
        return;
    }

    TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
    JsonObject->SetStringField(TEXT("data"), RequestContext->EncryptedData);

    FString JsonPayload;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonPayload);
    if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer))
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: failed to serialize JSON payload"));
        if (RequestContext->OnFailure)
        {
            RequestContext->OnFailure(TEXT("Failed to create request payload"));
        }
        return;
    }

    ConvaiEditor::FHttpAsyncRequest HttpRequest(Config.EndpointUrl);
    HttpRequest.WithVerb(TEXT("POST"))
        .WithHeader(TEXT("Content-Type"), TEXT("application/json"))
        .WithHeader(TEXT("Accept"), TEXT("application/json"))
        .WithBody(JsonPayload)
        .WithTimeout(Config.TimeoutSeconds);

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

    {
        FScopeLock Lock(&RequestLock);
        ActiveRequests.Add(RequestContext);
    }
    AsyncOp->OnComplete([this, RequestContext, AsyncOp](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
                        {
        {
            FScopeLock Lock(&RequestLock);
            ActiveRequests.Remove(RequestContext);
        }

        if (!RequestContext.IsValid())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: invalid request context in completion"));
            return;
        }

        const FTimespan ElapsedTime = FDateTime::UtcNow() - RequestContext->RequestStartTime;

        if (bIsShuttingDown)
        {
            if (RequestContext->OnFailure)
            {
                RequestContext->OnFailure(TEXT("Service shutting down"));
            }
            return;
        }

        if (!Result.IsSuccess())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: HTTP request failed"));
            
            if (RequestContext->OnFailure)
            {
                RequestContext->OnFailure(Result.GetError());
            }
            return;
        }

        const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();

        if (HttpResponse.ResponseCode < 200 || HttpResponse.ResponseCode >= 300)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: server returned error HTTP %d"), HttpResponse.ResponseCode);
            
            if (RequestContext->OnFailure)
            {
                RequestContext->OnFailure(FString::Printf(TEXT("Server returned error: HTTP %d - %s"), 
                                                          HttpResponse.ResponseCode, *HttpResponse.Body));
            }
            return;
        }

        FString DecryptedData;
        FString ParseError;

        if (!ParseDecryptionResponse(HttpResponse.Body, DecryptedData, ParseError))
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: failed to parse response"));
            
            if (RequestContext->OnFailure)
            {
                RequestContext->OnFailure(FString::Printf(TEXT("Failed to parse server response: %s"), *ParseError));
            }
            return;
        }

        if (RequestContext->OnSuccess)
        {
            AsyncTask(ENamedThreads::GameThread, [RequestContext, DecryptedData]()
            {
                RequestContext->OnSuccess(DecryptedData);
            });
        } });

    AsyncOp->Start();
}

bool FDecryptionService::ParseDecryptionResponse(const FString &ResponseBody, FString &OutDecryptedData, FString &OutError)
{
    if (!ResponseBody.IsEmpty() && !ResponseBody.StartsWith(TEXT("{")))
    {
        OutDecryptedData = ResponseBody;
        return true;
    }

    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);

    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: invalid JSON response"));
        OutError = TEXT("Invalid JSON response");
        return false;
    }

    if (JsonObject->HasField(TEXT("error")))
    {
        OutError = JsonObject->GetStringField(TEXT("error"));
        UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: server returned error"));
        return false;
    }

    const TArray<FString> PossibleFieldNames = {
        TEXT("decryptedData"),
        TEXT("decrypted_data"),
        TEXT("data"),
        TEXT("decrypted"),
        TEXT("result"),
        TEXT("api_key"),
        TEXT("apiKey"),
        TEXT("key")};

    for (const FString &FieldName : PossibleFieldNames)
    {
        if (JsonObject->HasField(FieldName))
        {
            OutDecryptedData = JsonObject->GetStringField(FieldName);
            if (!OutDecryptedData.IsEmpty())
            {
                return true;
            }
        }
    }

    UE_LOG(LogConvaiEditor, Error, TEXT("DecryptionService: could not find decrypted data in any expected field"));

    TArray<FString> FieldNames;
    for (const auto &Field : JsonObject->Values)
    {
        FieldNames.Add(Field.Key);
    }

    OutError = FString::Printf(TEXT("Missing decrypted data field. Available fields: %s"), *FString::Join(FieldNames, TEXT(", ")));

    return false;
}

bool FDecryptionService::ValidateEncryptedData(const FString &EncryptedData, FString &OutError)
{
    if (EncryptedData.IsEmpty())
    {
        OutError = TEXT("Encrypted data is empty");
        return false;
    }

    const FRegexPattern Base64Pattern(TEXT("^[A-Za-z0-9+/]+=*$"));
    FRegexMatcher Matcher(Base64Pattern, EncryptedData);

    if (!Matcher.FindNext())
    {
        OutError = TEXT("Encrypted data does not appear to be valid Base64");
        return false;
    }

    if (EncryptedData.Len() < 10)
    {
        OutError = TEXT("Encrypted data is too short");
        return false;
    }

    if (EncryptedData.Len() > 10000)
    {
        OutError = TEXT("Encrypted data exceeds maximum length");
        return false;
    }

    return true;
}

FString FDecryptionService::GenerateRequestId() const
{
    const int32 CurrentCount = RequestCounter.IncrementExchange();
    return FString::Printf(TEXT("REQ-%d-%lld"), CurrentCount, FDateTime::UtcNow().ToUnixTimestamp());
}

FString FDecryptionService::MaskSensitiveData(const FString &Data)
{
    if (Data.IsEmpty())
    {
        return TEXT("<empty>");
    }

    if (Data.Len() <= 8)
    {
        return TEXT("****");
    }

    const FString FirstPart = Data.Left(4);
    const FString LastPart = Data.Right(4);
    return FString::Printf(TEXT("%s...%s"), *FirstPart, *LastPart);
}
