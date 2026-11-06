// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include <atomic>

/** How FConvaiAvatarProcess::RunToExit ended. */
enum class EConvaiAvatarProcessEnd : uint8
{
    /** The child exited by itself; the exit code is set. */
    Exited,
    /** The cancel flag was set while it ran, and its job was terminated. */
    Cancelled,
    /** Its deadline passed, and its job was terminated. */
    TimedOut,
    /** Nothing ran. The error says why; it is empty when the output pipe could not be created. */
    NotStarted,
};

/** A hidden packaging child and its descendants belong to the editor's lifetime. */
class FConvaiAvatarProcess
{
public:
    using FOutput = TFunction<void(const TArray<uint8>&)>;

    FConvaiAvatarProcess() = default;
    ~FConvaiAvatarProcess();
    FConvaiAvatarProcess(const FConvaiAvatarProcess&) = delete;
    FConvaiAvatarProcess& operator=(const FConvaiAvatarProcess&) = delete;

    bool Start(const FString& Executable, const FString& Arguments, void* OutputPipe, FString& Error);
    void Terminate();
    FProcHandle& GetHandle() { return Process; }

    /** Starts Executable in its own job with stdout and stderr on one pipe and blocks until it exits, Cancelled is set or
     *  TimeoutSeconds pass (zero or less waits without a deadline); the last two terminate the whole job. Output receives
     *  each read's raw bytes, and Poll runs about every 100 ms; leave either unbound to skip it. Call from a worker. */
    static EConvaiAvatarProcessEnd RunToExit(const FString& Executable, const FString& Arguments, const std::atomic<bool>& Cancelled,
        double TimeoutSeconds, int32& OutExitCode, FString& OutError, const FOutput& Output = {}, const TFunction<void()>& Poll = {});

#if WITH_DEV_AUTOMATION_TESTS
    /** Test seam: while bound, RunToExit hands it each launch instead of starting it, and returns NotStarted. Bind and
     *  clear it on the game thread while no helper runs. */
    static TFunction<void(const FString& Executable, const FString& Arguments, double TimeoutSeconds)> LaunchRecorder;
#endif

private:
    FProcHandle Process;
    void* Job = nullptr;
};
