/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * CircuitBreaker.cpp
 *
 * Implementation of Circuit Breaker pattern
 */

#include "Utility/CircuitBreaker.h"
#include "Utility/CircuitBreakerRegistry.h"
#include "ConvaiEditor.h"

namespace ConvaiEditor
{

    FString FCircuitBreakerStats::GetStateString() const
    {
        switch (CurrentState)
        {
        case ECircuitBreakerState::Closed:
            return TEXT("CLOSED");
        case ECircuitBreakerState::Open:
            return TEXT("OPEN");
        case ECircuitBreakerState::HalfOpen:
            return TEXT("HALF-OPEN");
        default:
            return TEXT("UNKNOWN");
        }
    }

    FString FCircuitBreakerStats::GetSummary() const
    {
        return FString::Printf(
            TEXT("State: %s | Success: %d | Failures: %d | Consecutive Failures: %d | Circuit Opened: %d times"),
            *GetStateString(),
            TotalSuccesses,
            TotalFailures,
            ConsecutiveFailures,
            CircuitOpenCount);
    }

    void FCircuitBreakerStats::Reset()
    {
        TotalSuccesses = 0;
        TotalFailures = 0;
        CircuitOpenCount = 0;
        ConsecutiveFailures = 0;
        ConsecutiveSuccesses = 0;
        CurrentState = ECircuitBreakerState::Closed;
        CircuitOpenedAt = FDateTime();
    }

    FCircuitBreaker::FCircuitBreaker(const FCircuitBreakerConfig &InConfig)
        : Config(InConfig)
    {
        Stats.CurrentState = ECircuitBreakerState::Closed;

        FCircuitBreakerRegistry::Get().Register(Config.Name, this);

        if (Config.bEnableLogging)
        {
            UE_LOG(LogConvaiEditor, Log, TEXT("CircuitBreaker '%s' initialized"), *Config.Name);
        }
    }

    FCircuitBreaker::~FCircuitBreaker()
    {
        FCircuitBreakerRegistry::Get().Unregister(Config.Name);
    }

