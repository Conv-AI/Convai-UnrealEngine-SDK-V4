/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IWelcomeService.h
 *
 * Interface for welcome experience and API key management.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"

/**
 * Manages welcome experience and API key validation.
 */
class CONVAIEDITOR_API IWelcomeService : public IConvaiService
{
public:
    virtual ~IWelcomeService() = default;

    virtual bool HasCompletedWelcome() const = 0;
    virtual void MarkWelcomeCompleted() = 0;
    virtual bool HasValidApiKey() const = 0;
    virtual bool ValidateAndStoreApiKey(const FString &ApiKey) = 0;
    virtual FString GetStoredApiKey() const = 0;

    virtual void ShowWelcomeWindowIfNeeded() = 0;
    virtual void ShowWelcomeWindow() = 0;
    virtual void CloseWelcomeWindow() = 0;
    virtual bool IsWelcomeWindowOpen() const = 0;

    DECLARE_MULTICAST_DELEGATE(FOnWelcomeCompleted);
    virtual FOnWelcomeCompleted &OnWelcomeCompleted() = 0;

    DECLARE_MULTICAST_DELEGATE_OneParam(FOnApiKeyValidated, const FString & /*ApiKey*/);
    virtual FOnApiKeyValidated &OnApiKeyValidated() = 0;

    DECLARE_MULTICAST_DELEGATE_OneParam(FOnApiKeyValidationFailed, const FString & /*Error*/);
    virtual FOnApiKeyValidationFailed &OnApiKeyValidationFailed() = 0;

    static FName StaticType() { return TEXT("IWelcomeService"); }
};
