/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AsyncHelpers.h
 *
 * Utility functions for composing and managing async operations.
 */

#pragma once

#include "CoreMinimal.h"
#include "Async/AsyncOperation.h"
#include "Async/CancellationToken.h"
#include "Async/AsyncProgress.h"

namespace ConvaiEditor
{
    /**
     * Utility functions for composing and managing async operations.
     */
    namespace AsyncHelpers
    {
        /** Creates an async operation that completes when all provided operations complete */
        template <typename T>
        CONVAIEDITOR_API TSharedPtr<FAsyncOperation<TArray<T>>> WhenAll(
            TArray<TSharedPtr<FAsyncOperation<T>>> Operations,
            TSharedPtr<FCancellationToken> CancellationToken = nullptr);

        /**
         * Creates an async operation that completes when any of the provided operations complete.
         *
         * @param Operations Array of async operations to race
         * @param CancellationToken Optional cancellation token
         * @return An async operation that completes with the first result
         */
        template <typename T>
        CONVAIEDITOR_API TSharedPtr<FAsyncOperation<T>> WhenAny(
            TArray<TSharedPtr<FAsyncOperation<T>>> Operations,
            TSharedPtr<FCancellationToken> CancellationToken = nullptr);

        /**
         * Creates an async operation that completes after a specified delay.
         *
         * @param DelaySeconds Duration of the delay in seconds
         * @param CancellationToken Optional cancellation token
         * @return An async operation that completes after the delay
         */
        CONVAIEDITOR_API TSharedPtr<FAsyncOperation<void>> Delay(
            float DelaySeconds,
            TSharedPtr<FCancellationToken> CancellationToken = nullptr);

        /**
         * Wraps an async operation with a timeout.
         *
         * @param Operation The operation to wrap with a timeout
         * @param TimeoutSeconds Timeout duration in seconds
         * @return An async operation that fails if timeout is reached
         */
        template <typename T>
        CONVAIEDITOR_API TSharedPtr<FAsyncOperation<T>> WithTimeout(
            TSharedPtr<FAsyncOperation<T>> Operation,
            float TimeoutSeconds);

        /**
         * Wraps an operation factory with automatic retry logic.
         *
         * @param OperationFactory Function that creates a new operation instance for each attempt
         * @param MaxAttempts Maximum number of attempts (including initial try)
         * @param ShouldRetry Optional predicate to determine if retry should be attempted
         * @param CancellationToken Optional cancellation token
         * @return An async operation that retries on failure
         */
        template <typename T>
        CONVAIEDITOR_API TSharedPtr<FAsyncOperation<T>> WithRetry(
            TFunction<TSharedPtr<FAsyncOperation<T>>()> OperationFactory,
            int32 MaxAttempts = 3,
            TFunction<bool(const TConvaiResult<T> &)> ShouldRetry = nullptr,
            TSharedPtr<FCancellationToken> CancellationToken = nullptr);

        /**
         * Creates a sequence of async operations that execute one after another.
         *
         * @param OperationFactories Array of factory functions that create operations
         * @param CancellationToken Optional cancellation token
         * @return An async operation that returns all results in order
         */
        template <typename T>
        CONVAIEDITOR_API TSharedPtr<FAsyncOperation<TArray<T>>> Sequence(
            TArray<TFunction<TSharedPtr<FAsyncOperation<T>>()>> OperationFactories,
            TSharedPtr<FCancellationToken> CancellationToken = nullptr);

        /**
         * Creates an already-completed async operation with a success value.
         *
         * @param Value The value to return
         * @return A completed async operation
         */
        template <typename T>
        CONVAIEDITOR_API TSharedPtr<FAsyncOperation<T>> FromValue(const T &Value);

        /**
         * Creates an already-failed async operation with an error.
         *
         * @param Error The error message
         * @return A failed async operation
         */
        template <typename T>
        CONVAIEDITOR_API TSharedPtr<FAsyncOperation<T>> FromError(const FString &Error);

