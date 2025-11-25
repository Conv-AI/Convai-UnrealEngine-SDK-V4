/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IConvaiAccountService.h
 *
 * Interface for Convai account usage management.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Models/ConvaiAccountUsage.h"

/**
 * Delegate for account usage retrieval.
 */
DECLARE_DELEGATE_TwoParams(FOnAccountUsageReceived, const FConvaiAccountUsage & /*Usage*/, const FString & /*Error*/);

/**
 * Interface for fetching account usage information.
 */
class CONVAIEDITOR_API IConvaiAccountService : public IConvaiService
{
public:
    virtual ~IConvaiAccountService() = default;

    /** Fetches account usage data from Convai API */
    virtual void GetAccountUsage(const FString &ApiKey, FOnAccountUsageReceived Callback) = 0;

    static FName StaticType() { return TEXT("IConvaiAccountService"); }
};
