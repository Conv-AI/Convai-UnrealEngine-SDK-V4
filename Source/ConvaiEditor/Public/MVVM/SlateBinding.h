/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SlateBinding.h
 *
 * Two-way binding helpers for Slate widgets.
 */

#pragma once

#include "CoreMinimal.h"
#include "MVVM/ObservableProperty.h"
#include "MVVM/BindableProperty.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SCheckBox.h"

namespace ConvaiEditor
{
    namespace Binding
    {
        // Forward declarations for helper functions
        namespace Internal
        {
            CONVAIEDITOR_API FText ConvertValueToText(const FString &Value);
            CONVAIEDITOR_API bool TryParseTextToValue(const FText &Text, FString &OutValue);
            CONVAIEDITOR_API bool TryParseTextToValue(const FText &Text, int32 &OutValue);
            CONVAIEDITOR_API bool TryParseTextToValue(const FText &Text, float &OutValue);
            CONVAIEDITOR_API bool TryParseTextToValue(const FText &Text, double &OutValue);
            CONVAIEDITOR_API bool TryParseTextToValue(const FText &Text, bool &OutValue);
        }

        /**
         * Binds an observable value to editable widgets, creating a two-way binding.
         *
         * @param Observable The observable value to bind to
         * @return A binding object for use with Slate widgets
         */
        template <typename TValue>
        struct TEditableBinding
        {
            TEditableBinding(ConvaiEditor::TObservableProperty<TValue> &InObservable)
                : Observable(InObservable)
            {
            }

            FText GetAsText() const
            {
                return FText::FromString(LexToString(Observable.Get()));
            }

            void SetFromText(const FText &InText, ETextCommit::Type CommitType)
            {
                if (CommitType == ETextCommit::OnEnter || CommitType == ETextCommit::OnUserMovedFocus)
                {
                    TValue ParsedValue;
                    if (Internal::TryParseTextToValue(InText, ParsedValue))
                    {
                        Observable.Set(MoveTemp(ParsedValue));
                    }
                }
            }

            TAttribute<FText> GetTextAttribute() const
            {
                return TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda([this]() -> FText
                                                                                          { return GetAsText(); }));
            }

            FOnTextCommitted GetOnTextCommittedDelegate() const
            {
                return FOnTextCommitted::CreateLambda([this](const FText &InText, ETextCommit::Type CommitType)
                                                      { SetFromText(InText, CommitType); });
            }

            void SetFromString(const FString &InString)
            {
                TValue ParsedValue;
                if (LexTryParseString(ParsedValue, *InString))
                {
                    Observable.Set(MoveTemp(ParsedValue));
                }
            }

            void HandleTextChanged(const FText &InText)
            {
                TValue ParsedValue;
                if (Internal::TryParseTextToValue(InText, ParsedValue))
                {
                    Observable.Set(MoveTemp(ParsedValue));
                }
            }

            FOnTextChanged GetOnTextChangedDelegate() const
            {
                return FOnTextChanged::CreateLambda([this](const FText &InText)
                                                    { HandleTextChanged(InText); });
            }

            TValue GetValue() const
            {
                return Observable.Get();
            }

            void SetValue(TValue InValue)
            {
                Observable.Set(InValue);
            }

            TAttribute<TValue> GetValueAttribute() const
            {
                return TAttribute<TValue>::Create(TAttribute<TValue>::FGetter::CreateLambda([this]() -> TValue
                                                                                            { return GetValue(); }));
            }

            typename TSlateDelegates<TValue>::FOnValueChanged GetOnValueChangedDelegate() const
            {
                return typename TSlateDelegates<TValue>::FOnValueChanged::CreateLambda([this](TValue InValue)
                                                                                       { SetValue(InValue); });
            }

            ECheckBoxState GetCheckState() const
            {
                static_assert(std::is_same<TValue, bool>::value, "GetCheckState() only works with TObservable<bool>");
                return Observable.Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            }

            void SetCheckState(ECheckBoxState NewState)
            {
                static_assert(std::is_same<TValue, bool>::value, "SetCheckState() only works with TObservable<bool>");
                Observable.Set(NewState == ECheckBoxState::Checked);
            }

            TAttribute<ECheckBoxState> GetCheckStateAttribute() const
            {
                return TAttribute<ECheckBoxState>::Create(TAttribute<ECheckBoxState>::FGetter::CreateLambda([this]() -> ECheckBoxState
                                                                                                            { return GetCheckState(); }));
            }

            FOnCheckStateChanged GetOnCheckStateChangedDelegate() const
            {
                return FOnCheckStateChanged::CreateLambda([this](ECheckBoxState NewState)
                                                          { SetCheckState(NewState); });
            }

            ConvaiEditor::TObservableProperty<TValue> &Observable;
        };

        /** Creates a two-way binding to an observable value */
        template <typename TValue>
        TEditableBinding<TValue> BindEditable(ConvaiEditor::TObservableProperty<TValue> &Observable)
        {
            return TEditableBinding<TValue>(Observable);
        }
    }
} // namespace ConvaiEditor
