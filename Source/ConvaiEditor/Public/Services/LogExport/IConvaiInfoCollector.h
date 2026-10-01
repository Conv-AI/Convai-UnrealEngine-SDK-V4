/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IConvaiInfoCollector.h
 *
 * Interface for information collectors.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Interface for collecting information for log export.
 */
class IConvaiInfoCollector
{
public:
    virtual ~IConvaiInfoCollector() = default;

    virtual TSharedPtr<FJsonObject> CollectInfo() const = 0;
    virtual FString GetCollectorName() const = 0;
    virtual bool IsAvailable() const { return true; }
};
