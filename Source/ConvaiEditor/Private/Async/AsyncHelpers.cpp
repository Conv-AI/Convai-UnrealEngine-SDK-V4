/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AsyncHelpers.cpp
 *
 * Implementation of async helper utilities.
 */

#include "Async/AsyncHelpers.h"
#include "ConvaiEditor.h"

namespace ConvaiEditor
{
    namespace AsyncHelpers
    {
        TSharedPtr<FAsyncOperation<void>> Delay(
            float DelaySeconds,
            TSharedPtr<FCancellationToken> CancellationToken)
        {
            return MakeShared<FAsyncOperation<void>>(
                [DelaySeconds](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<void>
                {
                    Progress->ReportStage(TEXT("Delay"));

                    const double StartTime = FPlatformTime::Seconds();
                    const double EndTime = StartTime + DelaySeconds;

                    while (FPlatformTime::Seconds() < EndTime)
                    {
                        if (Token.IsValid() && Token->IsCancellationRequested())
                        {
                            return TConvaiResult<void>::Failure(TEXT("Delay cancelled"));
                        }

                        const double ElapsedTime = FPlatformTime::Seconds() - StartTime;
                        Progress->ReportProgress(static_cast<float>(ElapsedTime / DelaySeconds), FString::Printf(TEXT("%.1fs / %.1fs"), ElapsedTime, DelaySeconds));

                        FPlatformProcess::Sleep(0.01f); // 10ms poll interval
                    }

                    Progress->ReportProgress(1.0f, TEXT("Delay completed"));
                    return TConvaiResult<void>::Success();
                },
                CancellationToken);
        }

    } // namespace AsyncHelpers

} // namespace ConvaiEditor
