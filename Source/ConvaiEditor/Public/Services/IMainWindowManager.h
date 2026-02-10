/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IMainWindowManager.h
 *
 * Interface for main window lifecycle management.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Widgets/SWindow.h"
#include "Delegates/Delegate.h"

// Forward declarations
class SConvaiShell;

DECLARE_MULTICAST_DELEGATE(FOnMainWindowOpened);
DECLARE_MULTICAST_DELEGATE(FOnMainWindowClosed);

/** Manages main window lifecycle and display. */
class CONVAIEDITOR_API IMainWindowManager : public IConvaiService
{
public:
    virtual void OpenMainWindow(bool bShouldBeTopmost = false) = 0;
    virtual void CloseMainWindow() = 0;
    virtual bool IsMainWindowOpen() const = 0;
    virtual void BringMainWindowToFront() = 0;
    virtual TWeakPtr<SConvaiShell> GetMainWindow() const = 0;
    virtual FVector2D GetWindowSize() const = 0;
    virtual FVector2D GetMinWindowSize() const = 0;
    virtual void SetMainWindowTitle(const FString &Title) = 0;
    virtual void DisableMainWindowTopmost() = 0;
    virtual FOnMainWindowOpened &OnMainWindowOpened() = 0;
    virtual FOnMainWindowClosed &OnMainWindowClosed() = 0;

    static FName StaticType() { return TEXT("IMainWindowManager"); }
};
