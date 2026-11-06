/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AsyncProgress.h
 *
 * Thread-safe progress tracking for async operations.
 */

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

namespace ConvaiEditor
{
	/**
	 * Snapshot of async operation progress at a point in time.
	 */
	struct CONVAIEDITOR_API FAsyncProgressData
	{
		/** Current progress percentage (0.0 to 1.0) */
		float Progress = 0.0f;

		/** Human-readable progress message */
		FString Message;

		/** Current stage/phase name (e.g., "Downloading", "Processing", "Uploading") */
		FString Stage;

		/** Optional: Bytes transferred (for network operations) */
		int64 BytesTransferred = 0;

		/** Optional: Total bytes (for network operations) */
		int64 TotalBytes = 0;

		/** Timestamp when this progress was reported */
		FDateTime Timestamp;

		FAsyncProgressData()
			: Timestamp(FDateTime::UtcNow())
		{
		}

		/** Returns a percentage string (e.g., "75%") */
		FString GetProgressPercentageString() const
		{
			return FString::Printf(TEXT("%.0f%%"), Progress * 100.0f);
		}

		/** Returns a human-readable transfer size string (e.g., "1.5 MB / 10 MB") */
		FString GetTransferSizeString() const
		{
			if (TotalBytes > 0)
			{
				auto FormatBytes = [](int64 Bytes) -> FString
				{
					if (Bytes < 1024)
						return FString::Printf(TEXT("%lld B"), Bytes);
					if (Bytes < 1024 * 1024)
						return FString::Printf(TEXT("%.1f KB"), Bytes / 1024.0);
					if (Bytes < 1024 * 1024 * 1024)
						return FString::Printf(TEXT("%.1f MB"), Bytes / (1024.0 * 1024.0));
					return FString::Printf(TEXT("%.1f GB"), Bytes / (1024.0 * 1024.0 * 1024.0));
				};

				return FString::Printf(TEXT("%s / %s"), *FormatBytes(BytesTransferred), *FormatBytes(TotalBytes));
			}
			return FString();
		}
	};

	/**
	 * Interface for reporting progress of async operations.
	 */
	class CONVAIEDITOR_API IAsyncProgressReporter
	{
	public:
		virtual ~IAsyncProgressReporter() = default;

		/**
		 * Reports progress with a percentage and message.
		 *
		 * @param Progress Value between 0.0 and 1.0
		 * @param Message Human-readable progress message
		 */
		virtual void ReportProgress(float Progress, const FString &Message) = 0;

		/**
		 * Reports the current operation stage.
		 *
		 * @param Stage Stage name
		 */
		virtual void ReportStage(const FString &Stage) = 0;

		/**
		 * Reports transfer progress for network operations.
		 *
		 * @param BytesTransferred Number of bytes transferred so far
		 * @param TotalBytes Total number of bytes to transfer
		 */
		virtual void ReportTransferProgress(int64 BytesTransferred, int64 TotalBytes) = 0;
	};

	/**
	 * Thread-safe implementation of async progress tracking.
	 */
	class CONVAIEDITOR_API FAsyncProgress : public IAsyncProgressReporter, public TSharedFromThis<FAsyncProgress>
	{
	public:
		/** Delegate type for progress change notifications */
		DECLARE_MULTICAST_DELEGATE_OneParam(FOnProgressChanged, const FAsyncProgressData & /* ProgressData */);

		FAsyncProgress();
		virtual ~FAsyncProgress();

		//~ Begin IAsyncProgressReporter Interface
		virtual void ReportProgress(float Progress, const FString &Message) override;
		virtual void ReportStage(const FString &Stage) override;
		virtual void ReportTransferProgress(int64 BytesTransferred, int64 TotalBytes) override;
		//~ End IAsyncProgressReporter Interface

		/**
		 * Gets the current progress data snapshot.
		 *
		 * @return Current progress state
		 */
		FAsyncProgressData GetCurrentProgress() const;

		/**
		 * Resets progress to initial state.
		 */
		void Reset();

		/** Returns the delegate for progress change notifications */
		FOnProgressChanged &OnProgressChanged() { return OnProgressChangedDelegate; }

	private:
		/** Updates the internal progress state and broadcasts changes */
		void UpdateProgress(TFunction<void(FAsyncProgressData &)> Updater);

		/** Current progress state */
		FAsyncProgressData CurrentProgress;

		/** Protects CurrentProgress */
		mutable FCriticalSection ProgressMutex;

		/** Delegate for progress notifications */
		FOnProgressChanged OnProgressChangedDelegate;

		/** Protects delegate operations */
		mutable FCriticalSection DelegateMutex;
	};

	/**
	 * No-op progress reporter for operations that don't need progress tracking.
	 */
	class CONVAIEDITOR_API FNullProgressReporter : public IAsyncProgressReporter
	{
	public:
		virtual void ReportProgress(float Progress, const FString &Message) override {}
		virtual void ReportStage(const FString &Stage) override {}
		virtual void ReportTransferProgress(int64 BytesTransferred, int64 TotalBytes) override {}

		/** Singleton instance */
		static TSharedPtr<FNullProgressReporter> Get()
		{
			static TSharedPtr<FNullProgressReporter> Instance = MakeShared<FNullProgressReporter>();
			return Instance;
		}
	};

} // namespace ConvaiEditor