        // IMPLEMENTATION (Inline for Templates)

        template <typename T>
        TSharedPtr<FAsyncOperation<TArray<T>>> WhenAll(
            TArray<TSharedPtr<FAsyncOperation<T>>> Operations,
            TSharedPtr<FCancellationToken> CancellationToken)
        {
            return MakeShared<FAsyncOperation<TArray<T>>>(
                [Operations, CancellationToken](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<TArray<T>>
                {
                    if (Operations.Num() == 0)
                    {
                        return TConvaiResult<TArray<T>>::Success(TArray<T>());
                    }

                    Progress->ReportStage(TEXT("WhenAll"));
                    Progress->ReportProgress(0.0f, FString::Printf(TEXT("Waiting for %d operations..."), Operations.Num()));

                    // Start all operations
                    for (auto &Op : Operations)
                    {
                        if (Op.IsValid())
                        {
                            Op->Start();
                        }
                    }

                    // Wait for all to complete
                    TArray<T> Results;
                    Results.Reserve(Operations.Num());

                    for (int32 i = 0; i < Operations.Num(); ++i)
                    {
                        if (Token.IsValid() && Token->IsCancellationRequested())
                        {
                            // Cancel remaining operations
                            for (int32 j = i; j < Operations.Num(); ++j)
                            {
                                if (Operations[j].IsValid())
                                {
                                    Operations[j]->Cancel();
                                }
                            }
                            return TConvaiResult<TArray<T>>::Failure(TEXT("WhenAll cancelled"));
                        }

                        auto &Op = Operations[i];
                        if (!Op.IsValid())
                        {
                            return TConvaiResult<TArray<T>>::Failure(FString::Printf(TEXT("Operation %d is invalid"), i));
                        }

                        TConvaiResult<T> Result = Op->GetResult(); // Blocks until complete

                        if (!Result.IsSuccess())
                        {
                            // Cancel remaining operations on first failure
                            for (int32 j = i + 1; j < Operations.Num(); ++j)
                            {
                                if (Operations[j].IsValid())
                                {
                                    Operations[j]->Cancel();
                                }
                            }
                            return TConvaiResult<TArray<T>>::Failure(FString::Printf(TEXT("Operation %d failed: %s"), i, *Result.GetError()));
                        }

                        Results.Add(Result.GetValue());
                        Progress->ReportProgress(static_cast<float>(i + 1) / Operations.Num(), FString::Printf(TEXT("Completed %d/%d operations"), i + 1, Operations.Num()));
                    }

                    return TConvaiResult<TArray<T>>::Success(MoveTemp(Results));
                },
                CancellationToken);
        }

        template <typename T>
        TSharedPtr<FAsyncOperation<T>> WhenAny(
            TArray<TSharedPtr<FAsyncOperation<T>>> Operations,
            TSharedPtr<FCancellationToken> CancellationToken)
        {
            return MakeShared<FAsyncOperation<T>>(
                [Operations, CancellationToken](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<T>
                {
                    if (Operations.Num() == 0)
                    {
                        return TConvaiResult<T>::Failure(TEXT("No operations provided to WhenAny"));
                    }

                    Progress->ReportStage(TEXT("WhenAny"));
                    Progress->ReportProgress(0.0f, FString::Printf(TEXT("Racing %d operations..."), Operations.Num()));

                    // Start all operations
                    for (auto &Op : Operations)
                    {
                        if (Op.IsValid())
                        {
                            Op->Start();
                        }
                    }

                    // Poll for first completion
                    TConvaiResult<T> FirstResult = TConvaiResult<T>::Failure(TEXT("All operations failed or invalid"));
                    bool bAnyCompleted = false;

                    while (!bAnyCompleted)
                    {
                        if (Token.IsValid() && Token->IsCancellationRequested())
                        {
                            // Cancel all operations
                            for (auto &Op : Operations)
                            {
                                if (Op.IsValid())
                                {
                                    Op->Cancel();
                                }
                            }
                            return TConvaiResult<T>::Failure(TEXT("WhenAny cancelled"));
                        }

                        for (auto &Op : Operations)
                        {
                            if (Op.IsValid() && Op->IsComplete())
                            {
                                FirstResult = Op->GetResult();
                                bAnyCompleted = true;

                                // Cancel remaining operations
                                for (auto &OtherOp : Operations)
                                {
                                    if (OtherOp.IsValid() && OtherOp != Op)
                                    {
                                        OtherOp->Cancel();
                                    }
                                }
                                break;
                            }
                        }

                        if (!bAnyCompleted)
                        {
                            FPlatformProcess::Sleep(0.01f); // 10ms poll interval
                        }
                    }

                    Progress->ReportProgress(1.0f, TEXT("First operation completed"));
                    return FirstResult;
                },
                CancellationToken);
        }

        template <typename T>
        TSharedPtr<FAsyncOperation<T>> WithTimeout(
            TSharedPtr<FAsyncOperation<T>> Operation,
            float TimeoutSeconds)
        {
            if (!Operation.IsValid())
            {
                return FromError<T>(TEXT("Invalid operation provided to WithTimeout"));
            }

            return MakeShared<FAsyncOperation<T>>(
                [Operation, TimeoutSeconds](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<T>
                {
                    Progress->ReportStage(TEXT("WithTimeout"));

                    // Start the operation
                    Operation->Start();

                    const double StartTime = FPlatformTime::Seconds();

                    // Wait for completion or timeout
                    while (!Operation->IsComplete())
                    {
                        if (Token.IsValid() && Token->IsCancellationRequested())
                        {
                            Operation->Cancel();
                            return TConvaiResult<T>::Failure(TEXT("Operation cancelled"));
                        }

                        const double ElapsedTime = FPlatformTime::Seconds() - StartTime;
                        if (ElapsedTime >= TimeoutSeconds)
                        {
                            Operation->Cancel();
                            return TConvaiResult<T>::Failure(FString::Printf(TEXT("Operation timed out after %.1f seconds"), TimeoutSeconds));
                        }

                        Progress->ReportProgress(static_cast<float>(ElapsedTime / TimeoutSeconds), FString::Printf(TEXT("%.1fs / %.1fs"), ElapsedTime, TimeoutSeconds));
                        FPlatformProcess::Sleep(0.01f); // 10ms poll interval
                    }

                    return Operation->GetResult();
                },
                Operation->GetCancellationToken());
        }

        template <typename T>
        TSharedPtr<FAsyncOperation<T>> WithRetry(
            TFunction<TSharedPtr<FAsyncOperation<T>>()> OperationFactory,
            int32 MaxAttempts,
            TFunction<bool(const TConvaiResult<T> &)> ShouldRetry,
            TSharedPtr<FCancellationToken> CancellationToken)
        {
            if (!OperationFactory)
            {
                return FromError<T>(TEXT("Invalid operation factory provided to WithRetry"));
            }

            // Default retry predicate: retry on any failure
            if (!ShouldRetry)
            {
                ShouldRetry = [](const TConvaiResult<T> &Result)
                { return !Result.IsSuccess(); };
            }

            return MakeShared<FAsyncOperation<T>>(
                [OperationFactory, MaxAttempts, ShouldRetry](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<T>
                {
                    Progress->ReportStage(TEXT("WithRetry"));

                    TConvaiResult<T> LastResult = TConvaiResult<T>::Failure(TEXT("No attempts made"));

                    for (int32 Attempt = 1; Attempt <= MaxAttempts; ++Attempt)
                    {
                        if (Token.IsValid() && Token->IsCancellationRequested())
                        {
                            return TConvaiResult<T>::Failure(TEXT("Retry cancelled"));
                        }

                        Progress->ReportProgress(static_cast<float>(Attempt - 1) / MaxAttempts, FString::Printf(TEXT("Attempt %d/%d"), Attempt, MaxAttempts));

                        // Create and execute operation
                        TSharedPtr<FAsyncOperation<T>> Operation = OperationFactory();
                        if (!Operation.IsValid())
                        {
                            return TConvaiResult<T>::Failure(FString::Printf(TEXT("Operation factory returned null on attempt %d"), Attempt));
                        }

                        Operation->Start();
                        LastResult = Operation->GetResult();

                        // Check if successful or should not retry
                        if (LastResult.IsSuccess() || !ShouldRetry(LastResult))
                        {
                            Progress->ReportProgress(1.0f, FString::Printf(TEXT("Completed on attempt %d"), Attempt));
                            return LastResult;
                        }

                        // Wait before retry (except on last attempt)
                        if (Attempt < MaxAttempts)
                        {
                            const float DelaySeconds = FMath::Min(1.0f * FMath::Pow(2.0f, Attempt - 1), 10.0f); // Exponential backoff
                            FPlatformProcess::Sleep(DelaySeconds);
                        }
                    }

                    // All attempts exhausted
                    return TConvaiResult<T>::Failure(FString::Printf(TEXT("All %d retry attempts failed. Last error: %s"), MaxAttempts, *LastResult.GetError()));
                },
                CancellationToken);
        }

        template <typename T>
        TSharedPtr<FAsyncOperation<TArray<T>>> Sequence(
            TArray<TFunction<TSharedPtr<FAsyncOperation<T>>()>> OperationFactories,
            TSharedPtr<FCancellationToken> CancellationToken)
        {
            return MakeShared<FAsyncOperation<TArray<T>>>(
                [OperationFactories, CancellationToken](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<TArray<T>>
                {
                    if (OperationFactories.Num() == 0)
                    {
                        return TConvaiResult<TArray<T>>::Success(TArray<T>());
                    }

                    Progress->ReportStage(TEXT("Sequence"));

                    TArray<T> Results;
                    Results.Reserve(OperationFactories.Num());

                    for (int32 i = 0; i < OperationFactories.Num(); ++i)
                    {
                        if (Token.IsValid() && Token->IsCancellationRequested())
                        {
                            return TConvaiResult<TArray<T>>::Failure(TEXT("Sequence cancelled"));
                        }

                        Progress->ReportProgress(static_cast<float>(i) / OperationFactories.Num(), FString::Printf(TEXT("Step %d/%d"), i + 1, OperationFactories.Num()));

                        TSharedPtr<FAsyncOperation<T>> Operation = OperationFactories[i]();
                        if (!Operation.IsValid())
                        {
                            return TConvaiResult<TArray<T>>::Failure(FString::Printf(TEXT("Step %d returned null operation"), i + 1));
                        }

                        Operation->Start();
                        TConvaiResult<T> Result = Operation->GetResult();

                        if (!Result.IsSuccess())
                        {
                            return TConvaiResult<TArray<T>>::Failure(FString::Printf(TEXT("Step %d failed: %s"), i + 1, *Result.GetError()));
                        }

                        Results.Add(Result.GetValue());
                    }

                    Progress->ReportProgress(1.0f, TEXT("Sequence completed"));
                    return TConvaiResult<TArray<T>>::Success(MoveTemp(Results));
                },
                CancellationToken);
        }

        template <typename T>
        TSharedPtr<FAsyncOperation<T>> FromValue(const T &Value)
        {
            auto Operation = MakeShared<FAsyncOperation<T>>(
                [Value](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<T>
                {
                    return TConvaiResult<T>::Success(Value);
                });

            // Start immediately (will complete synchronously)
            Operation->Start();
            return Operation;
        }

        template <typename T>
        TSharedPtr<FAsyncOperation<T>> FromError(const FString &Error)
        {
            auto Operation = MakeShared<FAsyncOperation<T>>(
                [Error](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<T>
                {
                    return TConvaiResult<T>::Failure(Error);
                });

            // Start immediately (will complete synchronously)
            Operation->Start();
            return Operation;
        }

    } // namespace AsyncHelpers

} // namespace ConvaiEditor
