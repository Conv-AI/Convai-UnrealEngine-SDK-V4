// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiTests.h"

#include "HAL/PlatformMisc.h"

DEFINE_LOG_CATEGORY(LogConvaiTests);

namespace ConvaiTestExit
{
    namespace
    {
        int32 GCode = 0;
    }

    // F21: RequestExitWithStatus(Force=false) cannot deliver a code on Windows.
    // It ends at PostQuitMessage(ReturnCode), UE's pump dispatches WM_QUIT
    // without reading wParam, and GuardedMain returns EngineInit()'s ErrorLevel
    // -- 0 for any launch that started. There is no GExitCode in UE 5.8. Only
    // the Force path carries a code, because it is TerminateProcess.
    //
    // Forcing from Complete() would exit before the teardown that F25 is
    // measured in, so the code is armed there and delivered here instead, from
    // ShutdownModule: the last point a module still runs, after the world, the
    // rendering thread and the shader library are gone, and before AppExit tears
    // GLog down. A launch that hangs in teardown never reaches this and is still
    // killed and counted by run.py, which is what keeps F25 visible.
    //
    // atexit does not work here: UnloadModulesAtShutdown does not free the
    // module DLLs -- the OS does, at process exit -- and the CRT skips a DLL's
    // atexit table when the process is terminating.
    void Arm(int32 Code)
    {
        GCode = Code;
    }

    void DeliverAtShutdown()
    {
        // Zero needs no help: a clean exit already returns 0, and leaving that
        // path untouched keeps a green launch shutting down exactly as before.
        if (GCode != 0)
        {
            FPlatformMisc::RequestExitWithStatus(/*Force=*/true, static_cast<uint8>(GCode));
        }
    }
}

void FConvaiTestsModule::StartupModule()
{
}

void FConvaiTestsModule::ShutdownModule()
{
    ConvaiTestExit::DeliverAtShutdown();
}

IMPLEMENT_MODULE(FConvaiTestsModule, ConvaiTests)
