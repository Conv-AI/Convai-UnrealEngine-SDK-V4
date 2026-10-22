/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * DecryptionService.h
 *
 * Manages server-side decryption of OAuth tokens.
 */

#pragma once

#include "Services/OAuth/IDecryptionService.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Templates/SharedPointer.h"
#include "Containers/Queue.h"
#include "Utility/CircuitBreaker.h"
#include "Utility/RetryPolicy.h"
#include <atomic>

/**
 * Manages server-side decryption of OAuth tokens.
 */
class FDecryptionService : public IDecryptionService, public TSharedFromThis<FDecryptionService>
{
public:
    FDecryptionService();
    virtual ~FDecryptionService() override;

    /** Initializes the service */
    virtual void Startup() override;

    /** Shuts down the service */
    virtual void Shutdown() override;

    /** Decrypts encrypted data asynchronously */
    virtual void DecryptAsync(
        const FString &EncryptedData,
        TFunction<void(const FString &DecryptedData)> OnSuccess,
        TFunction<void(const FString &ErrorMessage)> OnFailure) override;

    /** Sets the decryption configuration */
    virtual void SetConfig(const FDecryptionConfig &Config) override;

    /** Returns the current configuration */
    virtual FDecryptionConfig GetConfig() const override;

    /** Returns whether a decryption request is processing */
    virtual bool IsProcessing() const override;

    /** Cancels all pending decryption requests */
    virtual void CancelPendingRequests() override;

private:
    /** Internal request context for tracking retry attempts */
    struct FDecryptionRequest
    {
        FString EncryptedData;
        TFunction<void(const FString &)> OnSuccess;
        TFunction<void(const FString &)> OnFailure;
        int32 RetryCount = 0;
        TSharedPtr<IHttpRequest> HttpRequest;
        FDateTime RequestStartTime;
    };

    /** Executes HTTP POST request to decryption endpoint */
    void ExecuteDecryptionRequest(TSharedPtr<FDecryptionRequest> Request);

    /** Parses JSON response from server */
    bool ParseDecryptionResponse(const FString &ResponseBody, FString &OutDecryptedData, FString &OutError);

    /** Validates request data before sending */
    bool ValidateEncryptedData(const FString &EncryptedData, FString &OutError);

    /** Generates unique request ID for logging */
    FString GenerateRequestId() const;

    /** Masks sensitive data for logging */
    static FString MaskSensitiveData(const FString &Data);

    FDecryptionConfig Config;
    TArray<TSharedPtr<FDecryptionRequest>> ActiveRequests;
    mutable TAtomic<int32> RequestCounter;
    mutable FCriticalSection RequestLock;
    std::atomic<bool> bIsShuttingDown{false};
    TSharedPtr<ConvaiEditor::FCircuitBreaker> CircuitBreaker;
    TSharedPtr<ConvaiEditor::FRetryPolicy> RetryPolicy;
};
