/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ObservableProperty.h
 *
 * Thread-safe observable property with automatic change notification.
 */

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HAL/CriticalSection.h"
#include "Templates/Function.h"
#include "ConvaiEditor.h"

namespace ConvaiEditor
{
    /**
     * Thread-safe observable property with automatic change notification.
     *
     * @tparam T The type of the property value
     */
    template <typename T>
    class TObservableProperty
    {
    public:
        /** Delegate fired when property value changes (after the change) */
        DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPropertyChanged, const T & /*OldValue*/, const T & /*NewValue*/);

        /** Delegate fired before property value changes (can be cancelled) */
        DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnPropertyChanging, const T & /*OldValue*/, const T & /*NewValue*/, bool & /*bCancel*/);

        /** Constructor with initial value */
        explicit TObservableProperty(const T &InitialValue = T())
            : Value(InitialValue), bSuppressNotifications(false)
        {
        }

        /** Get current value (thread-safe) */
        const T &Get() const
        {
            FScopeLock Lock(&Mutex);
            return Value;
        }

        /** Set new value with automatic notification */
        void Set(const T &NewValue)
        {
            FScopeLock Lock(&Mutex);

            // Check if value actually changed
            if (!HasChanged(Value, NewValue))
            {
                return; // No change, no notification
            }

            // Validate new value
            if (Validator && !Validator(NewValue))
            {
                UE_LOG(LogConvaiEditor, Warning, TEXT("ObservableProperty: Validation failed for new value"));
                return;
            }

            // Fire "changing" event (can be cancelled)
            bool bCancel = false;
            if (!bSuppressNotifications)
            {
                OnPropertyChangingDelegate.Broadcast(Value, NewValue, bCancel);
                if (bCancel)
                {
                    return; // Change cancelled
                }
            }

            // Apply transformation if set
            T TransformedValue = Transformer ? Transformer(NewValue) : NewValue;

            // Store old value for notification
            T OldValue = Value;
            Value = TransformedValue;

            // Fire "changed" event
            if (!bSuppressNotifications)
            {
                OnPropertyChangedDelegate.Broadcast(OldValue, Value);
            }
        }

        /** Set value without triggering notifications */
        void SetSilent(const T &NewValue)
        {
            FScopeLock Lock(&Mutex);
            Value = NewValue;
        }

        /** Get property changed delegate */
        FOnPropertyChanged &OnChanged() { return OnPropertyChangedDelegate; }

        /** Get property changing delegate */
        FOnPropertyChanging &OnChanging() { return OnPropertyChangingDelegate; }

        /** Set validator function */
        void SetValidator(TFunction<bool(const T &)> InValidator)
        {
            Validator = InValidator;
        }

        /** Set transformer function */
        void SetTransformer(TFunction<T(const T &)> InTransformer)
        {
            Transformer = InTransformer;
        }

        /** Suppress notifications temporarily */
        void SetSuppressNotifications(bool bSuppress)
        {
            bSuppressNotifications = bSuppress;
        }

        /** Force notification even if value hasn't changed */
        void ForceNotify()
        {
            FScopeLock Lock(&Mutex);
            if (!bSuppressNotifications)
            {
                OnPropertyChangedDelegate.Broadcast(Value, Value);
            }
        }

        /** Implicit conversion operator for convenience */
        operator const T &() const { return Get(); }

        /** Assignment operator for convenience */
        TObservableProperty &operator=(const T &NewValue)
        {
            Set(NewValue);
            return *this;
        }

    private:
        /**
         * Check if value has changed
         * Uses equality operator if available, otherwise assumes changed
         *
         * @param Old The old value
         * @param New The new value
         * @return True if the value has changed, false otherwise
         */
        bool HasChanged(const T &Old, const T &New) const
        {
            // For pointer types, compare pointers
            if constexpr (TIsPointer<T>::Value)
            {
                return Old != New;
            }
            // For all other types, try to use equality operator
            // If type doesn't support ==, this will cause a compile error
            // which is better than silently always notifying
            else
            {
                return !(Old == New);
            }
        }

        /** Current value */
        T Value;

        /** Validator function (optional) */
        TFunction<bool(const T &)> Validator;

        /** Transformer function (optional) */
        TFunction<T(const T &)> Transformer;

        /** Delegate fired when value changes */
        FOnPropertyChanged OnPropertyChangedDelegate;

        /** Delegate fired before value changes */
        FOnPropertyChanging OnPropertyChangingDelegate;

        /** Mutex for thread-safe operations */
        mutable FCriticalSection Mutex;

        /** Flag to suppress notifications */
        bool bSuppressNotifications;
    };

    /**
     * TScopedNotificationSuppressor - RAII helper to suppress notifications temporarily
     *
     * @tparam T The type of the observable property
     */
    template <typename T>
    class TScopedNotificationSuppressor
    {
    public:
        /** Constructor - suppresses notifications */
        explicit TScopedNotificationSuppressor(TObservableProperty<T> &Property)
            : PropertyRef(Property)
        {
            PropertyRef.SetSuppressNotifications(true);
        }

        /** Destructor - re-enables notifications */
        ~TScopedNotificationSuppressor()
        {
            PropertyRef.SetSuppressNotifications(false);
        }

        TScopedNotificationSuppressor(const TScopedNotificationSuppressor &) = delete;
        TScopedNotificationSuppressor &operator=(const TScopedNotificationSuppressor &) = delete;
        TScopedNotificationSuppressor(TScopedNotificationSuppressor &&) = delete;
        TScopedNotificationSuppressor &operator=(TScopedNotificationSuppressor &&) = delete;

    private:
        TObservableProperty<T> &PropertyRef;
    };

} // namespace ConvaiEditor

//----------------------------------------
// Convenience Type Aliases
//----------------------------------------

/** Observable boolean property */
using FObservableBool = ConvaiEditor::TObservableProperty<bool>;

/** Observable integer property */
using FObservableInt = ConvaiEditor::TObservableProperty<int32>;

/** Observable float property */
using FObservableFloat = ConvaiEditor::TObservableProperty<float>;

/** Observable string property */
using FObservableString = ConvaiEditor::TObservableProperty<FString>;

/** Observable text property */
using FObservableText = ConvaiEditor::TObservableProperty<FText>;
