/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AsyncOperation.h
 *
 * Standardized async operation wrapper with cancellation and progress support.
 */

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Async/CancellationToken.h"
#include "Async/AsyncProgress.h"
#include "Services/ConvaiDIContainer.h" // For TConvaiResult
#include "Async/Async.h"

namespace ConvaiEditor
{
    /**
     * Represents the current state of an async operation.
     */
    enum class EAsyncOperationState : uint8
    {
        /** Operation has not started yet */
        NotStarted,

        /** Operation is currently running */
        Running,

        /** Operation completed successfully */
        Succeeded,

        /** Operation failed with an error */
        Failed,

        /** Operation was cancelled */
        Cancelled
    };

    /**
     * Non-template base class for async operations.
     */
    class CONVAIEDITOR_API FAsyncOperationBase : public TSharedFromThis<FAsyncOperationBase>
    {
    public:
        virtual ~FAsyncOperationBase() = default;

        /** Starts the async operation */
        virtual void Start() = 0;

        /** Cancels the async operation */
        virtual void Cancel() = 0;

        /** Returns true if the operation is currently running */
        virtual bool IsRunning() const = 0;

        /** Returns true if the operation was cancelled */
        virtual bool IsCancelled() const = 0;

        /** Returns true if the operation has completed (success, failure, or cancelled) */
        virtual bool IsComplete() const = 0;

        /** Returns the current state of the operation */
        virtual EAsyncOperationState GetState() const = 0;

        /** Sets the progress reporter for this operation */
        virtual void SetProgressReporter(TSharedPtr<IAsyncProgressReporter> Reporter) = 0;

        /** Gets the cancellation token */
        virtual TSharedPtr<FCancellationToken> GetCancellationToken() const = 0;
    };

    /**
     * Async operation wrapper with cancellation and progress support.
     */
    template <typename TResult>
    class FAsyncOperation : public FAsyncOperationBase
    {
    public:
        /** Delegate type for completion callbacks */
        DECLARE_MULTICAST_DELEGATE_OneParam(FOnComplete, const TConvaiResult<TResult> & /* Result */);

        /** Function signature for the async work */
        using FWorkFunction = TFunction<TConvaiResult<TResult>(TSharedPtr<FCancellationToken>, TSharedPtr<IAsyncProgressReporter>)>;

        /**
         * Constructor.
         *
         * @param WorkFunction The async work to perform
         * @param CancellationToken Optional cancellation token
         */
        FAsyncOperation(FWorkFunction InWorkFunction, TSharedPtr<FCancellationToken> InCancellationToken = nullptr)
            : WorkFunction(MoveTemp(InWorkFunction)), CancellationToken(InCancellationToken), ProgressReporter(FNullProgressReporter::Get()), State(EAsyncOperationState::NotStarted)
        {
            // Create cancellation token if not provided
            if (!CancellationToken.IsValid())
            {
                TSharedPtr<FCancellationTokenSource> TokenSource = MakeShared<FCancellationTokenSource>();
                CancellationToken = TokenSource->GetToken();
                OwnedTokenSource = TokenSource; // Keep alive
            }
        }

        virtual ~FAsyncOperation()
        {
            // Cancel if still running
            if (IsRunning())
            {
                Cancel();
            }
        }

        //~ Begin FAsyncOperationBase Interface
        virtual void Start() override
        {
            FScopeLock Lock(&StateMutex);

            if (State != EAsyncOperationState::NotStarted)
            {
                return;
            }

            if (!WorkFunction)
            {
                UE_LOG(LogConvaiEditor, Error, TEXT("AsyncOperation: No work function provided"));
                SetStateLocked(EAsyncOperationState::Failed);
                Result = TConvaiResult<TResult>::Failure(TEXT("No work function provided"));
                BroadcastCompletion();
                return;
            }

            SetStateLocked(EAsyncOperationState::Running);

            // Execute work on thread pool
            TWeakPtr<FAsyncOperation<TResult>> WeakOperation = StaticCastSharedRef<FAsyncOperation<TResult>>(AsShared());
            Async(EAsyncExecution::ThreadPool,
                  [WeakOperation]()
                  {
                      if (TSharedPtr<FAsyncOperation<TResult>> This = WeakOperation.Pin())
                      {
                          This->ExecuteWork();
                      }
                  });
        }

