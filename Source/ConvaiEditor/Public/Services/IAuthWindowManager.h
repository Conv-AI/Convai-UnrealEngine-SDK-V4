/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IAuthWindowManager.h
 *
 * Interface for authentication window management.
 */

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "ConvaiEditor.h"

/**
 * Authentication flow states.
 */
enum class EAuthFlowState : uint8
{
    Welcome,
    Authenticating,
    Success,
    Error
};

/**
 * Manages authentication window lifecycle and state transitions.
 */
class CONVAIEDITOR_API IAuthWindowManager : public IConvaiService
{
public:
    virtual ~IAuthWindowManager() = default;

    virtual void StartAuthFlow() = 0;
    virtual void OnAuthSuccess() = 0;
    virtual void OnAuthCancelled() = 0;
    virtual void OnAuthError(const FString &Error) = 0;

    virtual bool IsAuthWindowOpen() const = 0;
    virtual bool IsWelcomeWindowOpen() const = 0;
    virtual EAuthFlowState GetAuthState() const = 0;

    virtual void CloseAuthWindow() = 0;
    virtual void OpenWelcomeWindow() = 0;
    virtual void CloseWelcomeWindow() = 0;

    DECLARE_MULTICAST_DELEGATE(FOnAuthFlowStarted);
    virtual FOnAuthFlowStarted &OnAuthFlowStarted() = 0;

    DECLARE_MULTICAST_DELEGATE(FOnAuthFlowCompleted);
    virtual FOnAuthFlowCompleted &OnAuthFlowCompleted() = 0;

    DECLARE_MULTICAST_DELEGATE(FOnWelcomeWindowRequested);
    virtual FOnWelcomeWindowRequested &OnWelcomeWindowRequested() = 0;

    static FName StaticType() { return TEXT("IAuthWindowManager"); }
};