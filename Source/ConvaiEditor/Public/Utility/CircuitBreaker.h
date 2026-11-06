/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * CircuitBreaker.h
 *
 * Implementation of Circuit Breaker pattern for error recovery.
 */

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Services/ConvaiDIContainer.h"
#include "Templates/Function.h"
#include "Misc/DateTime.h"

namespace ConvaiEditor
{
    /** Circuit breaker state */
    enum class ECircuitBreakerState : uint8
    {
        /** Circuit closed - normal operation */
        Closed,

        /** Circuit open - failing fast */
        Open,

        /** Circuit half-open - testing recovery */
        HalfOpen
    };

    /** Circuit breaker configuration */
    struct FCircuitBreakerConfig
    {
        /** Number of consecutive failures before opening circuit */
        int32 FailureThreshold = 5;

        /** Number of consecutive successes before closing circuit */
        int32 SuccessThreshold = 2;

        /** Duration to wait before attempting recovery */
        float OpenTimeoutSeconds = 30.0f;

        /** Maximum number of allowed requests in HalfOpen state */
        int32 HalfOpenMaxRequests = 1;

        /** Enable detailed logging */
        bool bEnableLogging = true;

        /** Name of this circuit breaker */
        FString Name = TEXT("CircuitBreaker");
    };

    /** Circuit breaker statistics */
    struct FCircuitBreakerStats
    {
        /** Total number of successful executions */
        int32 TotalSuccesses = 0;

        /** Total number of failed executions */
        int32 TotalFailures = 0;

        /** Number of times circuit opened */
        int32 CircuitOpenCount = 0;

        /** Current consecutive failure count */
        int32 ConsecutiveFailures = 0;

        /** Current consecutive success count (in HalfOpen) */
        int32 ConsecutiveSuccesses = 0;

        /** Timestamp when circuit was opened (if Open) */
        FDateTime CircuitOpenedAt;

        ECircuitBreakerState CurrentState = ECircuitBreakerState::Closed;

        FString GetStateString() const;
        FString GetSummary() const;
        void Reset();
    };

    /** Implements the Circuit Breaker pattern for resilient service calls */
    class CONVAIEDITOR_API FCircuitBreaker
    {
    public:
        explicit FCircuitBreaker(const FCircuitBreakerConfig &InConfig = FCircuitBreakerConfig());

        ~FCircuitBreaker();

        /** Execute an operation with circuit breaker protection */
        template <typename TResult>
        TConvaiResult<TResult> Execute(TFunction<TConvaiResult<TResult>()> Operation)
        {
            if (!CanExecute())
            {
                return TConvaiResult<TResult>::Failure(
                    FString::Printf(TEXT("Circuit breaker '%s' is OPEN - request rejected"), *Config.Name));
            }

            TConvaiResult<TResult> Result = Operation();

            if (Result.IsSuccess())
            {
                OnSuccess();
            }
            else
            {
                OnFailure();
            }

            return Result;
        }

        /** Execute a void operation with circuit breaker protection */
        TConvaiResult<void> Execute(TFunction<TConvaiResult<void>()> Operation);

        /** Manually open the circuit */
        void Open();

        /** Manually close the circuit */
        void Close();

        /** Force circuit to half-open state */
        void ForceHalfOpen();

        /** Reset circuit breaker to initial state */
        void Reset();

        /** Get current state */
        ECircuitBreakerState GetState() const;

        /** Check if circuit is open */
        bool IsOpen() const;

        /** Check if circuit is closed */
        bool IsClosed() const;

        /** Check if circuit is half-open */
        bool IsHalfOpen() const;

        /** Get statistics */
        FCircuitBreakerStats GetStats() const;

        /** Get configuration */
        const FCircuitBreakerConfig &GetConfig() const { return Config; }

    private:
        bool CanExecute();
        void OnSuccess();
        void OnFailure();
        void TransitionTo(ECircuitBreakerState NewState);
        bool HasTimeoutElapsed() const;
        void LogStateChange(ECircuitBreakerState OldState, ECircuitBreakerState NewState) const;

        FCircuitBreakerConfig Config;
        FCircuitBreakerStats Stats;
        int32 HalfOpenActiveRequests = 0;
        mutable FCriticalSection Mutex;
    };

} // namespace ConvaiEditor
