/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * EventAggregator.cpp
 *
 * Implementation of the event aggregator system.
 */

#include "Events/EventAggregator.h"

DEFINE_LOG_CATEGORY(LogConvaiEditorEvents);

namespace ConvaiEditor
{
    FEventSubscription::~FEventSubscription()
    {
        Unsubscribe();
    }

    FEventSubscription::FEventSubscription(FEventSubscription &&Other) noexcept
        : UnsubscribeCallback(MoveTemp(Other.UnsubscribeCallback))
    {
        Other.UnsubscribeCallback.Reset();
    }

    FEventSubscription &FEventSubscription::operator=(FEventSubscription &&Other) noexcept
    {
        if (this != &Other)
        {
            Unsubscribe();
            UnsubscribeCallback = MoveTemp(Other.UnsubscribeCallback);
            Other.UnsubscribeCallback.Reset();
        }
        return *this;
    }

    void FEventSubscription::Unsubscribe()
    {
        if (UnsubscribeCallback.IsSet())
        {
            (*UnsubscribeCallback)();
            UnsubscribeCallback.Reset();
        }
    }

    FEventAggregator &FEventAggregator::Get()
    {
        static FEventAggregator Instance;
        return Instance;
    }

    void FEventAggregator::Initialize(const FEventAggregatorConfig &InConfig)
    {
        FScopeLock Lock(&Mutex);

        // CRITICAL: Prevent double initialization to avoid ticker handle leak
        if (bIsInitialized)
        {
            UE_LOG(LogConvaiEditorEvents, Warning, TEXT("EventAggregator already initialized - skipping duplicate initialization"));
            return;
        }

        Config = InConfig;
        bIsInitialized = true;

        // CRITICAL: Ticker must NOT capture 'this' - may outlive singleton Shutdown()
        CleanupTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([](float DeltaTime)
            {
                FEventAggregator& Aggregator = FEventAggregator::Get();
                if (!Aggregator.IsInitialized())
                {
                    return false;
                }
                
                Aggregator.CleanupInvalidSubscriptions();
                return true;
            }),
            30.0f);
    }

    void FEventAggregator::Shutdown()
    {
        // CRITICAL: Remove ticker FIRST to prevent race condition with bIsInitialized
        if (CleanupTickerHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(CleanupTickerHandle);
            CleanupTickerHandle.Reset();
        }
        
        {
            FScopeLock Lock(&Mutex);
            bIsInitialized = false;
            
            Subscriptions.Empty();
            EventHistory.Empty();
            TotalEventsPublished = 0;
            NextSubscriptionId = 0;
        }
    }

    void FEventAggregator::Unsubscribe(int32 SubscriptionId)
    {
        FScopeLock Lock(&Mutex);

        if (Subscriptions.Remove(SubscriptionId) > 0)
        {
            if (Config.bEnableVerboseLogging)
            {
            }
        }
    }

    void FEventAggregator::AddToHistory(const FString &EventTypeName, const FString &EventName, int32 SubscriberCount)
    {
        FEventHistoryEntry Entry;
        Entry.EventTypeName = EventTypeName;
        Entry.EventName = EventName;
        Entry.Timestamp = FPlatformTime::Seconds();
        Entry.SubscriberCount = SubscriberCount;

        EventHistory.Add(Entry);

        if (EventHistory.Num() > Config.MaxEventHistory)
        {
            EventHistory.RemoveAt(0, EventHistory.Num() - Config.MaxEventHistory);
        }
    }

    void FEventAggregator::CleanupInvalidSubscriptions()
    {
        FScopeLock Lock(&Mutex);

        TArray<int32> ToRemove;

        for (const auto &Pair : Subscriptions)
        {
            if (!Pair.Value.Handler->IsValid())
            {
                ToRemove.Add(Pair.Key);
            }
        }

        for (int32 Id : ToRemove)
        {
            Subscriptions.Remove(Id);
        }

        if (ToRemove.Num() > 0 && Config.bEnableVerboseLogging)
        {
        }
    }

    int32 FEventAggregator::GetTotalSubscriberCount() const
    {
        FScopeLock Lock(&Mutex);

        int32 Count = 0;
        for (const auto &Pair : Subscriptions)
        {
            if (Pair.Value.Handler->IsValid())
            {
                Count++;
            }
        }

        return Count;
    }

    FEventAggregator::FEventAggregatorStats FEventAggregator::GetStats() const
    {
        FScopeLock Lock(&Mutex);

        FEventAggregatorStats Stats;
        Stats.TotalSubscriptions = Subscriptions.Num();
        Stats.TotalEventsPublished = TotalEventsPublished.load();
        Stats.EventHistorySize = EventHistory.Num();

        TSet<FString> UniqueEventTypes;
        for (const auto &Pair : Subscriptions)
        {
            UniqueEventTypes.Add(Pair.Value.EventTypeName);
        }
        Stats.TotalEventTypes = UniqueEventTypes.Num();

        return Stats;
    }

    void FEventAggregator::DumpSubscriptions() const
    {
        FScopeLock Lock(&Mutex);

        TMap<FString, TArray<int32>> SubscriptionsByType;
        for (const auto &Pair : Subscriptions)
        {
            if (Pair.Value.Handler->IsValid())
            {
                SubscriptionsByType.FindOrAdd(Pair.Value.EventTypeName).Add(Pair.Key);
            }
        }

        for (const auto &TypePair : SubscriptionsByType)
        {
            for (int32 Id : TypePair.Value)
            {
            }
        }
    }

} // namespace ConvaiEditor
