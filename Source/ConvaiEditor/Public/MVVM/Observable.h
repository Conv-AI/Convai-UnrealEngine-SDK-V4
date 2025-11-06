/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * Observable.h
 *
 * Lightweight template for observable values (deprecated).
 */

#pragma once

#include "Delegates/Delegate.h"
#include "Widgets/SWidget.h"

/**
 * Lightweight template for observable values.
 *
 * @deprecated Use ConvaiEditor::TObservableProperty instead
 */
template <typename TValue>
class TObservable
{
public:
    using FOnChanged = TMulticastDelegate<void(const TValue &)>;

    constexpr TObservable() = default;

    explicit constexpr TObservable(const TValue &InValue)
        : Value(InValue) {}

    explicit constexpr TObservable(TValue &&InValue)
        : Value(MoveTemp(InValue)) {}

    ~TObservable()
    {
        RemoveAllBindings();
    }

    TObservable &operator=(const TValue &InValue)
    {
        Set(InValue);
        return *this;
    }

    TObservable &operator=(TValue &&InValue)
    {
        if (!(Value == InValue))
        {
            Value = MoveTemp(InValue);
            OnChanged.Broadcast(Value);
        }
        return *this;
    }

    constexpr const TValue &Get() const { return Value; }

    operator TValue() const { return Value; }

    operator TAttribute<TValue>() const
    {
        return TAttribute<TValue>::Create(TAttribute<TValue>::FGetter::CreateLambda([this]() -> TValue
                                                                                    { return Value; }));
    }

    void Set(const TValue &InValue)
    {
        if (!(Value == InValue))
        {
            Value = InValue;
            OnChanged.Broadcast(Value);
        }
    }

    void Set(TValue &&InValue)
    {
        if (!(Value == InValue))
        {
            Value = MoveTemp(InValue);
            OnChanged.Broadcast(Value);
        }
    }

    FOnChanged &OnValueChanged() { return OnChanged; }

    /** Remove all delegate bindings to prevent memory leaks */
    void RemoveAllBindings()
    {
        OnChanged.Clear();
    }

    /** Get the number of currently bound delegates */
    int32 GetBindingCount() const
    {
        return OnChanged.GetAllObjects().Num();
    }

private:
    TValue Value{};
    FOnChanged OnChanged;
};
