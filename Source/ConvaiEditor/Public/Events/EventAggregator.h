/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * EventAggregator.h
 *
 * Centralized event aggregator for type-safe publish/subscribe pattern.
 */

#pragma once

#include "CoreMinimal.h"
#include "Events/IEvent.h"
#include "HAL/CriticalSection.h"
#include "Templates/SharedPointer.h"
#include "Templates/Function.h"
#include "Templates/UnrealTemplate.h"
#include "Async/TaskGraphInterfaces.h"

DECLARE_LOG_CATEGORY_EXTERN(LogConvaiEditorEvents, Log, All);

// Helper for UE 5.0 compatibility - StaticCastWeakPtr was added in 5.1
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 1
namespace ConvaiEditorInternal
{
    template <typename DestType, typename SourceType>
    inline TWeakPtr<DestType> StaticCastWeakPtrCompat(const TWeakPtr<SourceType> &InWeakPtr)
    {
        if (auto Pinned = InWeakPtr.Pin())
        {
            return StaticCastSharedPtr<DestType>(Pinned);
        }
        return nullptr;
    }
}
#define StaticCastWeakPtr ConvaiEditorInternal::StaticCastWeakPtrCompat
#endif

namespace ConvaiEditor
{
    /** Subscription token that automatically unsubscribes when destroyed. */
    class FEventSubscription
    {
    public:
        FEventSubscription() = default;
        ~FEventSubscription();

        FEventSubscription(FEventSubscription &&Other) noexcept;
        FEventSubscription &operator=(FEventSubscription &&Other) noexcept;

        FEventSubscription(const FEventSubscription &) = delete;
        FEventSubscription &operator=(const FEventSubscription &) = delete;

        bool IsValid() const { return UnsubscribeCallback.IsSet(); }
        void Unsubscribe();

    private:
        friend class FEventAggregator;

        TOptional<TFunction<void()>> UnsubscribeCallback;
    };

    /** Configuration for Event Aggregator */
    struct FEventAggregatorConfig
    {
        bool bEnableEventHistory = false;
        int32 MaxEventHistory = 100;
        bool bEnableVerboseLogging = false;
        FString LogCategory = TEXT("LogConvaiEditor");
    };

    /** Centralized event aggregator for type-safe publish/subscribe pattern. */
    class CONVAIEDITOR_API FEventAggregator
    {
    public:
        static FEventAggregator &Get();
        void Initialize(const FEventAggregatorConfig &InConfig);
        void Shutdown();

        template <typename TEvent>
        FEventSubscription Subscribe(TFunction<void(const TEvent &)> Handler);

        template <typename TEvent, typename TObject>
        FEventSubscription Subscribe(TWeakPtr<TObject> WeakRef, TFunction<void(const TEvent &)> Handler);

        template <typename TEvent, typename TObject>
        FEventSubscription Subscribe(TObject *Object, void (TObject::*Handler)(const TEvent &));

        template <typename TEvent>
        void Publish(const TEvent &Event);

        template <typename TEvent>
        void PublishGameThread(const TEvent &Event);

        template <typename TEvent>
        int32 GetSubscriberCount() const;

        int32 GetTotalSubscriberCount() const;

        struct FEventAggregatorStats
        {
            int32 TotalSubscriptions = 0;
            int32 TotalEventTypes = 0;
            int32 TotalEventsPublished = 0;
            int32 EventHistorySize = 0;
        };
        FEventAggregatorStats GetStats() const;

        void DumpSubscriptions() const;

    private:
        FEventAggregator() = default;
        ~FEventAggregator() = default;

        FEventAggregator(const FEventAggregator &) = delete;
        FEventAggregator &operator=(const FEventAggregator &) = delete;

        struct IEventHandler
        {
            virtual ~IEventHandler() = default;
            virtual bool IsValid() const = 0;
            virtual void Invoke(const IEvent &Event) = 0;
        };

        template <typename TEvent>
        struct TEventHandlerWeak : IEventHandler
        {
            TWeakPtr<void> WeakRef;
            TFunction<void(const TEvent &)> Handler;

            TEventHandlerWeak(TWeakPtr<void> InWeakRef, TFunction<void(const TEvent &)> InHandler)
                : WeakRef(InWeakRef), Handler(MoveTemp(InHandler))
            {
            }

            virtual bool IsValid() const override
            {
                return WeakRef.IsValid();
            }

            virtual void Invoke(const IEvent &Event) override
            {
                if (WeakRef.IsValid())
                {
                    Handler(static_cast<const TEvent &>(Event));
                }
            }
        };

        template <typename TEvent>
        struct TEventHandlerStrong : IEventHandler
        {
            TFunction<void(const TEvent &)> Handler;

            TEventHandlerStrong(TFunction<void(const TEvent &)> InHandler)
                : Handler(MoveTemp(InHandler))
            {
            }

            virtual bool IsValid() const override
            {
                return true;
            }

            virtual void Invoke(const IEvent &Event) override
            {
                Handler(static_cast<const TEvent &>(Event));
            }
        };

