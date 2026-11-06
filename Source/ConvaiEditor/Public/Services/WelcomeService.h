/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * WelcomeService.h
 *
 * Manages welcome experience and API key validation.
 */

#pragma once

#include "Services/IWelcomeService.h"
#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Services/ApiValidationService.h"

// Forward declarations
class SWindow;
class IConfigurationService;
class IApiValidationService;

/**
 * Manages welcome experience and API key validation.
 */
class CONVAIEDITOR_API FWelcomeService : public IWelcomeService, public TSharedFromThis<FWelcomeService>
{
public:
    FWelcomeService();
    virtual ~FWelcomeService() = default;

    virtual void Startup() override;
    virtual void Shutdown() override;

    virtual bool HasCompletedWelcome() const override;
    virtual void MarkWelcomeCompleted() override;
    virtual bool HasValidApiKey() const override;
    virtual bool ValidateAndStoreApiKey(const FString &ApiKey) override;
    virtual FString GetStoredApiKey() const override;

    virtual void ShowWelcomeWindowIfNeeded() override;
    virtual void ShowWelcomeWindow() override;
    virtual void CloseWelcomeWindow() override;
    virtual bool IsWelcomeWindowOpen() const override;

    virtual FOnWelcomeCompleted &OnWelcomeCompleted() override { return OnWelcomeCompletedDelegate; }
    virtual FOnApiKeyValidated &OnApiKeyValidated() override { return OnApiKeyValidatedDelegate; }
    virtual FOnApiKeyValidationFailed &OnApiKeyValidationFailed() override { return OnApiKeyValidationFailedDelegate; }

    static FName StaticType() { return TEXT("IWelcomeService"); }

private:
    static const FString WELCOME_COMPLETED_KEY;
    static const FString API_KEY_KEY;

    TSharedPtr<IConfigurationService> GetConfigurationService() const;
    bool IsValidApiKeyFormat(const FString &ApiKey) const;
    void OnWelcomeWindowClosed(const TSharedRef<SWindow> &ClosedWindow);
    void OnApiKeyValidationResult(const FApiValidationResult &Result);

    FOnWelcomeCompleted OnWelcomeCompletedDelegate;
    FOnApiKeyValidated OnApiKeyValidatedDelegate;
    FOnApiKeyValidationFailed OnApiKeyValidationFailedDelegate;

    mutable FCriticalSection StateLock;

    TWeakPtr<IApiValidationService> ValidationService;
    FDelegateHandle ValidationResultHandle;
    FString PendingValidationApiKey;
};
