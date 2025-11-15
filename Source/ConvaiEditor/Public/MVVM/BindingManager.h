/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * BindingManager.h
 *
 * Manages two-way bindings and polling for attribute changes.
 */

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Containers/Ticker.h"

namespace ConvaiEditor
{
    // Forward declaration
    template <typename T>
    class TBindableProperty;

    /** Manages two-way bindings and polling for attribute changes. */
    class CONVAIEDITOR_API FBindingManager
    {
    public:
        /** Get singleton instance */
        static FBindingManager &Get();

        /** Initialize the binding manager */
        void Initialize();

        /** Shutdown the binding manager */
        void Shutdown();

        /** Register a binding for automatic polling */
        void RegisterBinding(void *PropertyPtr, TFunction<void()> PollCallback);

        /** Unregister a binding */
        void UnregisterBinding(void *PropertyPtr);

        /** Poll all bindings */
        void PollBindings();

        /** Enable/disable binding polling */
        void SetEnabled(bool bEnabled);

        /** Check if binding polling is enabled */
        bool IsEnabled() const { return bIsEnabled; }

        /** Set the poll interval */
        void SetPollInterval(float IntervalSeconds);

        /** Get the current poll interval */
        float GetPollInterval() const { return PollInterval; }

        /** Get the number of registered bindings */
        int32 GetBindingCount() const;

    private:
        FBindingManager();

        /** Ticker callback for polling */
        bool Tick(float DeltaTime);

        /** Binding information */
        struct FBindingInfo
        {
            void *PropertyPtr = nullptr;
            TFunction<void()> PollCallback;
        };

        /** List of registered bindings */
        TArray<FBindingInfo> Bindings;

        /** Ticker handle */
        FTSTicker::FDelegateHandle TickerHandle;

        /** Thread safety */
        mutable FCriticalSection Mutex;

        /** Enable/disable flag */
        bool bIsEnabled;

        /** Poll interval in seconds */
        float PollInterval;

        /** Time since last poll */
        float TimeSinceLastPoll;
    };

} // namespace ConvaiEditor
