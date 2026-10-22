/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ApiValidationService.h
 *
 * Implementation of API key and token validation.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/IApiValidationService.h"
#include "Engine/Engine.h"
#include "Containers/Ticker.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Utility/ConvaiConstants.h"
#include "Services/ConvaiDIContainer.h"
#include "Utility/CircuitBreaker.h"
#include "Utility/RetryPolicy.h"

// Forward declarations
class IAuthProvider;

/** Validates API keys and tokens with caching and debouncing. */
class CONVAIEDITOR_API FApiValidationService : public IApiValidationService
{
public:
    FApiValidationService();
    virtual ~FApiValidationService();

    virtual void Startup() override;
    virtual void Shutdown() override;

    virtual void ValidateApiKey(const FString &ApiKey, bool bForceValidation = false) override;
    virtual bool IsValidatingApiKey() const override;
    virtual TOptional<bool> GetLastApiKeyValidationResult(const FString &ApiKey) const override;
    virtual TOptional<FApiValidationResult> GetLastApiKeyValidationResultDetailed(const FString &ApiKey) const override;

    virtual void ValidateAuthToken(const FString &AuthToken, bool bForceValidation = false) override;
    virtual bool IsValidatingAuthToken() const override;
    virtual TOptional<bool> GetLastAuthTokenValidationResult(const FString &AuthToken) const override;
    virtual TOptional<FApiValidationResult> GetLastAuthTokenValidationResultDetailed(const FString &AuthToken) const override;

    virtual void ValidateAuthentication(bool bForceValidation = false) override;
    virtual bool IsValidatingAuthentication() const override;
    virtual TOptional<bool> GetLastAuthenticationValidationResult() const override;
    virtual TOptional<FApiValidationResult> GetLastAuthenticationValidationResultDetailed() const override;

    virtual void ClearCache() override;
    virtual void ClearExpiredCache() override;

    virtual FOnApiKeyValidationResultDetailed &OnApiKeyValidationResultDetailed() override;
    virtual FOnAuthTokenValidationResultDetailed &OnAuthTokenValidationResultDetailed() override;
    virtual FOnAuthenticationValidationResultDetailed &OnAuthenticationValidationResultDetailed() override;

    virtual bool IsValidApiKeyFormat(const FString &ApiKey) const override;
    virtual bool IsValidAuthTokenFormat(const FString &AuthToken) const override;
    virtual EApiValidationError GetErrorTypeFromResponseCode(int32 ResponseCode) const override;

private:
    struct FValidationCacheEntry
    {
        bool bIsValid;
        FApiValidationResult Result;
        FDateTime Timestamp;

        FValidationCacheEntry(bool InIsValid)
            : bIsValid(InIsValid), Result(InIsValid, EApiValidationError::None, 0, TEXT("")), Timestamp(FDateTime::Now())
        {
        }

        FValidationCacheEntry(const FApiValidationResult &InResult)
            : bIsValid(InResult.bIsValid), Result(InResult), Timestamp(FDateTime::Now())
        {
        }

        bool IsExpired() const
        {
            return (FDateTime::Now() - Timestamp).GetTotalSeconds() > ConvaiEditor::Constants::ValidationCacheExpirationTime;
        }
    };

    void PerformApiKeyValidation(const FString &ApiKey);
    void PerformAuthTokenValidation(const FString &AuthToken);
    void PerformAuthenticationValidation(const TPair<FString, FString> &AuthHeaderAndKey);
    TSharedPtr<IAuthProvider> GetAuthProvider() const;
    bool IsValidResponseCode(int32 ResponseCode) const;
    void SetupCacheCleanupTimer();
    TPair<FString, FString> GetAuthHeaderAndKey() const;

private:
    bool bIsValidatingApiKey;
    bool bIsValidatingAuthToken;
    bool bIsValidatingAuthentication;

    FTSTicker::FDelegateHandle ApiKeyDebounceTicker;
    FTSTicker::FDelegateHandle AuthTokenDebounceTicker;
    FTSTicker::FDelegateHandle AuthenticationDebounceTicker;
    FTSTicker::FDelegateHandle CacheCleanupTicker;

    TMap<FString, FValidationCacheEntry> ApiKeyValidationCache;
    TMap<FString, FValidationCacheEntry> AuthTokenValidationCache;
    TMap<FString, FValidationCacheEntry> AuthenticationValidationCache;

    FOnApiKeyValidationResultDetailed OnApiKeyValidationResultDetailedDelegate;
    FOnAuthTokenValidationResultDetailed OnAuthTokenValidationResultDetailedDelegate;
    FOnAuthenticationValidationResultDetailed OnAuthenticationValidationResultDetailedDelegate;

    TSharedPtr<ConvaiEditor::FCircuitBreaker> CircuitBreaker;
    TSharedPtr<ConvaiEditor::FRetryPolicy> RetryPolicy;
};