    TConvaiResult<void> FCircuitBreaker::Execute(TFunction<TConvaiResult<void>()> Operation)
    {
        if (!CanExecute())
        {
            return TConvaiResult<void>::Failure(
                FString::Printf(TEXT("Circuit breaker '%s' is OPEN - request rejected"), *Config.Name));
        }

        TConvaiResult<void> Result = Operation();

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

    void FCircuitBreaker::Open()
    {
        FScopeLock Lock(&Mutex);

        if (Stats.CurrentState != ECircuitBreakerState::Open)
        {
            TransitionTo(ECircuitBreakerState::Open);
        }
    }

    void FCircuitBreaker::Close()
    {
        FScopeLock Lock(&Mutex);

        if (Stats.CurrentState != ECircuitBreakerState::Closed)
        {
            Stats.ConsecutiveFailures = 0;
            Stats.ConsecutiveSuccesses = 0;
            TransitionTo(ECircuitBreakerState::Closed);
        }
    }

    void FCircuitBreaker::ForceHalfOpen()
    {
        FScopeLock Lock(&Mutex);

        if (Stats.CurrentState == ECircuitBreakerState::Open)
        {
            Stats.ConsecutiveFailures = 0;
            Stats.ConsecutiveSuccesses = 0;
            TransitionTo(ECircuitBreakerState::HalfOpen);

            if (Config.bEnableLogging)
            {
                UE_LOG(LogConvaiEditor, Log, TEXT("CircuitBreaker '%s' forced to HALF-OPEN"), *Config.Name);
            }
        }
    }

    void FCircuitBreaker::Reset()
    {
        FScopeLock Lock(&Mutex);

        Stats.Reset();
        HalfOpenActiveRequests = 0;
    }

    ECircuitBreakerState FCircuitBreaker::GetState() const
    {
        FScopeLock Lock(&Mutex);
        return Stats.CurrentState;
    }

    bool FCircuitBreaker::IsOpen() const
    {
        return GetState() == ECircuitBreakerState::Open;
    }

    bool FCircuitBreaker::IsClosed() const
    {
        return GetState() == ECircuitBreakerState::Closed;
    }

    bool FCircuitBreaker::IsHalfOpen() const
    {
        return GetState() == ECircuitBreakerState::HalfOpen;
    }

    FCircuitBreakerStats FCircuitBreaker::GetStats() const
    {
        FScopeLock Lock(&Mutex);
        return Stats;
    }

    bool FCircuitBreaker::CanExecute()
    {
        FScopeLock Lock(&Mutex);

        if (Stats.CurrentState == ECircuitBreakerState::Open)
        {
            if (HasTimeoutElapsed())
            {
                TransitionTo(ECircuitBreakerState::HalfOpen);
            }
            else
            {
                return false;
            }
        }

        if (Stats.CurrentState == ECircuitBreakerState::Closed)
        {
            return true;
        }

        if (Stats.CurrentState == ECircuitBreakerState::HalfOpen)
        {
            if (HalfOpenActiveRequests >= Config.HalfOpenMaxRequests)
            {
                return false;
            }

            HalfOpenActiveRequests++;
            return true;
        }

        return false;
    }

    void FCircuitBreaker::OnSuccess()
    {
        FScopeLock Lock(&Mutex);

        Stats.TotalSuccesses++;
        Stats.ConsecutiveFailures = 0;

        if (Stats.CurrentState == ECircuitBreakerState::HalfOpen)
        {
            HalfOpenActiveRequests--;
            Stats.ConsecutiveSuccesses++;

            if (Stats.ConsecutiveSuccesses >= Config.SuccessThreshold)
            {
                Stats.ConsecutiveSuccesses = 0;
                TransitionTo(ECircuitBreakerState::Closed);
            }
        }
    }

    void FCircuitBreaker::OnFailure()
    {
        FScopeLock Lock(&Mutex);

        Stats.TotalFailures++;
        Stats.ConsecutiveFailures++;
        Stats.ConsecutiveSuccesses = 0;

        if (Stats.CurrentState == ECircuitBreakerState::HalfOpen)
        {
            HalfOpenActiveRequests--;
            TransitionTo(ECircuitBreakerState::Open);
        }
        else if (Stats.CurrentState == ECircuitBreakerState::Closed)
        {
            if (Stats.ConsecutiveFailures >= Config.FailureThreshold)
            {
                TransitionTo(ECircuitBreakerState::Open);
            }
        }
    }

    void FCircuitBreaker::TransitionTo(ECircuitBreakerState NewState)
    {
        ECircuitBreakerState OldState = Stats.CurrentState;

        if (OldState == NewState)
        {
            return;
        }

        Stats.CurrentState = NewState;
        if (NewState == ECircuitBreakerState::Open)
        {
            Stats.CircuitOpenCount++;
            Stats.CircuitOpenedAt = FDateTime::Now();
            HalfOpenActiveRequests = 0;
        }
        else if (NewState == ECircuitBreakerState::Closed)
        {
            Stats.ConsecutiveFailures = 0;
            HalfOpenActiveRequests = 0;
        }
        else if (NewState == ECircuitBreakerState::HalfOpen)
        {
            HalfOpenActiveRequests = 0;
        }

        LogStateChange(OldState, NewState);
    }

    bool FCircuitBreaker::HasTimeoutElapsed() const
    {
        if (Stats.CurrentState != ECircuitBreakerState::Open)
        {
            return false;
        }

        FTimespan Elapsed = FDateTime::Now() - Stats.CircuitOpenedAt;
        return Elapsed.GetTotalSeconds() >= Config.OpenTimeoutSeconds;
    }

    void FCircuitBreaker::LogStateChange(ECircuitBreakerState OldState, ECircuitBreakerState NewState) const
    {
        if (!Config.bEnableLogging)
        {
            return;
        }

        FString OldStateStr;
        switch (OldState)
        {
        case ECircuitBreakerState::Closed:
            OldStateStr = TEXT("CLOSED");
            break;
        case ECircuitBreakerState::Open:
            OldStateStr = TEXT("OPEN");
            break;
        case ECircuitBreakerState::HalfOpen:
            OldStateStr = TEXT("HALF-OPEN");
            break;
        }

        FString NewStateStr;
        switch (NewState)
        {
        case ECircuitBreakerState::Closed:
            NewStateStr = TEXT("CLOSED");
            break;
        case ECircuitBreakerState::Open:
            NewStateStr = TEXT("OPEN");
            break;
        case ECircuitBreakerState::HalfOpen:
            NewStateStr = TEXT("HALF-OPEN");
            break;
        }

        UE_LOG(LogConvaiEditor, Warning,
               TEXT("CircuitBreaker '%s' state transition: %s → %s | Consecutive Failures: %d | Total Opens: %d"),
               *Config.Name,
               *OldStateStr,
               *NewStateStr,
               Stats.ConsecutiveFailures,
               Stats.CircuitOpenCount);
    }

} // namespace ConvaiEditor
