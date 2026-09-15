/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ApiValidationService.cpp
 *
 * Implementation of API key validation service.
 */

#include "Services/ApiValidationService.h"
#include "Services/Configuration/IAuthProvider.h"
#include "Utility/ConvaiValidationUtils.h"
#include "ConvaiEditor.h"
#include "Utility/ConvaiURLs.h"
#include "Async/HttpAsyncOperation.h"
#include "Async/Async.h"

FApiValidationService::FApiValidationService()
    : bIsValidatingApiKey(false), bIsValidatingAuthToken(false), bIsValidatingAuthentication(false)
{
}

FApiValidationService::~FApiValidationService()
{
    Shutdown();
}

void FApiValidationService::Startup()
{
    ConvaiEditor::FCircuitBreakerConfig CircuitConfig;
    CircuitConfig.Name = TEXT("ConvaiAPIValidation");
    CircuitConfig.FailureThreshold = 3;
    CircuitConfig.SuccessThreshold = 2;
    CircuitConfig.OpenTimeoutSeconds = 45.0f;
    CircuitConfig.bEnableLogging = false;
    CircuitBreaker = MakeShared<ConvaiEditor::FCircuitBreaker>(CircuitConfig);

    ConvaiEditor::FRetryPolicyConfig RetryConfig;
    RetryConfig.Name = TEXT("ConvaiAPIValidation");
    RetryConfig.MaxAttempts = 2;
    RetryConfig.BaseDelaySeconds = 1.5f;
    RetryConfig.MaxDelaySeconds = 10.0f;
    RetryConfig.Strategy = ConvaiEditor::ERetryStrategy::Exponential;
    RetryConfig.bEnableJitter = true;
    RetryConfig.bEnableLogging = false;
    RetryConfig.ShouldRetryPredicate = ConvaiEditor::RetryPredicates::OnlyTransientErrors;
    RetryPolicy = MakeShared<ConvaiEditor::FRetryPolicy>(RetryConfig);

    SetupCacheCleanupTimer();
}

void FApiValidationService::Shutdown()
{
    if (ApiKeyDebounceTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(ApiKeyDebounceTicker);
        ApiKeyDebounceTicker.Reset();
    }
    if (AuthTokenDebounceTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(AuthTokenDebounceTicker);
        AuthTokenDebounceTicker.Reset();
    }
    if (AuthenticationDebounceTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(AuthenticationDebounceTicker);
        AuthenticationDebounceTicker.Reset();
    }
    if (CacheCleanupTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(CacheCleanupTicker);
        CacheCleanupTicker.Reset();
    }

    ClearCache();
}

void FApiValidationService::ValidateApiKey(const FString &ApiKey, bool bForceValidation)
{
    if (!IsValidApiKeyFormat(ApiKey))
    {
        FApiValidationResult Result(false, EApiValidationError::InvalidFormat, 0, TEXT("Invalid API key format"));
        OnApiKeyValidationResultDetailedDelegate.Broadcast(Result);
        ApiKeyValidationCache.Add(ApiKey, FValidationCacheEntry(Result));
        return;
    }

    if (!bForceValidation)
    {
        TOptional<FApiValidationResult> CachedResult = GetLastApiKeyValidationResultDetailed(ApiKey);
        if (CachedResult.IsSet())
        {
            const FApiValidationResult &Result = CachedResult.GetValue();
            OnApiKeyValidationResultDetailedDelegate.Broadcast(Result);
            return;
        }
    }

    SetupCacheCleanupTimer();

    if (ApiKeyDebounceTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(ApiKeyDebounceTicker);
    }

    ApiKeyDebounceTicker = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([this, ApiKey](float DeltaTime) -> bool
                                      {
                                          this->PerformApiKeyValidation(ApiKey);
                                          return false; }),
        ConvaiEditor::Constants::ApiKeyValidationDebounceTime);
}

bool FApiValidationService::IsValidatingApiKey() const
{
    return bIsValidatingApiKey;
}

TOptional<bool> FApiValidationService::GetLastApiKeyValidationResult(const FString &ApiKey) const
{
    const FValidationCacheEntry *CachedEntry = ApiKeyValidationCache.Find(ApiKey);
    if (CachedEntry && !CachedEntry->IsExpired())
    {
        return CachedEntry->bIsValid;
    }
    return TOptional<bool>();
}

