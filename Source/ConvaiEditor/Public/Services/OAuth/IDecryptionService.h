/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IDecryptionService.h
 *
 * Interface for server-side OAuth token decryption.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Services/ConvaiDIContainer.h"
#include "Templates/SharedPointer.h"

/** Interface for decrypting OAuth tokens via remote API. */
class CONVAIEDITOR_API IDecryptionService : public IConvaiService
{
public:
    virtual ~IDecryptionService() override = default;

    struct FDecryptionConfig
    {
        FString EndpointUrl = TEXT("https://login.convai.com/api/decrypt");
        float TimeoutSeconds = 30.0f;
        int32 MaxRetries = 3;
        float RetryDelaySeconds = 1.0f;
        bool bVerboseLogging = false;
    };

    /** Asynchronously decrypt encrypted data using remote decryption service */
    virtual void DecryptAsync(
        const FString &EncryptedData,
        TFunction<void(const FString &DecryptedData)> OnSuccess,
        TFunction<void(const FString &ErrorMessage)> OnFailure) = 0;

    virtual void SetConfig(const FDecryptionConfig &Config) = 0;
    virtual FDecryptionConfig GetConfig() const = 0;
    virtual bool IsProcessing() const = 0;
    virtual void CancelPendingRequests() = 0;

    static FName StaticType() { return TEXT("IDecryptionService"); }
};
