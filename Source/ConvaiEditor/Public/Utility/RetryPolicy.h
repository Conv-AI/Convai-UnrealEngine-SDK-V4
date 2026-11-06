/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * RetryPolicy.h
 *
 * Retry policy implementation with exponential backoff and jitter.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/ConvaiDIContainer.h"
#include "Templates/Function.h"
#include "HAL/PlatformProcess.h"

namespace ConvaiEditor
{
    /** Retry strategy type */
    enum class ERetryStrategy : uint8
    {
        /** No retry */
        None,

        /** Fixed delay between retries */
        Fixed,

        /** Exponential backoff */
        Exponential,

        /** Linear backoff */
        Linear
    };

    /** Retry policy configuration */
    struct FRetryPolicyConfig
    {
        /** Maximum number of retry attempts */
        int32 MaxAttempts = 3;

        /** Base delay between retries in seconds */
        float BaseDelaySeconds = 1.0f;

        /** Maximum delay between retries in seconds */
        float MaxDelaySeconds = 30.0f;

        /** Retry strategy to use */
        ERetryStrategy Strategy = ERetryStrategy::Exponential;

        /** Enable random jitter to prevent thundering herd */
        bool bEnableJitter = true;

        /** Maximum jitter to add in seconds */
        float JitterMaxSeconds = 1.0f;

        /** Enable detailed logging */
        bool bEnableLogging = true;

        /** Name of this retry policy for logging */
        FString Name = TEXT("RetryPolicy");

        /** Custom predicate to determine if error should be retried */
        TFunction<bool(const FString &ErrorMessage, int32 AttemptNumber)> ShouldRetryPredicate;
    };

    /** Retry statistics */
    struct FRetryStats
    {
        /** Total number of operations executed */
        int32 TotalOperations = 0;

        /** Total number of successful operations (without retry) */
        int32 ImmediateSuccesses = 0;

        /** Total number of operations that succeeded after retry */
        int32 SuccessAfterRetry = 0;

        /** Total number of operations that failed after all retries */
        int32 TotalFailures = 0;

        /** Total number of retry attempts made */
        int32 TotalRetryAttempts = 0;

        FString GetSummary() const;
        void Reset();
    };

    /** Implements retry logic with configurable backoff strategies */
    class CONVAIEDITOR_API FRetryPolicy
    {
    public:
        explicit FRetryPolicy(const FRetryPolicyConfig &InConfig = FRetryPolicyConfig());

        ~FRetryPolicy() = default;

        /** Execute an operation with retry logic */
        template <typename TResult>
        TConvaiResult<TResult> Execute(TFunction<TConvaiResult<TResult>()> Operation)
        {
            int32 Attempt = 0;
            TConvaiResult<TResult> Result;

            while (Attempt <= Config.MaxAttempts)
            {
                Result = Operation();

                if (Result.IsSuccess())
                {
                    RecordSuccess(Attempt);
                    return Result;
                }

                Attempt++;

                if (Attempt > Config.MaxAttempts)
                {
                    RecordFailure(Attempt - 1);
                    LogRetryExhausted(Result.GetError(), Attempt - 1);
                    return Result;
                }

                if (Config.ShouldRetryPredicate && !Config.ShouldRetryPredicate(Result.GetError(), Attempt))
                {
                    RecordFailure(Attempt - 1);
                    LogRetrySkipped(Result.GetError(), Attempt);
                    return Result;
                }

                float Delay = CalculateDelay(Attempt);

                LogRetryAttempt(Result.GetError(), Attempt, Delay);

                FPlatformProcess::Sleep(Delay);
            }

            RecordFailure(Attempt);
            return Result;
        }

        /** Execute a void operation with retry logic */
        TConvaiResult<void> Execute(TFunction<TConvaiResult<void>()> Operation);

        /** Get statistics */
        FRetryStats GetStats() const;

        /** Reset statistics */
        void ResetStats();

        /** Get configuration */
        const FRetryPolicyConfig &GetConfig() const { return Config; }

    private:
        float CalculateDelay(int32 AttemptNumber) const;
        float AddJitter(float Delay) const;
        void RecordSuccess(int32 AttemptNumber);
        void RecordFailure(int32 AttemptCount);
        void LogRetryAttempt(const FString &ErrorMessage, int32 AttemptNumber, float Delay) const;
        void LogRetryExhausted(const FString &ErrorMessage, int32 TotalAttempts) const;
        void LogRetrySkipped(const FString &ErrorMessage, int32 AttemptNumber) const;

        FRetryPolicyConfig Config;
        FRetryStats Stats;
        mutable FCriticalSection StatsMutex;
    };

    /** Common retry predicates */
    namespace RetryPredicates
    {
        /** Retry only on network errors */
        CONVAIEDITOR_API bool OnlyNetworkErrors(const FString &ErrorMessage, int32 AttemptNumber);

        /** Retry only on timeout errors */
        CONVAIEDITOR_API bool OnlyTimeoutErrors(const FString &ErrorMessage, int32 AttemptNumber);

        /** Retry on transient errors */
        CONVAIEDITOR_API bool OnlyTransientErrors(const FString &ErrorMessage, int32 AttemptNumber);

        /** Never retry */
        CONVAIEDITOR_API bool NeverRetry(const FString &ErrorMessage, int32 AttemptNumber);

        /** Always retry */
        CONVAIEDITOR_API bool AlwaysRetry(const FString &ErrorMessage, int32 AttemptNumber);
    }

} // namespace ConvaiEditor