        struct FSubscriptionEntry
        {
            int32 SubscriptionId;
            TUniquePtr<IEventHandler> Handler;
            FString EventTypeName;
        };

        struct FEventHistoryEntry
        {
            FString EventTypeName;
            FString EventName;
            double Timestamp;
            int32 SubscriberCount;
        };

        template <typename TEvent>
        static FString GetEventTypeName()
        {
            TEvent DefaultEvent;
            return DefaultEvent.GetEventName();
        }

        void Unsubscribe(int32 SubscriptionId);
        void AddToHistory(const FString &EventTypeName, const FString &EventName, int32 SubscriberCount);
        void CleanupInvalidSubscriptions();

        FEventAggregatorConfig Config;
        std::atomic<int32> NextSubscriptionId{0};
        TMap<int32, FSubscriptionEntry> Subscriptions;
        TArray<FEventHistoryEntry> EventHistory;
        std::atomic<int32> TotalEventsPublished{0};
        mutable FCriticalSection Mutex;
        bool bIsInitialized = false;
    };

    template <typename TEvent>
    FEventSubscription FEventAggregator::Subscribe(TFunction<void(const TEvent &)> Handler)
    {
        FScopeLock Lock(&Mutex);

        int32 SubscriptionId = NextSubscriptionId++;

        FSubscriptionEntry Entry;
        Entry.SubscriptionId = SubscriptionId;
        Entry.Handler = MakeUnique<TEventHandlerStrong<TEvent>>(MoveTemp(Handler));
        Entry.EventTypeName = GetEventTypeName<TEvent>();

        Subscriptions.Add(SubscriptionId, MoveTemp(Entry));

        FEventSubscription Subscription;
        Subscription.UnsubscribeCallback = [this, SubscriptionId]()
        {
            Unsubscribe(SubscriptionId);
        };

        return Subscription;
    }

    template <typename TEvent, typename TObject>
    FEventSubscription FEventAggregator::Subscribe(TWeakPtr<TObject> WeakRef, TFunction<void(const TEvent &)> Handler)
    {
        FScopeLock Lock(&Mutex);

        int32 SubscriptionId = NextSubscriptionId++;

        FSubscriptionEntry Entry;
        Entry.SubscriptionId = SubscriptionId;
        Entry.Handler = MakeUnique<TEventHandlerWeak<TEvent>>(
            StaticCastWeakPtr<void>(WeakRef),
            MoveTemp(Handler));
        Entry.EventTypeName = GetEventTypeName<TEvent>();

        Subscriptions.Add(SubscriptionId, MoveTemp(Entry));

        FEventSubscription Subscription;
        Subscription.UnsubscribeCallback = [this, SubscriptionId]()
        {
            Unsubscribe(SubscriptionId);
        };

        return Subscription;
    }

    template <typename TEvent, typename TObject>
    FEventSubscription FEventAggregator::Subscribe(TObject *Object, void (TObject::*Handler)(const TEvent &))
    {
        return Subscribe<TEvent>([Object, Handler](const TEvent &Event)
                                 { (Object->*Handler)(Event); });
    }

    template <typename TEvent>
    void FEventAggregator::Publish(const TEvent &Event)
    {
        FScopeLock Lock(&Mutex);

        if (!bIsInitialized)
        {
            UE_LOG(LogConvaiEditorEvents, Warning, TEXT("Event publish attempted before initialization: %s"),
                   *GetEventTypeName<TEvent>());
            return;
        }

        TArray<int32> InvalidSubscriptions;
        int32 ValidSubscriberCount = 0;

        for (const auto &Pair : Subscriptions)
        {
            const FSubscriptionEntry &Entry = Pair.Value;

            if (Entry.EventTypeName == GetEventTypeName<TEvent>())
            {
                if (Entry.Handler->IsValid())
                {
                    Entry.Handler->Invoke(Event);
                    ValidSubscriberCount++;
                }
                else
                {
                    InvalidSubscriptions.Add(Pair.Key);
                }
            }
        }

        for (int32 InvalidId : InvalidSubscriptions)
        {
            Subscriptions.Remove(InvalidId);

            if (Config.bEnableVerboseLogging)
            {
            }
        }

        TotalEventsPublished++;

        if (Config.bEnableEventHistory)
        {
            AddToHistory(GetEventTypeName<TEvent>(), Event.GetEventName(), ValidSubscriberCount);
        }
    }

    template <typename TEvent>
    void FEventAggregator::PublishGameThread(const TEvent &Event)
    {
        AsyncTask(ENamedThreads::GameThread, [this, Event]()
                  { Publish(Event); });
    }

    template <typename TEvent>
    int32 FEventAggregator::GetSubscriberCount() const
    {
        FScopeLock Lock(&Mutex);

        int32 Count = 0;
        FString EventTypeName = GetEventTypeName<TEvent>();

        for (const auto &Pair : Subscriptions)
        {
            if (Pair.Value.EventTypeName == EventTypeName && Pair.Value.Handler->IsValid())
            {
                Count++;
            }
        }

        return Count;
    }

} // namespace ConvaiEditor
