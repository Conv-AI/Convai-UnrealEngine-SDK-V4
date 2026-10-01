/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiUserInfo.h
 *
 * Model for user information received from OAuth.
 */

#pragma once

#include "CoreMinimal.h"

/**
 * User information received from OAuth authentication.
 */
struct CONVAIEDITOR_API FConvaiUserInfo
{
    FString Username;
    FString Email;

    FConvaiUserInfo()
    {
    }

    /** Creates user info from JSON string */
    static bool FromJson(const FString &JsonString, FConvaiUserInfo &OutUserInfo);

    /** Checks if user info is valid */
    bool IsValid() const
    {
        return !Username.IsEmpty() && !Email.IsEmpty();
    }
};