TOptional<FApiValidationResult> FApiValidationService::GetLastApiKeyValidationResultDetailed(const FString &ApiKey) const
{
    const FValidationCacheEntry *CachedEntry = ApiKeyValidationCache.Find(ApiKey);
    if (CachedEntry && !CachedEntry->IsExpired())
    {
        return CachedEntry->Result;
    }
    return TOptional<FApiValidationResult>();
}

void FApiValidationService::ValidateAuthToken(const FString &AuthToken, bool bForceValidation)
{
    if (!IsValidAuthTokenFormat(AuthToken))
    {
        FApiValidationResult Result(false, EApiValidationError::InvalidFormat, 0, TEXT("Invalid Auth token format"));
        OnAuthTokenValidationResultDetailedDelegate.Broadcast(Result);
        AuthTokenValidationCache.Add(AuthToken, FValidationCacheEntry(Result));
        return;
    }

    if (!bForceValidation)
    {
        TOptional<FApiValidationResult> CachedResult = GetLastAuthTokenValidationResultDetailed(AuthToken);
        if (CachedResult.IsSet())
        {
            const FApiValidationResult &Result = CachedResult.GetValue();
            OnAuthTokenValidationResultDetailedDelegate.Broadcast(Result);
            return;
        }
    }

    SetupCacheCleanupTimer();

    if (AuthTokenDebounceTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(AuthTokenDebounceTicker);
    }

    AuthTokenDebounceTicker = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([this, AuthToken](float DeltaTime) -> bool
                                      {
                                          this->PerformAuthTokenValidation(AuthToken);
                                          return false; }),
        ConvaiEditor::Constants::ApiKeyValidationDebounceTime);
}

bool FApiValidationService::IsValidatingAuthToken() const
{
    return bIsValidatingAuthToken;
}

TOptional<bool> FApiValidationService::GetLastAuthTokenValidationResult(const FString &AuthToken) const
{
    const FValidationCacheEntry *CachedEntry = AuthTokenValidationCache.Find(AuthToken);
    if (CachedEntry && !CachedEntry->IsExpired())
    {
        return CachedEntry->bIsValid;
    }
    return TOptional<bool>();
}

TOptional<FApiValidationResult> FApiValidationService::GetLastAuthTokenValidationResultDetailed(const FString &AuthToken) const
{
    const FValidationCacheEntry *CachedEntry = AuthTokenValidationCache.Find(AuthToken);
    if (CachedEntry && !CachedEntry->IsExpired())
    {
        return CachedEntry->Result;
    }
    return TOptional<FApiValidationResult>();
}

void FApiValidationService::ValidateAuthentication(bool bForceValidation)
{
    TPair<FString, FString> AuthHeaderAndKey = GetAuthHeaderAndKey();

    if (AuthHeaderAndKey.Value.IsEmpty())
    {
        FApiValidationResult Result(false, EApiValidationError::InvalidFormat, 0, TEXT("No authentication configured"));
        OnAuthenticationValidationResultDetailedDelegate.Broadcast(Result);
        return;
    }

    if (!bForceValidation)
    {
        TOptional<FApiValidationResult> CachedResult = GetLastAuthenticationValidationResultDetailed();
        if (CachedResult.IsSet())
        {
            const FApiValidationResult &Result = CachedResult.GetValue();
            OnAuthenticationValidationResultDetailedDelegate.Broadcast(Result);
            return;
        }
    }

    SetupCacheCleanupTimer();

    if (AuthenticationDebounceTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(AuthenticationDebounceTicker);
    }

    AuthenticationDebounceTicker = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([this, AuthHeaderAndKey](float DeltaTime) -> bool
                                      {
                                          this->PerformAuthenticationValidation(AuthHeaderAndKey);
                                          return false; }),
        ConvaiEditor::Constants::ApiKeyValidationDebounceTime);
}

bool FApiValidationService::IsValidatingAuthentication() const
{
    return bIsValidatingAuthentication;
}

TOptional<bool> FApiValidationService::GetLastAuthenticationValidationResult() const
{
    TPair<FString, FString> AuthHeaderAndKey = GetAuthHeaderAndKey();
    FString CacheKey = AuthHeaderAndKey.Key + TEXT(":") + AuthHeaderAndKey.Value;

    const FValidationCacheEntry *CachedEntry = AuthenticationValidationCache.Find(CacheKey);
    if (CachedEntry && !CachedEntry->IsExpired())
    {
        return CachedEntry->bIsValid;
    }
    return TOptional<bool>();
}

