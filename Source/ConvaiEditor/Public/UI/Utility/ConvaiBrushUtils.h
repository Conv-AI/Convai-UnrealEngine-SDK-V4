/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiBrushUtils.h
 *
 * Centralized utility class for brush creation patterns.
 * Eliminates code duplication in brush creation logic.
 */

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "HAL/CriticalSection.h"
#include "Styling/SlateBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "ConvaiEditor.h"
#include "Services/ConvaiDIContainer.h"

struct CONVAIEDITOR_API FConvaiBrushConfig
{
    float CornerRadius = 4.0f;
    FVector4 BorderThickness = FVector4(0.0f);
    FVector2D ImageSize = FVector2D(16.0f, 16.0f);
    bool bEnableCaching = true;
    FString CacheKey;
};

/** Brush creation utilities. */
class CONVAIEDITOR_API FConvaiBrushUtils
{
public:
    template <typename TBrushType, typename TCreateFunc>
    static TConvaiResult<TSharedPtr<TBrushType>> GetOrCreateBrush(
        TMap<FString, TSharedPtr<TBrushType>> &CacheMap,
        FRWLock &CacheLock,
        const FString &Key,
        TCreateFunc &&CreateBrushFunc)
    {
        {
            FReadScopeLock ReadLock(CacheLock);
            if (TSharedPtr<TBrushType> *ExistingBrush = CacheMap.Find(Key))
            {
                if (ExistingBrush->IsValid())
                {
                    return TConvaiResult<TSharedPtr<TBrushType>>::Success(*ExistingBrush);
                }
            }
        }

        {
            FWriteScopeLock WriteLock(CacheLock);

            if (TSharedPtr<TBrushType> *ExistingBrush = CacheMap.Find(Key))
            {
                if (ExistingBrush->IsValid())
                {
                    return TConvaiResult<TSharedPtr<TBrushType>>::Success(*ExistingBrush);
                }
            }

            TSharedPtr<TBrushType> NewBrush = CreateBrushFunc();
            if (!NewBrush.IsValid())
            {
                return TConvaiResult<TSharedPtr<TBrushType>>::Failure(
                    FString::Printf(TEXT("Failed to create brush for key: %s"), *Key));
            }

            CacheMap.Add(Key, NewBrush);
            return TConvaiResult<TSharedPtr<TBrushType>>::Success(NewBrush);
        }
    }

    static TSharedPtr<FSlateColorBrush> CreateColorBrush(const FLinearColor &Color);
    static TSharedPtr<FSlateImageBrush> CreateImageBrush(const FString &ImagePath, const FVector2D &ImageSize);
    static TSharedPtr<FSlateRoundedBoxBrush> CreateRoundedBoxBrush(
        const FLinearColor &Color,
        float CornerRadius,
        const FVector4 &BorderThickness = FVector4(0.0f));
    static bool ValidateColor(const FLinearColor &Color);
    static bool ValidateImagePath(const FString &ImagePath);
    static FName GenerateBrushKey(const FString &Prefix, const FString &Identifier);
    static TConvaiResult<void> ValidateConfig(const FConvaiBrushConfig &Config);

private:
    FConvaiBrushUtils() = delete;
    ~FConvaiBrushUtils() = delete;
};
