/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiStyleResources.h
 *
 * Centralized resource management for Slate brushes and style assets.
 */

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Containers/Map.h"
#include "HAL/CriticalSection.h"
#include "Styling/SlateBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Services/ConvaiDIContainer.h"

/** Centralized brush resource manager with thread-safe caching. */
class CONVAIEDITOR_API FConvaiStyleResources
{
public:
    static void Initialize();
    static void Shutdown();
    static FConvaiStyleResources &Get();

    TConvaiResult<TSharedPtr<FSlateColorBrush>> GetOrCreateColorBrush(const FName &Key, const FLinearColor &Color);

    TConvaiResult<TSharedPtr<FSlateImageBrush>> GetOrCreateImageBrush(const FName &Key, const FString &ImagePath, const FVector2D &ImageSize);

    TConvaiResult<TSharedPtr<FSlateRoundedBoxBrush>> GetOrCreateRoundedBoxBrush(
        const FName &Key,
        const FLinearColor &Color,
        float CornerRadius,
        const FVector4 &BorderThickness = FVector4(0.0f));

    TSharedPtr<FSlateColorBrush> CreateTemporaryColorBrush(const FLinearColor &Color);

    TSharedPtr<FSlateRoundedBoxBrush> CreateTemporaryRoundedBoxBrush(const FLinearColor &Color, float CornerRadius);

    void ClearBrush(const FName &Key);
    void ClearAllBrushes();

    struct FBrushStats
    {
        int32 ColorBrushCount = 0;
        int32 ImageBrushCount = 0;
        int32 RoundedBoxBrushCount = 0;
        int32 TotalMemoryUsage = 0;
    };

    FBrushStats GetBrushStats() const;

    FConvaiStyleResources() = default;
    ~FConvaiStyleResources() = default;

private:
    FConvaiStyleResources(const FConvaiStyleResources &) = delete;
    FConvaiStyleResources &operator=(const FConvaiStyleResources &) = delete;
    FConvaiStyleResources(FConvaiStyleResources &&) = delete;
    FConvaiStyleResources &operator=(FConvaiStyleResources &&) = delete;

private:
    mutable FRWLock BrushCacheLock;

    TMap<FString, TSharedPtr<FSlateColorBrush>> ColorBrushCache;
    TMap<FString, TSharedPtr<FSlateImageBrush>> ImageBrushCache;
    TMap<FString, TSharedPtr<FSlateRoundedBoxBrush>> RoundedBoxBrushCache;

    static TUniquePtr<FConvaiStyleResources> Instance;

    bool ValidateImagePath(const FString &ImagePath) const;
    bool ValidateColor(const FLinearColor &Color) const;
    FString GenerateBrushKey(const FString &Prefix, const FString &Identifier) const;
};

/** RAII helper for temporary brush lifetime management. */
template <typename TBrushType>
class TConvaiBrushGuard
{
public:
    explicit TConvaiBrushGuard(TSharedPtr<TBrushType> InBrush)
        : Brush(MoveTemp(InBrush))
    {
    }

    ~TConvaiBrushGuard()
    {
    }

    const TBrushType *Get() const { return Brush.Get(); }
    const TBrushType *operator->() const { return Brush.Get(); }
    const TBrushType &operator*() const { return *Brush.Get(); }

    bool IsValid() const { return Brush.IsValid(); }

private:
    TSharedPtr<TBrushType> Brush;
};

using FColorBrushGuard = TConvaiBrushGuard<FSlateColorBrush>;
using FImageBrushGuard = TConvaiBrushGuard<FSlateImageBrush>;
using FRoundedBoxBrushGuard = TConvaiBrushGuard<FSlateRoundedBoxBrush>;