TOptional<FApiValidationResult> FApiValidationService::GetLastAuthenticationValidationResultDetailed() const
{
    TPair<FString, FString> AuthHeaderAndKey = GetAuthHeaderAndKey();
    FString CacheKey = AuthHeaderAndKey.Key + TEXT(":") + AuthHeaderAndKey.Value;

    const FValidationCacheEntry *CachedEntry = AuthenticationValidationCache.Find(CacheKey);
    if (CachedEntry && !CachedEntry->IsExpired())
    {
        return CachedEntry->Result;
    }
    return TOptional<FApiValidationResult>();
}

void FApiValidationService::ClearCache()
{
    ApiKeyValidationCache.Empty();
    AuthTokenValidationCache.Empty();
    AuthenticationValidationCache.Empty();
}

void FApiValidationService::ClearExpiredCache()
{
    for (auto It = ApiKeyValidationCache.CreateIterator(); It; ++It)
    {
        if (It.Value().IsExpired())
        {
            It.RemoveCurrent();
        }
    }

    for (auto It = AuthTokenValidationCache.CreateIterator(); It; ++It)
    {
        if (It.Value().IsExpired())
        {
            It.RemoveCurrent();
        }
    }

    for (auto It = AuthenticationValidationCache.CreateIterator(); It; ++It)
    {
        if (It.Value().IsExpired())
        {
            It.RemoveCurrent();
        }
    }
}

FOnApiKeyValidationResultDetailed &FApiValidationService::OnApiKeyValidationResultDetailed()
{
    return OnApiKeyValidationResultDetailedDelegate;
}

FOnAuthTokenValidationResultDetailed &FApiValidationService::OnAuthTokenValidationResultDetailed()
{
    return OnAuthTokenValidationResultDetailedDelegate;
}

FOnAuthenticationValidationResultDetailed &FApiValidationService::OnAuthenticationValidationResultDetailed()
{
    return OnAuthenticationValidationResultDetailedDelegate;
}

bool FApiValidationService::IsValidApiKeyFormat(const FString &ApiKey) const
{
    if (ApiKey.IsEmpty() || ApiKey.Len() < ConvaiEditor::Constants::MinApiKeyLength || ApiKey.Len() > ConvaiEditor::Constants::MaxApiKeyLength)
    {
        return false;
    }

    for (TCHAR Char : ApiKey)
    {
        FString CharString = FString(1, &Char);
        if (!ConvaiEditor::Constants::ValidApiKeyCharacters.Contains(CharString))
        {
            return false;
        }
    }

    return true;
}

bool FApiValidationService::IsValidAuthTokenFormat(const FString &AuthToken) const
{
    if (AuthToken.IsEmpty() || AuthToken.Len() < ConvaiEditor::Constants::MinApiKeyLength || AuthToken.Len() > ConvaiEditor::Constants::MaxApiKeyLength)
    {
        return false;
    }

    for (TCHAR Char : AuthToken)
    {
        FString CharString = FString(1, &Char);
        if (!ConvaiEditor::Constants::ValidAuthTokenCharacters.Contains(CharString))
        {
            return false;
        }
    }

    return true;
}

EApiValidationError FApiValidationService::GetErrorTypeFromResponseCode(int32 ResponseCode) const
{
    if (ResponseCode >= 200 && ResponseCode <= 299)
    {
        return EApiValidationError::None;
    }
    else if (ResponseCode == 401 || ResponseCode == 403)
    {
        return EApiValidationError::InvalidCredentials;
    }
    else if (ResponseCode == 429)
    {
        return EApiValidationError::RateLimited;
    }
    else if (ResponseCode >= 500 && ResponseCode <= 599)
    {
        return EApiValidationError::ServerError;
    }
    else if (ResponseCode == 0)
    {
        return EApiValidationError::NetworkError;
    }
    else
    {
        return EApiValidationError::Unknown;
    }
}

