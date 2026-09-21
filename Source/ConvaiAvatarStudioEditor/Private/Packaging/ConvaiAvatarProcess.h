// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"

/** A hidden packaging child and its descendants belong to the editor's lifetime. */
class FConvaiAvatarProcess
{
public:
    FConvaiAvatarProcess() = default;
    ~FConvaiAvatarProcess();
    FConvaiAvatarProcess(const FConvaiAvatarProcess&) = delete;
    FConvaiAvatarProcess& operator=(const FConvaiAvatarProcess&) = delete;

    bool Start(const FString& Executable, const FString& Arguments, void* OutputPipe, FString& Error);
    void Terminate();
    FProcHandle& GetHandle() { return Process; }

private:
    FProcHandle Process;
    void* Job = nullptr;
};
