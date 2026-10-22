// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace ConvaiAvatarFileTransferPrivate
{
extern const TCHAR* WorkerLongestRuntimeFile;
bool ValidateWorkerPathBudget(const FString& Directory, FString& Error);
/** Call under the uploader lease, which every transfer and packaging run holds, so no job in Jobs is in flight and a
 * crashed holder's handles closed with it. Deletes each Jobs/<32 hex digits> folder; returns what stayed. */
TArray<FString> SweepJobDirectories(const FString& Jobs);
bool SafePath(const FString& Path);
/** Reads a transfer worker's result: Result is the parsed result file, null when it could not be read; ChildError is what
 * running the child reported, empty when it ran and exited 0. Returns empty only for a verified success: success true from
 * a child that ran, bytes from 1 to the cap and equal to LocalSize, an md5 of 32 hex digits and the v3 transport. A success
 * claim that fails any check gets the verification error; a missing result gets ChildError, else a fixed message; any other
 * result gets the worker's own error text, else ChildError, else a fixed message. */
FString InterpretTransferResult(const TSharedPtr<FJsonObject>& Result, const FString& ChildError, int64 LocalSize);

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
