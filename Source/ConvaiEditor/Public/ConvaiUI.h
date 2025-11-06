/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiUI.h
 *
 * Alternative header for ConvaiEditor plugin.
 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "HAL/CriticalSection.h"

/** Log category for Convai Editor messages */
DECLARE_LOG_CATEGORY_EXTERN(LogConvaiEditor, Log, All);

class IConvaiService;

/**
 * Base interface for all services in the ConvaiEditor system.
 */
class CONVAIEDITOR_API IConvaiService
{
public:
    virtual ~IConvaiService() = default;

    /** Called after service construction for initialization */
    virtual void Startup() {}

    /** Called before service destruction for cleanup */
    virtual void Shutdown() {}

    /** Returns the service type name for registration and lookup */
    static FName StaticType() { return TEXT("IConvaiService"); }
};

class CONVAIEDITOR_API FConvaiServiceRegistry;

/**
 * Main module for the ConvaiEditor plugin.
 */
class FConvaiEditorModule : public IModuleInterface
{
public:
    /** Initializes the module and registers services */
    virtual void StartupModule() override;

    /** Shuts down the module and cleans up resources */
    virtual void ShutdownModule() override;

    /** Opens the main Convai Editor window */
    void OpenConvaiWindow();

private:
    /** Shows the welcome window if needed for first-time setup */
    void ShowWelcomeWindowIfNeeded();
};