        virtual void Cancel() override
        {
            // Use owned token source if available, otherwise we can't cancel external tokens
            if (OwnedTokenSource.IsValid())
            {
                OwnedTokenSource->Cancel();
            }
            else if (CancellationToken.IsValid() && CancellationToken->IsCancellationRequested())
            {
                // Token is already cancelled by external source
            }
            else
            {
                UE_LOG(LogConvaiEditor, Warning, TEXT("AsyncOperation: Cannot cancel - external token without source"));
            }

            // Complete the operation if running
            TConvaiResult<TResult> CancelResult;
            bool bShouldComplete = false;
            {
                FScopeLock Lock(&StateMutex);
                if (State == EAsyncOperationState::Running || State == EAsyncOperationState::NotStarted)
                {
                    SetStateLocked(EAsyncOperationState::Cancelled);
                    bShouldComplete = true;
                }
            }

            // Complete with cancellation error (outside of lock to avoid deadlock)
            if (bShouldComplete)
            {
                // Store result
                {
                    FScopeLock Lock(&ResultMutex);
                    Result = TConvaiResult<TResult>::Failure(TEXT("Operation cancelled"));
                    CancelResult = Result;
                }

                // Broadcast completion on game thread WITHOUT using AsShared() to avoid lifetime issues
                // We schedule the broadcast on game thread, capturing delegate by value
                {
                    FScopeLock Lock(&CompletionMutex);
                    // Make a copy of the delegate to broadcast
                    FOnComplete DelegateCopy = OnCompleteDelegate;

                    // Schedule broadcast on game thread with captured delegate
                    AsyncTask(ENamedThreads::GameThread, [DelegateCopy, CancelResult]()
                              {
						// Broadcast to all registered callbacks
						const_cast<FOnComplete&>(DelegateCopy).Broadcast(CancelResult); });
                }
            }
        }

        virtual bool IsRunning() const override
        {
            FScopeLock Lock(&StateMutex);
            return State == EAsyncOperationState::Running;
        }

        virtual bool IsCancelled() const override
        {
            FScopeLock Lock(&StateMutex);
            return State == EAsyncOperationState::Cancelled;
        }

        virtual bool IsComplete() const override
        {
            FScopeLock Lock(&StateMutex);
            return State == EAsyncOperationState::Succeeded ||
                   State == EAsyncOperationState::Failed ||
                   State == EAsyncOperationState::Cancelled;
        }

        virtual EAsyncOperationState GetState() const override
        {
            FScopeLock Lock(&StateMutex);
            return State;
        }

        virtual void SetProgressReporter(TSharedPtr<IAsyncProgressReporter> Reporter) override
        {
            if (Reporter.IsValid())
            {
                ProgressReporter = Reporter;
            }
            else
            {
                ProgressReporter = FNullProgressReporter::Get();
            }
        }

        virtual TSharedPtr<FCancellationToken> GetCancellationToken() const override
        {
            return CancellationToken;
        }
        //~ End FAsyncOperationBase Interface

        /**
         * Registers a completion callback.
         *
         * @param Callback Function to call on completion
         * @return Delegate handle for unregistering
         */
        FDelegateHandle OnComplete(TFunction<void(const TConvaiResult<TResult> &)> Callback)
        {
            FScopeLock Lock(&CompletionMutex);

            // If already complete, invoke immediately on game thread
            if (IsComplete())
            {
                TConvaiResult<TResult> CurrentResult = GetResult();
                AsyncTask(ENamedThreads::GameThread, [Callback = MoveTemp(Callback), CurrentResult]()
                          { Callback(CurrentResult); });
                return FDelegateHandle(); // Invalid handle since already invoked
            }

            return OnCompleteDelegate.AddLambda(MoveTemp(Callback));
        }

