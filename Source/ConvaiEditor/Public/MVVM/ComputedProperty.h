/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ComputedProperty.h
 *
 * Property that derives its value from other properties.
 */

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Widgets/SWidget.h"

namespace ConvaiEditor
{
    /**
     * Property that derives its value from other properties.
     *
     * @tparam T The type of the computed value
     */
    template <typename T>
    class TComputedProperty
    {
    public:
        using FComputeFunction = TFunction<T()>;

        /** Constructor with compute function */
        explicit TComputedProperty(FComputeFunction InComputeFunc)
            : ComputeFunc(InComputeFunc), bIsDirty(true)
        {
        }

        /** Get computed value (cached) */
        const T &Get()
        {
            if (bIsDirty)
            {
                CachedValue = ComputeFunc();
                bIsDirty = false;
            }
            return CachedValue;
        }

        /** Mark as dirty (needs recomputation) */
        void Invalidate()
        {
            bIsDirty = true;
        }

        /** Force recomputation */
        void Recompute()
        {
            bIsDirty = true;
            Get();
        }

        /** Check if the value is dirty (needs recomputation) */
        bool IsDirty() const
        {
            return bIsDirty;
        }

        /** Create TAttribute for Slate binding */
        TAttribute<T> AsAttribute()
        {
            return TAttribute<T>::Create(TAttribute<T>::FGetter::CreateLambda([this]()
                                                                              { return this->Get(); }));
        }

        /** Implicit conversion operator for convenience */
        operator const T &() { return Get(); }

    private:
        /** Function that computes the value */
        FComputeFunction ComputeFunc;

        /** Cached computed value */
        T CachedValue;

        /** Flag indicating if value needs recomputation */
        bool bIsDirty;
    };

    /**
     * Property transformation helpers
     * Common transformations for property values
     */
    namespace PropertyTransformers
    {
        /** Transform string to uppercase */
        inline TFunction<FString(const FString &)> ToUpper()
        {
            return [](const FString &Input)
            {
                return Input.ToUpper();
            };
        }

        /** Transform string to lowercase */
        inline TFunction<FString(const FString &)> ToLower()
        {
            return [](const FString &Input)
            {
                return Input.ToLower();
            };
        }

        /** Trim whitespace from string */
        inline TFunction<FString(const FString &)> Trim()
        {
            return [](const FString &Input)
            {
                return Input.TrimStartAndEnd();
            };
        }

        /** Clamp numeric value to range */
        template <typename T>
        inline TFunction<T(const T &)> Clamp(T Min, T Max)
        {
            return [Min, Max](const T &Input)
            {
                return FMath::Clamp(Input, Min, Max);
            };
        }

        /** Round float to nearest integer */
        inline TFunction<int32(const float &)> Round()
        {
            return [](const float &Input)
            {
                return FMath::RoundToInt(Input);
            };
        }

        /** Convert integer to string */
        inline TFunction<FString(const int32 &)> IntToString()
        {
            return [](const int32 &Input)
            {
                return FString::FromInt(Input);
            };
        }

        /** Convert float to string with precision */
        inline TFunction<FString(const float &)> FloatToString(int32 Precision = 2)
        {
            return [Precision](const float &Input)
            {
                return FString::SanitizeFloat(Input, Precision);
            };
        }

        /** Convert string to FText */
        inline TFunction<FText(const FString &)> StringToText()
        {
            return [](const FString &Input)
            {
                return FText::FromString(Input);
            };
        }

        /** Format string with arguments */
        inline TFunction<FString(const FString &)> Format(const TArray<FString> &Args)
        {
            return [Args](const FString &Input)
            {
                FString Result = Input;
                for (int32 i = 0; i < Args.Num(); ++i)
                {
                    Result = Result.Replace(*FString::Printf(TEXT("{%d}"), i), *Args[i]);
                }
                return Result;
            };
        }

    } // namespace PropertyTransformers

} // namespace ConvaiEditor
