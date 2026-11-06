/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IApiValidationService.h
 *
 * Interface for API key and token validation.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Utility/ConvaiConstants.h"

/**
 * API validation error types.
 */
enum class EApiValidationError : uint8
{
    None = 0,
    NetworkError = 1,
    InvalidFormat = 2,
    InvalidCredentials = 3,
    ServerError = 4,
    RateLimited = 5,
    Unknown = 6
};

/**
 * API validation result.
 */
struct CONVAIEDITOR_API FApiValidationResult
{
    /** Whether the validation was successful */
    bool bIsValid;

    /** Error type if validation failed */
    EApiValidationError ErrorType;

    /** HTTP response code if available */
    int32 ResponseCode;

    /** Human-readable error message */
    FString ErrorMessage;

    /** Timestamp of the validation */
    FDateTime Timestamp;

    FApiValidationResult()
        : bIsValid(false), ErrorType(EApiValidationError::None), ResponseCode(0), ErrorMessage(TEXT("")), Timestamp(FDateTime::Now())
    {
    }

    FApiValidationResult(bool InIsValid, EApiValidationError InErrorType = EApiValidationError::None,
                         int32 InResponseCode = 0, const FString &InErrorMessage = TEXT(""))
        : bIsValid(InIsValid), ErrorType(InErrorType), ResponseCode(InResponseCode), ErrorMessage(InErrorMessage), Timestamp(FDateTime::Now())
    {
    }
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnApiKeyValidationResultDetailed, const FApiValidationResult & /*Result*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnAuthTokenValidationResultDetailed, const FApiValidationResult & /*Result*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnAuthenticationValidationResultDetailed, const FApiValidationResult & /*Result*/);

/**
 * Interface for API key and token validation with caching.
 */
class CONVAIEDITOR_API IApiValidationService : public IConvaiService
{
public:
    virtual ~IApiValidationService() = default;

    virtual void ValidateApiKey(const FString &ApiKey, bool bForceValidation = false) = 0;
    virtual bool IsValidatingApiKey() const = 0;
    virtual TOptional<bool> GetLastApiKeyValidationResult(const FString &ApiKey) const = 0;
    virtual TOptional<FApiValidationResult> GetLastApiKeyValidationResultDetailed(const FString &ApiKey) const = 0;

    virtual void ValidateAuthToken(const FString &AuthToken, bool bForceValidation = false) = 0;
    virtual bool IsValidatingAuthToken() const = 0;
    virtual TOptional<bool> GetLastAuthTokenValidationResult(const FString &AuthToken) const = 0;
    virtual TOptional<FApiValidationResult> GetLastAuthTokenValidationResultDetailed(const FString &AuthToken) const = 0;

    virtual void ValidateAuthentication(bool bForceValidation = false) = 0;
    virtual bool IsValidatingAuthentication() const = 0;
    virtual TOptional<bool> GetLastAuthenticationValidationResult() const = 0;
    virtual TOptional<FApiValidationResult> GetLastAuthenticationValidationResultDetailed() const = 0;

    virtual void ClearCache() = 0;
    virtual void ClearExpiredCache() = 0;

    virtual FOnApiKeyValidationResultDetailed &OnApiKeyValidationResultDetailed() = 0;
    virtual FOnAuthTokenValidationResultDetailed &OnAuthTokenValidationResultDetailed() = 0;
    virtual FOnAuthenticationValidationResultDetailed &OnAuthenticationValidationResultDetailed() = 0;

    virtual bool IsValidApiKeyFormat(const FString &ApiKey) const = 0;
    virtual bool IsValidAuthTokenFormat(const FString &AuthToken) const = 0;
    virtual EApiValidationError GetErrorTypeFromResponseCode(int32 ResponseCode) const = 0;

    static FName StaticType() { return TEXT("IApiValidationService"); }
};
