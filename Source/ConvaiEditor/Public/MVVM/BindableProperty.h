/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * BindableProperty.h
 *
 * Observable property with two-way binding support for Slate.
 */

#pragma once

#include "CoreMinimal.h"
#include "MVVM/ObservableProperty.h"
#include "Widgets/SWidget.h"

namespace ConvaiEditor
{
    /** Observable property with two-way binding support for Slate. */
    template <typename T>
    class TBindableProperty : public TObservableProperty<T>
    {
    public:
        using Super = TObservableProperty<T>;

        explicit TBindableProperty(const T &InitialValue = T())
            : Super(InitialValue)
        {
        }

        TAttribute<T> AsAttribute()
        {
            return TAttribute<T>::Create(TAttribute<T>::FGetter::CreateLambda([this]()
                                                                              { return this->Get(); }));
        }

        template <typename TOutput>
        TAttribute<TOutput> AsAttribute(TFunction<TOutput(const T &)> TransformFunc)
        {
            return TAttribute<TOutput>::Create(TAttribute<TOutput>::FGetter::CreateLambda([this, TransformFunc]()
                                                                                          { return TransformFunc(this->Get()); }));
        }

        void BindToAttribute(TAttribute<T> Attribute)
        {
            BoundAttribute = Attribute;
            bHasBoundAttribute = true;
        }

        void BindTwoWay(TAttribute<T> GetterAttribute, TFunction<void(const T &)> SetterCallback)
        {
            BindToAttribute(GetterAttribute);

            this->OnChanged().AddLambda([SetterCallback](const T &Old, const T &New)
                                        { SetterCallback(New); });
        }

        void PollBoundAttribute()
        {
            if (bHasBoundAttribute && BoundAttribute.IsSet())
            {
                T CurrentValue = BoundAttribute.Get();
                if (this->Get() != CurrentValue)
                {
                    this->Set(CurrentValue);
                }
            }
        }

        bool HasBoundAttribute() const
        {
            return bHasBoundAttribute;
        }

        void Unbind()
        {
            bHasBoundAttribute = false;
            BoundAttribute = TAttribute<T>();
        }

    private:
        TAttribute<T> BoundAttribute;
        bool bHasBoundAttribute = false;
    };

} // namespace ConvaiEditor

using FBindableString = ConvaiEditor::TBindableProperty<FString>;
using FBindableBool = ConvaiEditor::TBindableProperty<bool>;
using FBindableInt = ConvaiEditor::TBindableProperty<int32>;
using FBindableFloat = ConvaiEditor::TBindableProperty<float>;
using FBindableText = ConvaiEditor::TBindableProperty<FText>;
