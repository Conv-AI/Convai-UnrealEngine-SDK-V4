/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiCharacterApiService.h
 *
 * Service for fetching Convai character metadata.
 */

#pragma once

#include "CoreMinimal.h"
#include "Models/ConvaiCharacterMetadata.h"
#include "ConvaiEditor.h"
#include "Utility/CircuitBreaker.h"
#include "Utility/RetryPolicy.h"

/**
 * Interface for fetching character metadata from Convai API.
 */
class CONVAIEDITOR_API IConvaiCharacterApiService : public IConvaiService
{
public:
    virtual ~IConvaiCharacterApiService() = default;

    /** Fetches character metadata asynchronously */
    virtual TFuture<TOptional<FConvaiCharacterMetadata>> FetchCharacterMetadataAsync(const FString &CharacterID) = 0;

    static FName StaticType() { return TEXT("IConvaiCharacterApiService"); }
};

/**
 * Fetches and caches Convai character metadata.
 */
class CONVAIEDITOR_API FConvaiCharacterApiService : public IConvaiCharacterApiService
{
public:
    FConvaiCharacterApiService();
    FConvaiCharacterApiService(const FString &InApiKey);
    virtual ~FConvaiCharacterApiService() override;

    virtual void Startup() override;
    virtual void Shutdown() override;

    virtual TFuture<TOptional<FConvaiCharacterMetadata>> FetchCharacterMetadataAsync(const FString &CharacterID) override;

    /** Clears internal cache */
    void InvalidateCache();

    /** Sets API key */
    void SetApiKey(const FString &InApiKey);

private:
    FString ApiKey;
    TMap<FString, FConvaiCharacterMetadata> MetadataCache;
    FCriticalSection CacheMutex;

    /** Circuit breaker for Convai Character API (shared across all requests) */
    TSharedPtr<ConvaiEditor::FCircuitBreaker> CircuitBreaker;

    /** Retry policy for transient failures (shared across all requests) */
    TSharedPtr<ConvaiEditor::FRetryPolicy> RetryPolicy;
};