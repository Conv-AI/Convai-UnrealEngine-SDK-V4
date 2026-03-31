/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IWelcomeWindowManager.h
 *
 * Interface for welcome window management.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Delegates/Delegate.h"

/**
 * Manages welcome window lifecycle and styling.
 */
class CONVAIEDITOR_API IWelcomeWindowManager : public IConvaiService
{
public:
    virtual ~IWelcomeWindowManager() = default;

    virtual void ShowWelcomeWindow() = 0;
    virtual void CloseWelcomeWindow() = 0;
    virtual bool IsWelcomeWindowOpen() const = 0;
    virtual void BringWelcomeWindowToFront() = 0;
    virtual FVector2D GetWelcomeWindowSize() const = 0;
    virtual FVector2D GetWelcomeWindowMinSize() const = 0;
    virtual void SetWelcomeWindowTitle(const FString &Title) = 0;

    DECLARE_MULTICAST_DELEGATE(FOnWelcomeWindowOpened);
    virtual FOnWelcomeWindowOpened &OnWelcomeWindowOpened() = 0;

    DECLARE_MULTICAST_DELEGATE(FOnWelcomeWindowClosed);
    virtual FOnWelcomeWindowClosed &OnWelcomeWindowClosed() = 0;

    static FName StaticType() { return TEXT("IWelcomeWindowManager"); }
};