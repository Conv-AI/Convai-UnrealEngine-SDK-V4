/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * BindingManager.cpp
 *
 * Implementation of the binding manager for MVVM pattern.
 */

#include "MVVM/BindingManager.h"
#include "ConvaiEditor.h"

namespace ConvaiEditor
{
    FBindingManager::FBindingManager()
        : bIsEnabled(true), PollInterval(0.1f), TimeSinceLastPoll(0.0f)
    {
    }

    FBindingManager &FBindingManager::Get()
    {
        static FBindingManager Instance;
        return Instance;
    }

    void FBindingManager::Initialize()
    {
        TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateRaw(this, &FBindingManager::Tick),
            0.0f);
    }

    void FBindingManager::Shutdown()
    {
        if (TickerHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
            TickerHandle.Reset();
        }

        {
            FScopeLock Lock(&Mutex);
            Bindings.Empty();
        }
    }

    void FBindingManager::RegisterBinding(void *PropertyPtr, TFunction<void()> PollCallback)
    {
        if (!PropertyPtr || !PollCallback)
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("BindingManager: Cannot register binding with null pointer or callback"));
            return;
        }

        FScopeLock Lock(&Mutex);

        for (const FBindingInfo &Info : Bindings)
        {
            if (Info.PropertyPtr == PropertyPtr)
            {
                return;
            }
        }

        FBindingInfo Info;
        Info.PropertyPtr = PropertyPtr;
        Info.PollCallback = PollCallback;
        Bindings.Add(Info);
    }

    void FBindingManager::UnregisterBinding(void *PropertyPtr)
    {
        if (!PropertyPtr)
        {
            return;
        }

        FScopeLock Lock(&Mutex);

        int32 RemovedCount = Bindings.RemoveAll([PropertyPtr](const FBindingInfo &Info)
                                                { return Info.PropertyPtr == PropertyPtr; });

        if (RemovedCount > 0)
        {
        }
    }

    void FBindingManager::PollBindings()
    {
        if (!bIsEnabled)
        {
            return;
        }

        FScopeLock Lock(&Mutex);

        for (const FBindingInfo &Info : Bindings)
        {
            if (Info.PollCallback)
            {
                Info.PollCallback();
            }
        }
    }

    void FBindingManager::SetEnabled(bool bEnabled)
    {
        bIsEnabled = bEnabled;
    }

    void FBindingManager::SetPollInterval(float IntervalSeconds)
    {
        if (IntervalSeconds <= 0.0f)
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("BindingManager: Invalid poll interval %.3fs - must be positive"), IntervalSeconds);
            return;
        }

        PollInterval = IntervalSeconds;
    }

    int32 FBindingManager::GetBindingCount() const
    {
        FScopeLock Lock(&Mutex);
        return Bindings.Num();
    }

    bool FBindingManager::Tick(float DeltaTime)
    {
        if (!bIsEnabled)
        {
            return true;
        }

        TimeSinceLastPoll += DeltaTime;

        if (TimeSinceLastPoll >= PollInterval)
        {
            PollBindings();
            TimeSinceLastPoll = 0.0f;
        }

        return true;
    }

} // namespace ConvaiEditor