void FApiValidationService::PerformApiKeyValidation(const FString &ApiKey)
{
    if (bIsValidatingApiKey)
    {
        return;
    }

    if (CircuitBreaker && CircuitBreaker->IsOpen())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Convai API temporarily unavailable - circuit breaker open"));
        FApiValidationResult Result(false, EApiValidationError::NetworkError, 503, TEXT("Service temporarily unavailable"));
        OnApiKeyValidationResultDetailedDelegate.Broadcast(Result);
        return;
    }

    bIsValidatingApiKey = true;

    ConvaiEditor::FHttpAsyncRequest HttpRequest(FConvaiURLs::GetAPIValidationURL());
    HttpRequest.WithVerb(TEXT("POST"))
        .WithHeader(ConvaiEditor::Constants::API_Key_Header, ApiKey)
        .WithHeader(TEXT("Content-Type"), TEXT("application/json"))
        .WithBody(TEXT("{}"))
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

    AsyncOp->OnComplete([this, ApiKey, AsyncOp](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
                        {
        bIsValidatingApiKey = false;

        FApiValidationResult ValidationResult;

        if (!Result.IsSuccess())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("API key validation request failed"));
            ValidationResult = FApiValidationResult(false, EApiValidationError::NetworkError, 0, TEXT("Network error or invalid response"));
        }
        else
        {
            const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();
            const int32 ResponseCode = HttpResponse.ResponseCode;
            const EApiValidationError ErrorType = GetErrorTypeFromResponseCode(ResponseCode);

            if (ErrorType == EApiValidationError::None)
            {
                ValidationResult = FApiValidationResult(true, EApiValidationError::None, ResponseCode, TEXT("Validation successful"));
            }
            else
            {
                FString ErrorMessage = FString::Printf(TEXT("Validation failed with status code: %d"), ResponseCode);
                ValidationResult = FApiValidationResult(false, ErrorType, ResponseCode, ErrorMessage);
                UE_LOG(LogConvaiEditor, Warning, TEXT("API key validation failed with status code: %d"), ResponseCode);
            }
        }

        ApiKeyValidationCache.Add(ApiKey, FValidationCacheEntry(ValidationResult));

        AsyncTask(ENamedThreads::GameThread, [this, ValidationResult]()
        {
            OnApiKeyValidationResultDetailedDelegate.Broadcast(ValidationResult);
        }); });

    AsyncOp->Start();
}

void FApiValidationService::PerformAuthTokenValidation(const FString &AuthToken)
{
    if (bIsValidatingAuthToken)
    {
        return;
    }

    if (CircuitBreaker && CircuitBreaker->IsOpen())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Convai API temporarily unavailable - circuit breaker open"));
        FApiValidationResult Result(false, EApiValidationError::NetworkError, 503, TEXT("Service temporarily unavailable"));
        OnAuthTokenValidationResultDetailedDelegate.Broadcast(Result);
        return;
    }

    bIsValidatingAuthToken = true;

    ConvaiEditor::FHttpAsyncRequest HttpRequest(FConvaiURLs::GetAPIValidationURL());
    HttpRequest.WithVerb(TEXT("POST"))
        .WithHeader(ConvaiEditor::Constants::Auth_Token_Header, AuthToken)
        .WithHeader(TEXT("Content-Type"), TEXT("application/json"))
        .WithBody(TEXT("{}"))
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

    AsyncOp->OnComplete([this, AuthToken, AsyncOp](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
                        {
        bIsValidatingAuthToken = false;

        FApiValidationResult ValidationResult;

        if (!Result.IsSuccess())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("Auth token validation request failed"));
            ValidationResult = FApiValidationResult(false, EApiValidationError::NetworkError, 0, TEXT("Network error or invalid response"));
        }
        else
        {
            const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();
            const int32 ResponseCode = HttpResponse.ResponseCode;
            const EApiValidationError ErrorType = GetErrorTypeFromResponseCode(ResponseCode);

            if (ErrorType == EApiValidationError::None)
            {
                ValidationResult = FApiValidationResult(true, EApiValidationError::None, ResponseCode, TEXT("Validation successful"));
            }
            else
            {
                FString ErrorMessage = FString::Printf(TEXT("Validation failed with status code: %d"), ResponseCode);
                ValidationResult = FApiValidationResult(false, ErrorType, ResponseCode, ErrorMessage);
                UE_LOG(LogConvaiEditor, Warning, TEXT("Auth token validation failed with status code: %d"), ResponseCode);
            }
        }

        AuthTokenValidationCache.Add(AuthToken, FValidationCacheEntry(ValidationResult));

        AsyncTask(ENamedThreads::GameThread, [this, ValidationResult]()
        {
            OnAuthTokenValidationResultDetailedDelegate.Broadcast(ValidationResult);
        }); });

    AsyncOp->Start();
}

