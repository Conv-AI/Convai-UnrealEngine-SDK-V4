/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ValidationRules.h
 *
 * Validation rules and result types for validated properties.
 */

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "Templates/SharedPointer.h"
#include "Templates/Function.h"
#include "Internationalization/Regex.h"

namespace ConvaiEditor
{
    /** Result of a validation operation. */
    struct CONVAIEDITOR_API FValidationResult
    {
        /** True if validation passed, false otherwise */
        bool bIsValid = true;

        /** List of validation errors */
        TArray<FText> Errors;

        /** List of validation warnings (non-blocking) */
        TArray<FText> Warnings;

        /** Create a successful validation result */
        static FValidationResult Success()
        {
            return FValidationResult{true, {}, {}};
        }

        /** Create a failed validation result with an error */
        static FValidationResult Failure(const FText &Error)
        {
            FValidationResult Result;
            Result.bIsValid = false;
            Result.Errors.Add(Error);
            return Result;
        }

        /** Add an error to the result */
        void AddError(const FText &Error)
        {
            bIsValid = false;
            Errors.Add(Error);
        }

        /** Add a warning to the result */
        void AddWarning(const FText &Warning)
        {
            Warnings.Add(Warning);
        }

        /** Get the first error message */
        FText GetFirstError() const
        {
            return Errors.Num() > 0 ? Errors[0] : FText::GetEmpty();
        }

        /** Get the first warning message */
        FText GetFirstWarning() const
        {
            return Warnings.Num() > 0 ? Warnings[0] : FText::GetEmpty();
        }

        /** Convert to string for logging */
        FString ToString() const
        {
            FString Result;
            for (const FText &Error : Errors)
            {
                Result += FString::Printf(TEXT("Error: %s\n"), *Error.ToString());
            }
            for (const FText &Warning : Warnings)
            {
                Result += FString::Printf(TEXT("Warning: %s\n"), *Warning.ToString());
            }
            return Result;
        }
    };

    /**
     * IValidationRule - Interface for validation rules
     *
     * @tparam T The type of value to validate
     */
    template <typename T>
    class IValidationRule
    {
    public:
        virtual ~IValidationRule() = default;

        /** Validate a value */
        virtual FValidationResult Validate(const T &Value) const = 0;
    };

    /**
     * TRequiredRule - Validation rule that requires a non-empty value
     *
     * @tparam T The type of value to validate
     */
    template <typename T>
    class TRequiredRule : public IValidationRule<T>
    {
    public:
        virtual FValidationResult Validate(const T &Value) const override
        {
            if constexpr (TIsPointer<T>::Value)
            {
                return Value != nullptr ? FValidationResult::Success() : FValidationResult::Failure(NSLOCTEXT("Validation", "Required", "This field is required"));
            }
            else if constexpr (std::is_same_v<T, FString>)
            {
                return !Value.IsEmpty() ? FValidationResult::Success() : FValidationResult::Failure(NSLOCTEXT("Validation", "Required", "This field is required"));
            }
            else if constexpr (std::is_same_v<T, FText>)
            {
                return !Value.IsEmpty() ? FValidationResult::Success() : FValidationResult::Failure(NSLOCTEXT("Validation", "Required", "This field is required"));
            }
            else
            {
                return FValidationResult::Success();
            }
        }
    };

    /**
     * TRangeRule - Validation rule that checks if a value is within a range
     *
     * @tparam T The type of value to validate (must support < and > operators)
     */
    template <typename T>
    class TRangeRule : public IValidationRule<T>
    {
    public:
        /** Constructor */
        TRangeRule(T InMin, T InMax) : Min(InMin), Max(InMax) {}

        virtual FValidationResult Validate(const T &Value) const override
        {
            if (Value < Min || Value > Max)
            {
                return FValidationResult::Failure(
                    FText::Format(NSLOCTEXT("Validation", "Range", "Value must be between {0} and {1}"),
                                  FText::AsNumber(Min), FText::AsNumber(Max)));
            }
            return FValidationResult::Success();
        }

    private:
        T Min, Max;
    };

    /** FStringLengthRule - Validation rule that checks string length */
    class CONVAIEDITOR_API FStringLengthRule : public IValidationRule<FString>
    {
    public:
        /** Constructor */
        FStringLengthRule(int32 InMinLength, int32 InMaxLength = MAX_int32)
            : MinLength(InMinLength), MaxLength(InMaxLength) {}

        virtual FValidationResult Validate(const FString &Value) const override
        {
            int32 Length = Value.Len();
            if (Length < MinLength)
            {
                return FValidationResult::Failure(
                    FText::Format(NSLOCTEXT("Validation", "MinLength", "Minimum length is {0} characters"),
                                  FText::AsNumber(MinLength)));
            }
            if (Length > MaxLength)
            {
                return FValidationResult::Failure(
                    FText::Format(NSLOCTEXT("Validation", "MaxLength", "Maximum length is {0} characters"),
                                  FText::AsNumber(MaxLength)));
            }
            return FValidationResult::Success();
        }

    private:
        int32 MinLength, MaxLength;
    };

    /** FRegexRule - Validation rule that checks if string matches a regex pattern */
    class CONVAIEDITOR_API FRegexRule : public IValidationRule<FString>
    {
    public:
        /** Constructor */
        FRegexRule(const FString &InPattern, const FText &InErrorMessage)
            : Pattern(InPattern), ErrorMessage(InErrorMessage) {}

        virtual FValidationResult Validate(const FString &Value) const override
        {
            FRegexPattern RegexPattern(Pattern);
            FRegexMatcher Matcher(RegexPattern, Value);

            return Matcher.FindNext() ? FValidationResult::Success() : FValidationResult::Failure(ErrorMessage);
        }

    private:
        FString Pattern;
        FText ErrorMessage;
    };

    /** FEmailRule - Validation rule that checks if string is a valid email */
    class CONVAIEDITOR_API FEmailRule : public IValidationRule<FString>
    {
    public:
        virtual FValidationResult Validate(const FString &Value) const override
        {
            // Simple email regex pattern
            FString EmailPattern = TEXT("^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}$");
            FRegexPattern RegexPattern(EmailPattern);
            FRegexMatcher Matcher(RegexPattern, Value);

            return Matcher.FindNext() ? FValidationResult::Success() : FValidationResult::Failure(NSLOCTEXT("Validation", "EmailFormat", "Invalid email format"));
        }
    };

    /**
     * FCustomRule - Validation rule that uses a custom validation function
     *
     * @tparam T The type of value to validate
     */
    template <typename T>
    class TCustomRule : public IValidationRule<T>
    {
    public:
        /** Constructor */
        TCustomRule(TFunction<bool(const T &)> InValidationFunc, const FText &InErrorMessage)
            : ValidationFunc(InValidationFunc), ErrorMessage(InErrorMessage) {}

        virtual FValidationResult Validate(const T &Value) const override
        {
            return ValidationFunc(Value) ? FValidationResult::Success() : FValidationResult::Failure(ErrorMessage);
        }

    private:
        TFunction<bool(const T &)> ValidationFunc;
        FText ErrorMessage;
    };

} // namespace ConvaiEditor
