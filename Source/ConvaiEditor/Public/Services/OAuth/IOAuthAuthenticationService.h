/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IOAuthAuthenticationService.h
 *
 * Interface for OAuth authentication flow orchestration.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Templates/SharedPointer.h"

/**
 * Orchestrates OAuth authentication flow.
 */
class CONVAIEDITOR_API IOAuthAuthenticationService : public IConvaiService
{
public:
    virtual ~IOAuthAuthenticationService() override = default;

    virtual void StartLogin() = 0;
    virtual void CancelLogin() = 0;
    virtual bool IsAuthenticated() const = 0;

    DECLARE_MULTICAST_DELEGATE(FOnAuthSuccess);
    virtual FOnAuthSuccess &OnAuthSuccess() = 0;

    DECLARE_MULTICAST_DELEGATE_OneParam(FOnAuthFailure, const FString & /*Error*/);
    virtual FOnAuthFailure &OnAuthFailure() = 0;

    DECLARE_MULTICAST_DELEGATE(FOnAuthWindowClosed);
    virtual FOnAuthWindowClosed &OnAuthWindowClosed() = 0;

    virtual bool IsAuthInProgress() const = 0;
    virtual void SetOnWindowClosedCallback(FSimpleDelegate Callback) = 0;

    static FName StaticType() { return TEXT("IOAuthAuthenticationService"); }
};