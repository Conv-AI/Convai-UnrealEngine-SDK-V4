// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace ConvaiAvatarFileTransferPrivate
{
extern const TCHAR* WorkerLongestRuntimeFile;
bool ValidateWorkerPathBudget(const FString& Directory, FString& Error);
bool SafePath(const FString& Path);

/** Owns a protected, delete-on-close request for one worker process. */
class FPrivateRequest
{
public:
    FPrivateRequest();
    ~FPrivateRequest();
    bool CreateDirectory(const FString& Directory, FString& Error);
    bool Write(const FString& Path, const FString& Text, FString& Error);
private:
    void* Descriptor = nullptr;
    void* File;
};
}
