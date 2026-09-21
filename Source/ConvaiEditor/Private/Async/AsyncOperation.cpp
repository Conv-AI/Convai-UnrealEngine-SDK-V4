/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AsyncOperation.cpp
 *
 * Implementation of async operation specialization for void return type.
 */

#include "Async/AsyncOperation.h"
#include "ConvaiEditor.h"

namespace ConvaiEditor
{
    FAsyncOperation<void>::FAsyncOperation(FWorkFunction InWorkFunction, TSharedPtr<FCancellationToken> InCancellationToken)
        : WorkFunction(MoveTemp(InWorkFunction)), CancellationToken(InCancellationToken), ProgressReporter(FNullProgressReporter::Get()), State(EAsyncOperationState::NotStarted)
    {
        if (!CancellationToken.IsValid())
        {
            TSharedPtr<FCancellationTokenSource> TokenSource = MakeShared<FCancellationTokenSource>();
            CancellationToken = TokenSource->GetToken();
            OwnedTokenSource = TokenSource;
        }
    }

    FAsyncOperation<void>::~FAsyncOperation()
    {
        if (IsRunning())
        {
            Cancel();
        }
    }

    void FAsyncOperation<void>::Start()
    {
        FScopeLock Lock(&StateMutex);

        if (State != EAsyncOperationState::NotStarted)
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("AsyncOperation: Cannot start operation - already in progress"));
            return;
        }

        if (!WorkFunction)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("AsyncOperation: Cannot start operation - no work function provided"));
            SetStateLocked(EAsyncOperationState::Failed);
            Result = TConvaiResult<void>::Failure(TEXT("No work function provided"));
            BroadcastCompletion();
            return;
        }

        SetStateLocked(EAsyncOperationState::Running);

        TWeakPtr<FAsyncOperation<void>> WeakOperation = StaticCastSharedRef<FAsyncOperation<void>>(AsShared());
        Async(EAsyncExecution::ThreadPool,
              [WeakOperation]()
              {
                  if (TSharedPtr<FAsyncOperation<void>> This = WeakOperation.Pin())
                  {
                      This->ExecuteWork();
                  }
              });
    }

    void FAsyncOperation<void>::Cancel()
    {
        if (OwnedTokenSource.IsValid())
        {
            OwnedTokenSource->Cancel();
        }
        else if (CancellationToken.IsValid() && CancellationToken->IsCancellationRequested())
        {
        }
        else
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("AsyncOperation: Cannot cancel operation - using external cancellation token"));
        }

        bool bShouldComplete = false;
        {
            FScopeLock Lock(&StateMutex);
            if (State == EAsyncOperationState::Running || State == EAsyncOperationState::NotStarted)
            {
                SetStateLocked(EAsyncOperationState::Cancelled);
                bShouldComplete = true;
            }
        }

        if (bShouldComplete)
        {
            TConvaiResult<void> CancelResult;
            {
                FScopeLock Lock(&ResultMutex);
                Result = TConvaiResult<void>::Failure(TEXT("Operation cancelled"));
                CancelResult = Result;
            }

            {
                FScopeLock Lock(&CompletionMutex);
                FOnComplete DelegateCopy = OnCompleteDelegate;

                AsyncTask(ENamedThreads::GameThread, [DelegateCopy, CancelResult]()
                          { const_cast<FOnComplete &>(DelegateCopy).Broadcast(CancelResult); });
            }
        }
    }

    bool FAsyncOperation<void>::IsRunning() const
    {
        FScopeLock Lock(&StateMutex);
        return State == EAsyncOperationState::Running;
    }

    bool FAsyncOperation<void>::IsCancelled() const
    {
        FScopeLock Lock(&StateMutex);
        return State == EAsyncOperationState::Cancelled;
    }

    bool FAsyncOperation<void>::IsComplete() const
    {
        FScopeLock Lock(&StateMutex);
        return State == EAsyncOperationState::Succeeded ||
               State == EAsyncOperationState::Failed ||
               State == EAsyncOperationState::Cancelled;
    }

    EAsyncOperationState FAsyncOperation<void>::GetState() const
    {
        FScopeLock Lock(&StateMutex);
        return State;
    }

    void FAsyncOperation<void>::SetProgressReporter(TSharedPtr<IAsyncProgressReporter> Reporter)
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

    TSharedPtr<FCancellationToken> FAsyncOperation<void>::GetCancellationToken() const
    {
        return CancellationToken;
    }

    FDelegateHandle FAsyncOperation<void>::OnComplete(TFunction<void(const TConvaiResult<void> &)> Callback)
    {
        FScopeLock Lock(&CompletionMutex);

        if (IsComplete())
        {
            TConvaiResult<void> CurrentResult = GetResult();
            AsyncTask(ENamedThreads::GameThread, [Callback = MoveTemp(Callback), CurrentResult]()
                      { Callback(CurrentResult); });
            return FDelegateHandle();
        }

        return OnCompleteDelegate.AddLambda(MoveTemp(Callback));
    }

    TConvaiResult<void> FAsyncOperation<void>::GetResult() const
    {
        const double StartTime = FPlatformTime::Seconds();
        const double TimeoutSeconds = 60.0;

        while (!IsComplete())
        {
            if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
            {
                UE_LOG(LogConvaiEditor, Error, TEXT("AsyncOperation: GetResult() operation timed out after %.1f seconds"), TimeoutSeconds);
                return TConvaiResult<void>::Failure(TEXT("Operation timed out"));
            }
            FPlatformProcess::Sleep(0.01f);
        }

        FScopeLock Lock(&ResultMutex);
        return Result;
    }

    void FAsyncOperation<void>::ExecuteWork()
    {

        TConvaiResult<void> WorkResult;

        try
        {
            if (CancellationToken.IsValid() && CancellationToken->IsCancellationRequested())
            {
                WorkResult = TConvaiResult<void>::Failure(TEXT("Operation cancelled before execution"));
            }
            else
            {
                WorkResult = WorkFunction(CancellationToken, ProgressReporter);
            }
        }
        catch (const std::exception &Ex)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("AsyncOperation: Exception occurred during execution: %hs"), Ex.what());
            WorkResult = TConvaiResult<void>::Failure(FString::Printf(TEXT("Exception: %hs"), Ex.what()));
        }
        catch (...)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("AsyncOperation: Unknown exception occurred during execution"));
            WorkResult = TConvaiResult<void>::Failure(TEXT("Unknown exception"));
        }

        CompleteWith(WorkResult);
    }

    void FAsyncOperation<void>::CompleteWith(const TConvaiResult<void> &InResult)
    {
        {
            FScopeLock Lock(&StateMutex);
            if (State != EAsyncOperationState::Running && State != EAsyncOperationState::NotStarted)
            {
                return;
            }

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

        {
            FScopeLock Lock(&ResultMutex);
            Result = InResult;
        }

        BroadcastCompletion();
    }

    void FAsyncOperation<void>::BroadcastCompletion()
    {
        TConvaiResult<void> FinalResult = GetResult();
        TWeakPtr<FAsyncOperation<void>> WeakOperation = StaticCastSharedRef<FAsyncOperation<void>>(AsShared());

        AsyncTask(ENamedThreads::GameThread, [WeakOperation, FinalResult]()
                  {
			if (TSharedPtr<FAsyncOperation<void>> This = WeakOperation.Pin())
			{
				FScopeLock Lock(&This->CompletionMutex);
				This->OnCompleteDelegate.Broadcast(FinalResult);
			} });
    }

    void FAsyncOperation<void>::SetStateLocked(EAsyncOperationState NewState)
    {
        State = NewState;
    }

} // namespace ConvaiEditor
