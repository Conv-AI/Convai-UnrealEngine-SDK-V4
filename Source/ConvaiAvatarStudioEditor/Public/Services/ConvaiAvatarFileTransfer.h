// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Services/ConvaiAvatarTransferProgress.h"
#include <atomic>

/** File bodies use the resolved HTTP Editor module in the shared uploader; metadata stays in the host. */
class CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarFileTransfer : public TSharedFromThis<FConvaiAvatarFileTransfer, ESPMode::ThreadSafe>
{
public:
    using FCompletion = TFunction<void(FString)>;
    using FProgress = TFunction<void(float)>;
    using FProgressDetails = TFunction<void(const FConvaiAvatarTransferProgress&)>;

    ~FConvaiAvatarFileTransfer();
    /** An upload operation reuses its captured dependency resolution; an empty value resolves before a download. */
    void Start(bool bUpload, FString Url, FString FilePath, FCompletion Completion, FProgress Progress = {}, FString ResolutionFile = {}, FProgressDetails ProgressDetails = {});
    void Cancel();
    void Shutdown();
    bool IsBusy() const { return bBusy.load(); }

private:
    std::atomic<bool> bBusy{false};
    std::atomic<bool> bCancelled{false};
    std::atomic<bool> bShuttingDown{false};
    std::atomic<uint64> Epoch{0};
    TFuture<void> Task;
};