        /**
         * Gets the result of the operation.
         *
         * @return The operation result
         */
        TConvaiResult<TResult> GetResult() const
        {
            // Wait for completion (with timeout to prevent deadlock)
            const double StartTime = FPlatformTime::Seconds();
            const double TimeoutSeconds = 60.0; // 60 second timeout

            while (!IsComplete())
            {
                if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
                {
                    UE_LOG(LogConvaiEditor, Error, TEXT("AsyncOperation: GetResult() timed out"));
                    return TConvaiResult<TResult>::Failure(TEXT("Operation timed out"));
                }
                FPlatformProcess::Sleep(0.01f); // 10ms sleep
            }

            FScopeLock Lock(&ResultMutex);
            return Result;
        }

        /**
         * Chains another operation to execute after this one completes successfully.
         *
         * @param Continuation Function to create the next operation
         * @return The chained operation
         */
        template <typename TNext>
        TSharedPtr<FAsyncOperation<TNext>> Then(TFunction<TConvaiResult<TNext>(const TResult &)> Continuation)
        {
            TSharedPtr<FAsyncOperation<TNext>> NextOperation = MakeShared<FAsyncOperation<TNext>>(
                [Continuation = MoveTemp(Continuation)](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<TNext>
                {
                    // This will be populated by the OnComplete callback
                    // For now, return a placeholder
                    return TConvaiResult<TNext>::Failure(TEXT("Internal error: continuation not set up"));
                },
                CancellationToken);

            // When this operation completes, execute the continuation
            TWeakPtr<FAsyncOperation<TNext>> WeakNext = NextOperation;
            OnComplete([Continuation, WeakNext](const TConvaiResult<TResult> &Result)
                       {
				if (TSharedPtr<FAsyncOperation<TNext>> Next = WeakNext.Pin())
				{
					if (Result.IsSuccess())
					{
						// Execute continuation with the result
						TConvaiResult<TNext> NextResult = Continuation(Result.GetValue());
						Next->CompleteWith(NextResult);
					}
					else
					{
						// Propagate error
						Next->CompleteWith(TConvaiResult<TNext>::Failure(Result.GetError()));
					}
				} });

            return NextOperation;
        }

    private:
        /** Executes the work function and handles completion */
        void ExecuteWork()
        {

            TConvaiResult<TResult> WorkResult;

            try
            {
                // Check cancellation before starting
                if (CancellationToken.IsValid() && CancellationToken->IsCancellationRequested())
                {
                    WorkResult = TConvaiResult<TResult>::Failure(TEXT("Operation cancelled before execution"));
                }
                else
                {
                    // Execute the work
                    WorkResult = WorkFunction(CancellationToken, ProgressReporter);
                }
            }
            catch (const std::exception &Ex)
            {
                UE_LOG(LogConvaiEditor, Error, TEXT("AsyncOperation: Exception during execution: %hs"), Ex.what());
                WorkResult = TConvaiResult<TResult>::Failure(FString::Printf(TEXT("Exception: %hs"), Ex.what()));
            }
            catch (...)
            {
                UE_LOG(LogConvaiEditor, Error, TEXT("AsyncOperation: Unknown exception during execution"));
                WorkResult = TConvaiResult<TResult>::Failure(TEXT("Unknown exception"));
            }

            // Complete the operation
            CompleteWith(WorkResult);
        }

        /** Completes the operation with a result */
        void CompleteWith(const TConvaiResult<TResult> &InResult)
        {
            {
                FScopeLock Lock(&StateMutex);
                if (State != EAsyncOperationState::Running && State != EAsyncOperationState::NotStarted)
                {
                    return; // Already completed or cancelled
                }

                // Determine final state
                if (CancellationToken.IsValid() && CancellationToken->IsCancellationRequested())
                {
                    SetStateLocked(EAsyncOperationState::Cancelled);
                }
                else if (InResult.IsSuccess())
                {
                    SetStateLocked(EAsyncOperationState::Succeeded);
                }
                else
                {
                    SetStateLocked(EAsyncOperationState::Failed);
                }
            }

            // Store result
            {
                FScopeLock Lock(&ResultMutex);
                Result = InResult;
            }

            // Broadcast completion
            BroadcastCompletion();
        }

        /** Broadcasts completion to all registered callbacks on game thread */
        void BroadcastCompletion()
        {
            TConvaiResult<TResult> FinalResult = GetResult();
            TWeakPtr<FAsyncOperation<TResult>> WeakOperation = StaticCastSharedRef<FAsyncOperation<TResult>>(AsShared());

            AsyncTask(ENamedThreads::GameThread, [WeakOperation, FinalResult]()
                      {
				if (TSharedPtr<FAsyncOperation<TResult>> This = WeakOperation.Pin())
				{
					FScopeLock Lock(&This->CompletionMutex);
					This->OnCompleteDelegate.Broadcast(FinalResult);
				} });
        }

        /** Sets the state (assumes StateMutex is locked) */
        void SetStateLocked(EAsyncOperationState NewState)
        {
            State = NewState;
        }

        /** The async work function */
        FWorkFunction WorkFunction;

        /** Cancellation token */
        TSharedPtr<FCancellationToken> CancellationToken;

        /** Owned token source (if we created the token) */
        TSharedPtr<FCancellationTokenSource> OwnedTokenSource;

        /** Progress reporter */
        TSharedPtr<IAsyncProgressReporter> ProgressReporter;

        /** Current operation state */
        EAsyncOperationState State;

        /** Protects State */
        mutable FCriticalSection StateMutex;

        /** Operation result */
        TConvaiResult<TResult> Result;

        /** Protects Result */
        mutable FCriticalSection ResultMutex;

        /** Completion delegate */
        FOnComplete OnCompleteDelegate;

        /** Protects completion delegate */
        mutable FCriticalSection CompletionMutex;
    };

