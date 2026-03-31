/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IEvent.h
 *
 * Base interface for all events in the Event Aggregator system.
 */

#pragma once

#include "CoreMinimal.h"

namespace ConvaiEditor
{
    /** Base interface for all events in the Event Aggregator system. */
    struct IEvent
    {
        virtual ~IEvent() = default;

        double GetTimestamp() const { return Timestamp; }
        virtual FString GetEventName() const { return TEXT("IEvent"); }

    protected:
        IEvent()
            : Timestamp(FPlatformTime::Seconds())
        {
        }

    private:
        double Timestamp;
    };

} // namespace ConvaiEditor
