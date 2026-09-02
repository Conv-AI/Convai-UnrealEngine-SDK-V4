// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

CONVAITESTS_API DECLARE_LOG_CATEGORY_EXTERN(LogConvaiTests, Log, All);

namespace ConvaiTestExit
{
    // The exit code the finished run wants the process to return, and the last
    // point in shutdown a module can still deliver it. Split across the module
    // and the runner because neither half works alone -- see F21 and the
    // comment on Arm().
    CONVAITESTS_API void Arm(int32 Code);
    CONVAITESTS_API void DeliverAtShutdown();
}

class FConvaiTestsModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