    /**
     * Async operation specialization for void return type.
     */
    template <>
    class FAsyncOperation<void> : public FAsyncOperationBase
    {
    public:
        DECLARE_MULTICAST_DELEGATE_OneParam(FOnComplete, const TConvaiResult<void> & /* Result */);
        using FWorkFunction = TFunction<TConvaiResult<void>(TSharedPtr<FCancellationToken>, TSharedPtr<IAsyncProgressReporter>)>;

        FAsyncOperation(FWorkFunction InWorkFunction, TSharedPtr<FCancellationToken> InCancellationToken = nullptr);
        virtual ~FAsyncOperation();

        virtual void Start() override;
        virtual void Cancel() override;
        virtual bool IsRunning() const override;
        virtual bool IsCancelled() const override;
        virtual bool IsComplete() const override;
        virtual EAsyncOperationState GetState() const override;
        virtual void SetProgressReporter(TSharedPtr<IAsyncProgressReporter> Reporter) override;
        virtual TSharedPtr<FCancellationToken> GetCancellationToken() const override;

        FDelegateHandle OnComplete(TFunction<void(const TConvaiResult<void> &)> Callback);
        TConvaiResult<void> GetResult() const;

        template <typename TNext>
        TSharedPtr<FAsyncOperation<TNext>> Then(TFunction<TConvaiResult<TNext>()> Continuation);

    private:
        void ExecuteWork();
        void CompleteWith(const TConvaiResult<void> &InResult);
        void BroadcastCompletion();
        void SetStateLocked(EAsyncOperationState NewState);

        FWorkFunction WorkFunction;
        TSharedPtr<FCancellationToken> CancellationToken;
        TSharedPtr<FCancellationTokenSource> OwnedTokenSource;
        TSharedPtr<IAsyncProgressReporter> ProgressReporter;
        EAsyncOperationState State;
        mutable FCriticalSection StateMutex;
        TConvaiResult<void> Result;
        mutable FCriticalSection ResultMutex;
        FOnComplete OnCompleteDelegate;
        mutable FCriticalSection CompletionMutex;
    };

} // namespace ConvaiEditor
