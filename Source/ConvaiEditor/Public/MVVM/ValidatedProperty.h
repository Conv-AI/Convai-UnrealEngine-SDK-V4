/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ValidatedProperty.h
 *
 * Observable property with validation support.
 */

#pragma once

#include "CoreMinimal.h"
#include "MVVM/ObservableProperty.h"
#include "MVVM/ValidationRules.h"
#include "Templates/SharedPointer.h"

namespace ConvaiEditor
{
    /**
     * Observable property with validation support.
     *
     * @tparam T The type of the property value
     */
    template <typename T>
    class TValidatedProperty : public TObservableProperty<T>
    {
    public:
        using Super = TObservableProperty<T>;

        /** Delegate fired when validation state changes */
        DECLARE_MULTICAST_DELEGATE_OneParam(FOnValidationChanged, const FValidationResult &);

        /** Constructor with initial value */
        explicit TValidatedProperty(const T &InitialValue = T())
            : Super(InitialValue)
        {
        }

        /** Add a validation rule */
        void AddRule(TSharedPtr<IValidationRule<T>> Rule)
        {
            if (Rule.IsValid())
            {
                ValidationRules.Add(Rule);
            }
        }

        /** Remove all validation rules */
        void ClearRules()
        {
            ValidationRules.Empty();
        }

        /** Validate the current value */
        FValidationResult Validate() const
        {
            FValidationResult Result = FValidationResult::Success();

            for (const auto &Rule : ValidationRules)
            {
                if (Rule.IsValid())
                {
                    FValidationResult RuleResult = Rule->Validate(this->Get());
                    if (!RuleResult.bIsValid)
                    {
                        Result.bIsValid = false;
                        Result.Errors.Append(RuleResult.Errors);
                    }
                    Result.Warnings.Append(RuleResult.Warnings);
                }
            }

            return Result;
        }

        /** Set value with validation */
        bool SetWithValidation(const T &NewValue, FValidationResult &OutResult)
        {
            // Validate new value
            OutResult = FValidationResult::Success();
            for (const auto &Rule : ValidationRules)
            {
                if (Rule.IsValid())
                {
                    FValidationResult RuleResult = Rule->Validate(NewValue);
                    if (!RuleResult.bIsValid)
                    {
                        OutResult.bIsValid = false;
                        OutResult.Errors.Append(RuleResult.Errors);
                    }
                    OutResult.Warnings.Append(RuleResult.Warnings);
                }
            }

            // Only set if valid
            if (OutResult.bIsValid)
            {
                this->Set(NewValue);
                LastValidationResult = OutResult;
                OnValidationChangedDelegate.Broadcast(OutResult);
                return true;
            }

            // Update last validation result even if invalid
            LastValidationResult = OutResult;
            OnValidationChangedDelegate.Broadcast(OutResult);
            return false;
        }

        /** Set value with validation (simplified version) */
        bool SetWithValidation(const T &NewValue)
        {
            FValidationResult Result;
            return SetWithValidation(NewValue, Result);
        }

        /** Get the last validation result */
        const FValidationResult &GetLastValidationResult() const
        {
            return LastValidationResult;
        }

        /** Check if the current value is valid */
        bool IsValid() const
        {
            return Validate().bIsValid;
        }

        /** Get validation changed delegate */
        FOnValidationChanged &OnValidationChanged() { return OnValidationChangedDelegate; }

        /** Validate and notify */
        void ValidateAndNotify()
        {
            LastValidationResult = Validate();
            OnValidationChangedDelegate.Broadcast(LastValidationResult);
        }

    private:
        /** List of validation rules */
        TArray<TSharedPtr<IValidationRule<T>>> ValidationRules;

        /** Last validation result */
        FValidationResult LastValidationResult;

        /** Delegate fired when validation state changes */
        FOnValidationChanged OnValidationChangedDelegate;
    };

} // namespace ConvaiEditor

//----------------------------------------
// Convenience Type Aliases
//----------------------------------------

/** Validated string property */
using FValidatedString = ConvaiEditor::TValidatedProperty<FString>;

/** Validated integer property */
using FValidatedInt = ConvaiEditor::TValidatedProperty<int32>;

/** Validated float property */
using FValidatedFloat = ConvaiEditor::TValidatedProperty<float>;
