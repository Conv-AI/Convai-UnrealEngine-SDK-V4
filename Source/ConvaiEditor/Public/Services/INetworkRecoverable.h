/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * INetworkRecoverable.h
 *
 * Interface for network recovery notification.
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Interface for services that can recover from network loss.
 */
class CONVAIEDITOR_API INetworkRecoverable
{
public:
    virtual ~INetworkRecoverable() = default;

    /** Called when network connectivity is restored */
    virtual void OnNetworkRestored() = 0;
};
