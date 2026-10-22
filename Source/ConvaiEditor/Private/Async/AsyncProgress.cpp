/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AsyncProgress.cpp
 *
 * Implementation of async progress tracking.
 */

#include "Async/AsyncProgress.h"
#include "ConvaiEditor.h"
#include "Async/Async.h"

namespace ConvaiEditor
{
    FAsyncProgress::FAsyncProgress()
    {
    }

    FAsyncProgress::~FAsyncProgress()
    {
        FScopeLock Lock(&DelegateMutex);
        OnProgressChangedDelegate.Clear();
    }

    void FAsyncProgress::ReportProgress(float Progress, const FString &Message)
    {
        Progress = FMath::Clamp(Progress, 0.0f, 1.0f);

        UpdateProgress([Progress, &Message](FAsyncProgressData &Data)
                       {
			Data.Progress = Progress;
			Data.Message = Message;
			Data.Timestamp = FDateTime::UtcNow(); });
    }

    void FAsyncProgress::ReportStage(const FString &Stage)
    {
        UpdateProgress([&Stage](FAsyncProgressData &Data)
                       {
			Data.Stage = Stage;
			Data.Timestamp = FDateTime::UtcNow(); });
    }

    void FAsyncProgress::ReportTransferProgress(int64 BytesTransferred, int64 TotalBytes)
    {
        UpdateProgress([BytesTransferred, TotalBytes](FAsyncProgressData &Data)
                       {
			Data.BytesTransferred = BytesTransferred;
			Data.TotalBytes = TotalBytes;
			
			if (TotalBytes > 0)
			{
				Data.Progress = static_cast<float>(BytesTransferred) / static_cast<float>(TotalBytes);
				Data.Progress = FMath::Clamp(Data.Progress, 0.0f, 1.0f);
			}
			
			Data.Timestamp = FDateTime::UtcNow(); });
    }

    FAsyncProgressData FAsyncProgress::GetCurrentProgress() const
    {
        FScopeLock Lock(&ProgressMutex);
        return CurrentProgress;
    }

    void FAsyncProgress::Reset()
    {
        UpdateProgress([](FAsyncProgressData &Data)
                       {
			Data.Progress = 0.0f;
			Data.Message.Empty();
			Data.Stage.Empty();
			Data.BytesTransferred = 0;
			Data.TotalBytes = 0;
			Data.Timestamp = FDateTime::UtcNow(); });
    }

    void FAsyncProgress::UpdateProgress(TFunction<void(FAsyncProgressData &)> Updater)
    {
        if (!Updater)
        {
            return;
        }

        FAsyncProgressData UpdatedProgress;
        {
            FScopeLock Lock(&ProgressMutex);
            Updater(CurrentProgress);
            UpdatedProgress = CurrentProgress;
        }

        TWeakPtr<FAsyncProgress> WeakProgress = AsShared();
        AsyncTask(ENamedThreads::GameThread, [WeakProgress, UpdatedProgress]()
                  {
			if (TSharedPtr<FAsyncProgress> This = WeakProgress.Pin())
			{
				FScopeLock DelegateLock(&This->DelegateMutex);
				This->OnProgressChangedDelegate.Broadcast(UpdatedProgress);
			} });
    }

} // namespace ConvaiEditor
