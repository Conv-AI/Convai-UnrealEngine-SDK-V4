/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * HttpAsyncOperation.h
 *
 * HTTP async operations with circuit breaker and retry support.
 */

#pragma once

#include "CoreMinimal.h"
#include "Async/AsyncOperation.h"
#include "Async/CancellationToken.h"
#include "Async/AsyncProgress.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Utility/CircuitBreaker.h"
#include "Utility/RetryPolicy.h"

namespace ConvaiEditor
{
    /** Configuration for HTTP async operations. */
    struct CONVAIEDITOR_API FHttpAsyncRequest
    {
        FString Verb = TEXT("GET");
        FString URL;
        TMap<FString, FString> Headers;
        FString Body;
        float TimeoutSeconds = 30.0f;
        FString ContentType = TEXT("application/json");

        FHttpAsyncRequest() = default;

        explicit FHttpAsyncRequest(const FString &InURL)
            : URL(InURL)
        {
        }

        FHttpAsyncRequest &WithHeader(const FString &Key, const FString &Value)
        {
            Headers.Add(Key, Value);
            return *this;
        }

        FHttpAsyncRequest &WithBody(const FString &InBody)
        {
            Body = InBody;
            return *this;
        }

        FHttpAsyncRequest &WithVerb(const FString &InVerb)
        {
            Verb = InVerb;
            return *this;
        }

        FHttpAsyncRequest &WithTimeout(float InTimeout)
        {
            TimeoutSeconds = InTimeout;
            return *this;
        }
    };

    /** HTTP async operation result. */
    struct CONVAIEDITOR_API FHttpAsyncResponse
    {
        int32 ResponseCode = 0;
        FString Body;
        TMap<FString, FString> Headers;

        bool IsSuccess() const { return ResponseCode >= 200 && ResponseCode < 300; }
        bool IsClientError() const { return ResponseCode >= 400 && ResponseCode < 500; }
        bool IsServerError() const { return ResponseCode >= 500 && ResponseCode < 600; }

        FString GetHeader(const FString &Key) const
        {
            const FString *Value = Headers.Find(Key);
            return Value ? *Value : FString();
        }
    };

    /** HTTP async operations with circuit breaker and retry support. */
    class CONVAIEDITOR_API FHttpAsyncOperation
    {
    public:
        /** Creates an HTTP async operation */
        static TSharedPtr<FAsyncOperation<FHttpAsyncResponse>> Create(
            const FHttpAsyncRequest &Request,
            TSharedPtr<FCancellationToken> CancellationToken = nullptr);

        /** Creates an HTTP async operation with circuit breaker */
        static TSharedPtr<FAsyncOperation<FHttpAsyncResponse>> CreateWithCircuitBreaker(
            const FHttpAsyncRequest &Request,
            TSharedPtr<FCircuitBreaker> CircuitBreaker,
            TSharedPtr<FCancellationToken> CancellationToken = nullptr);

        /** Creates an HTTP async operation with retry policy */
        static TSharedPtr<FAsyncOperation<FHttpAsyncResponse>> CreateWithRetry(
            const FHttpAsyncRequest &Request,
            TSharedPtr<FRetryPolicy> RetryPolicy,
            TSharedPtr<FCancellationToken> CancellationToken = nullptr);

        /** Creates an HTTP async operation with full protection */
        static TSharedPtr<FAsyncOperation<FHttpAsyncResponse>> CreateWithProtection(
            const FHttpAsyncRequest &Request,
            TSharedPtr<FCircuitBreaker> CircuitBreaker,
            TSharedPtr<FRetryPolicy> RetryPolicy,
            TSharedPtr<FCancellationToken> CancellationToken = nullptr);

    private:
        static TConvaiResult<FHttpAsyncResponse> ExecuteHttpRequest(
            const FHttpAsyncRequest &Request,
            TSharedPtr<FCancellationToken> Token,
            TSharedPtr<IAsyncProgressReporter> Progress,
            TSharedPtr<FCircuitBreaker> CircuitBreaker = nullptr,
            TSharedPtr<FRetryPolicy> RetryPolicy = nullptr);

        static TConvaiResult<FHttpAsyncResponse> PerformHttpRequest(
            const FHttpAsyncRequest &Request,
            TSharedPtr<FCancellationToken> Token,
            TSharedPtr<IAsyncProgressReporter> Progress);

        static FHttpAsyncResponse ConvertHttpResponse(FHttpResponsePtr HttpResponse);
    };

} // namespace ConvaiEditor
