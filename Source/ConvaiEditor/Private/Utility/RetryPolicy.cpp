/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * RetryPolicy.cpp
 *
 * Implementation of retry policy with exponential backoff
 */

#include "Utility/RetryPolicy.h"
#include "ConvaiEditor.h"
#include "Math/UnrealMathUtility.h"

namespace ConvaiEditor
{

    FString FRetryStats::GetSummary() const
    {
        float SuccessRate = TotalOperations > 0
                                ? (static_cast<float>(ImmediateSuccesses + SuccessAfterRetry) / TotalOperations) * 100.0f
                                : 0.0f;

        return FString::Printf(
            TEXT("Total: %d | Immediate Success: %d | Success After Retry: %d | Failures: %d | Retry Attempts: %d | Success Rate: %.1f%%"),
            TotalOperations,
            ImmediateSuccesses,
            SuccessAfterRetry,
            TotalFailures,
            TotalRetryAttempts,
            SuccessRate);
    }

    void FRetryStats::Reset()
    {
        TotalOperations = 0;
        ImmediateSuccesses = 0;
        SuccessAfterRetry = 0;
        TotalFailures = 0;
        TotalRetryAttempts = 0;
    }

    FRetryPolicy::FRetryPolicy(const FRetryPolicyConfig &InConfig)
        : Config(InConfig)
    {
    }

    TConvaiResult<void> FRetryPolicy::Execute(TFunction<TConvaiResult<void>()> Operation)
    {
        int32 Attempt = 0;
        TConvaiResult<void> Result;

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

    FRetryStats FRetryPolicy::GetStats() const
    {
        FScopeLock Lock(&StatsMutex);
        return Stats;
    }

    void FRetryPolicy::ResetStats()
    {
        FScopeLock Lock(&StatsMutex);
        Stats.Reset();
    }

    float FRetryPolicy::CalculateDelay(int32 AttemptNumber) const
    {
        float Delay = 0.0f;

        switch (Config.Strategy)
        {
        case ERetryStrategy::None:
            Delay = 0.0f;
            break;

        case ERetryStrategy::Fixed:
            Delay = Config.BaseDelaySeconds;
            break;

        case ERetryStrategy::Exponential:
            Delay = FMath::Pow(2.0f, static_cast<float>(AttemptNumber - 1)) * Config.BaseDelaySeconds;
            break;

        case ERetryStrategy::Linear:
            Delay = static_cast<float>(AttemptNumber) * Config.BaseDelaySeconds;
            break;
        }

        Delay = FMath::Min(Delay, Config.MaxDelaySeconds);

        if (Config.bEnableJitter)
        {
            Delay = AddJitter(Delay);
        }

        return Delay;
    }

    float FRetryPolicy::AddJitter(float Delay) const
    {
        float Jitter = FMath::FRand() * Config.JitterMaxSeconds;
        return Delay + Jitter;
    }

    void FRetryPolicy::RecordSuccess(int32 AttemptNumber)
    {
        FScopeLock Lock(&StatsMutex);

        Stats.TotalOperations++;

        if (AttemptNumber == 0)
        {
            Stats.ImmediateSuccesses++;
        }
        else
        {
            Stats.SuccessAfterRetry++;
            Stats.TotalRetryAttempts += AttemptNumber;
        }
    }

    void FRetryPolicy::RecordFailure(int32 AttemptCount)
    {
        FScopeLock Lock(&StatsMutex);

        Stats.TotalOperations++;
        Stats.TotalFailures++;
        Stats.TotalRetryAttempts += AttemptCount;
    }

    void FRetryPolicy::LogRetryAttempt(const FString &ErrorMessage, int32 AttemptNumber, float Delay) const
    {
        if (!Config.bEnableLogging)
        {
            return;
        }

        UE_LOG(LogConvaiEditor, Warning,
               TEXT("Retry attempt %d/%d failed: %s"),
               AttemptNumber,
               Config.MaxAttempts,
               *ErrorMessage);
    }

    void FRetryPolicy::LogRetryExhausted(const FString &ErrorMessage, int32 TotalAttempts) const
    {
        if (!Config.bEnableLogging)
        {
            return;
        }

        UE_LOG(LogConvaiEditor, Error,
               TEXT("Retry exhausted all %d attempts: %s"),
               TotalAttempts + 1,
               *ErrorMessage);
    }

    void FRetryPolicy::LogRetrySkipped(const FString &ErrorMessage, int32 AttemptNumber) const
    {
        if (!Config.bEnableLogging)
        {
            return;
        }
    }

    namespace RetryPredicates
    {
        bool OnlyNetworkErrors(const FString &ErrorMessage, int32 AttemptNumber)
        {
            return ErrorMessage.Contains(TEXT("network"), ESearchCase::IgnoreCase) ||
                   ErrorMessage.Contains(TEXT("connection"), ESearchCase::IgnoreCase) ||
                   ErrorMessage.Contains(TEXT("unreachable"), ESearchCase::IgnoreCase) ||
                   ErrorMessage.Contains(TEXT("DNS"), ESearchCase::IgnoreCase);
        }

        bool OnlyTimeoutErrors(const FString &ErrorMessage, int32 AttemptNumber)
        {
            return ErrorMessage.Contains(TEXT("timeout"), ESearchCase::IgnoreCase) ||
                   ErrorMessage.Contains(TEXT("timed out"), ESearchCase::IgnoreCase);
        }

        bool OnlyTransientErrors(const FString &ErrorMessage, int32 AttemptNumber)
        {
            return OnlyNetworkErrors(ErrorMessage, AttemptNumber) ||
                   OnlyTimeoutErrors(ErrorMessage, AttemptNumber) ||
                   ErrorMessage.Contains(TEXT("500")) ||
                   ErrorMessage.Contains(TEXT("502")) ||
                   ErrorMessage.Contains(TEXT("503")) ||
                   ErrorMessage.Contains(TEXT("504")) ||
                   ErrorMessage.Contains(TEXT("temporary"), ESearchCase::IgnoreCase) ||
                   ErrorMessage.Contains(TEXT("transient"), ESearchCase::IgnoreCase);
        }

        bool NeverRetry(const FString &ErrorMessage, int32 AttemptNumber)
        {
            return false;
        }

        bool AlwaysRetry(const FString &ErrorMessage, int32 AttemptNumber)
        {
            return true;
        }
    }

} // namespace ConvaiEditor
