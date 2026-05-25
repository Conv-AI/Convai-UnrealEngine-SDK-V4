/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IAuthProvider.h
 *
 * Interface for authentication management.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/ConvaiDIContainer.h"

/**
 * Interface for API key and auth token management.
 */
class CONVAIEDITOR_API IAuthProvider : public IConvaiService
{
public:
    virtual ~IAuthProvider() = default;

    virtual FString GetApiKey() const = 0;
    virtual FString GetAuthToken() const = 0;
    virtual TPair<FString, FString> GetAuthHeaderAndKey() const = 0;
    virtual bool HasApiKey() const = 0;
    virtual bool HasAuthToken() const = 0;
    virtual bool HasAuthentication() const = 0;

    virtual void SetApiKey(const FString &ApiKey) = 0;
    virtual void SetAuthToken(const FString &AuthToken) = 0;
    virtual void ClearAuthentication() = 0;

    static FName StaticType() { return TEXT("IAuthProvider"); }
};