void FApiValidationService::PerformAuthenticationValidation(const TPair<FString, FString> &AuthHeaderAndKey)
{
    if (bIsValidatingAuthentication)
    {
        return;
    }

    if (CircuitBreaker && CircuitBreaker->IsOpen())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Convai API temporarily unavailable - circuit breaker open"));
        FApiValidationResult Result(false, EApiValidationError::NetworkError, 503, TEXT("Service temporarily unavailable"));
        OnAuthenticationValidationResultDetailedDelegate.Broadcast(Result);
        return;
    }

    bIsValidatingAuthentication = true;

    ConvaiEditor::FHttpAsyncRequest HttpRequest(FConvaiURLs::GetAPIValidationURL());
    HttpRequest.WithVerb(TEXT("POST"))
        .WithHeader(AuthHeaderAndKey.Key, AuthHeaderAndKey.Value)
        .WithHeader(TEXT("Content-Type"), TEXT("application/json"))
        .WithBody(TEXT("{}"))
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

    AsyncOp->OnComplete([this, AuthHeaderAndKey, AsyncOp](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
                        {
        bIsValidatingAuthentication = false;

        FApiValidationResult ValidationResult;

        if (!Result.IsSuccess())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("Authentication validation request failed"));
            ValidationResult = FApiValidationResult(false, EApiValidationError::NetworkError, 0, TEXT("Network error or invalid response"));
        }
        else
        {
            const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();
            const int32 ResponseCode = HttpResponse.ResponseCode;
            const EApiValidationError ErrorType = GetErrorTypeFromResponseCode(ResponseCode);

            if (ErrorType == EApiValidationError::None)
            {
                ValidationResult = FApiValidationResult(true, EApiValidationError::None, ResponseCode, TEXT("Validation successful"));
            }
            else
            {
                FString ErrorMessage = FString::Printf(TEXT("Validation failed with status code: %d"), ResponseCode);
                ValidationResult = FApiValidationResult(false, ErrorType, ResponseCode, ErrorMessage);
                UE_LOG(LogConvaiEditor, Warning, TEXT("Authentication validation failed with status code: %d"), ResponseCode);
            }
        }

        FString CacheKey = AuthHeaderAndKey.Key + TEXT(":") + AuthHeaderAndKey.Value;
        AuthenticationValidationCache.Add(CacheKey, FValidationCacheEntry(ValidationResult));

        AsyncTask(ENamedThreads::GameThread, [this, ValidationResult]()
        {
            OnAuthenticationValidationResultDetailedDelegate.Broadcast(ValidationResult);
        }); });

    AsyncOp->Start();
}

TSharedPtr<IAuthProvider> FApiValidationService::GetAuthProvider() const
{
    auto Result = FConvaiDIContainerManager::Get().Resolve<IAuthProvider>();
    if (Result.IsSuccess())
    {
        return Result.GetValue();
    }
    return nullptr;
}

bool FApiValidationService::IsValidResponseCode(int32 ResponseCode) const
{
    return ResponseCode >= ConvaiEditor::Constants::MinValidResponseCode && ResponseCode <= ConvaiEditor::Constants::MaxValidResponseCode;
}

void FApiValidationService::SetupCacheCleanupTimer()
{
    if (CacheCleanupTicker.IsValid())
    {
        return;
    }

    CacheCleanupTicker = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([this](float DeltaTime) -> bool
                                      {
                                          this->ClearExpiredCache();
                                          return true; }),
        ConvaiEditor::Constants::ValidationCacheExpirationTime);
}

TPair<FString, FString> FApiValidationService::GetAuthHeaderAndKey() const
{
    TSharedPtr<IAuthProvider> AuthProvider = GetAuthProvider();
    if (AuthProvider.IsValid())
    {
        FString ApiKey = AuthProvider->GetApiKey();
        FString AuthToken = AuthProvider->GetAuthToken();

        if (!AuthToken.IsEmpty())
        {
            return TPair<FString, FString>(ConvaiEditor::Constants::Auth_Token_Header, AuthToken);
        }
        else if (!ApiKey.IsEmpty())
        {
            return TPair<FString, FString>(ConvaiEditor::Constants::API_Key_Header, ApiKey);
        }
    }

    return TPair<FString, FString>(TEXT(""), TEXT(""));
}
