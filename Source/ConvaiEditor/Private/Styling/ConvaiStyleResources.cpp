/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiStyleResources.cpp
 *
 * Implementation of resource management for Slate brushes.
 */

#include "Styling/ConvaiStyleResources.h"
#include "UI/Utility/ConvaiBrushUtils.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Logging/ConvaiEditorThemeLog.h"
#include "ConvaiEditor.h"

TUniquePtr<FConvaiStyleResources> FConvaiStyleResources::Instance = nullptr;

void FConvaiStyleResources::Initialize()
{
    if (!Instance)
    {
        Instance = MakeUnique<FConvaiStyleResources>();
    }
}

void FConvaiStyleResources::Shutdown()
{
    if (Instance)
    {
        Instance->ClearAllBrushes();
        Instance.Reset();
    }
}

FConvaiStyleResources &FConvaiStyleResources::Get()
{
    checkf(Instance, TEXT("FConvaiStyleResources::Get() called before Initialize()"));
    return *Instance;
}

TConvaiResult<TSharedPtr<FSlateColorBrush>> FConvaiStyleResources::GetOrCreateColorBrush(const FName &Key, const FLinearColor &Color)
{
    if (!FConvaiBrushUtils::ValidateColor(Color))
    {
        return TConvaiResult<TSharedPtr<FSlateColorBrush>>::Failure(
            FString::Printf(TEXT("Invalid color provided for brush key: %s"), *Key.ToString()));
    }

    return FConvaiBrushUtils::GetOrCreateBrush<FSlateColorBrush>(
        ColorBrushCache,
        BrushCacheLock,
        Key.ToString(),
        [Color]()
        { return FConvaiBrushUtils::CreateColorBrush(Color); });
}

TConvaiResult<TSharedPtr<FSlateImageBrush>> FConvaiStyleResources::GetOrCreateImageBrush(const FName &Key, const FString &ImagePath, const FVector2D &ImageSize)
{
    if (!FConvaiBrushUtils::ValidateImagePath(ImagePath))
    {
        return TConvaiResult<TSharedPtr<FSlateImageBrush>>::Failure(
            FString::Printf(TEXT("Invalid image path for brush key %s: %s"), *Key.ToString(), *ImagePath));
    }

    if (ImageSize.X <= 0.0f || ImageSize.Y <= 0.0f)
    {
        return TConvaiResult<TSharedPtr<FSlateImageBrush>>::Failure(
            FString::Printf(TEXT("Invalid image size for brush key %s: %s"), *Key.ToString(), *ImageSize.ToString()));
    }

    return FConvaiBrushUtils::GetOrCreateBrush<FSlateImageBrush>(
        ImageBrushCache,
        BrushCacheLock,
        Key.ToString(),
        [ImagePath, ImageSize]()
        { return FConvaiBrushUtils::CreateImageBrush(ImagePath, ImageSize); });
}

TConvaiResult<TSharedPtr<FSlateRoundedBoxBrush>> FConvaiStyleResources::GetOrCreateRoundedBoxBrush(
    const FName &Key,
    const FLinearColor &Color,
    float CornerRadius,
    const FVector4 &BorderThickness)
{
    if (!FConvaiBrushUtils::ValidateColor(Color))
    {
        return TConvaiResult<TSharedPtr<FSlateRoundedBoxBrush>>::Failure(
            FString::Printf(TEXT("Invalid color provided for rounded box brush key: %s"), *Key.ToString()));
    }

    if (CornerRadius < 0.0f)
    {
        return TConvaiResult<TSharedPtr<FSlateRoundedBoxBrush>>::Failure(
            FString::Printf(TEXT("Invalid corner radius for brush key %s: %f"), *Key.ToString(), CornerRadius));
    }

    return FConvaiBrushUtils::GetOrCreateBrush<FSlateRoundedBoxBrush>(
        RoundedBoxBrushCache,
        BrushCacheLock,
        Key.ToString(),
        [Color, CornerRadius, BorderThickness]()
        {
            return FConvaiBrushUtils::CreateRoundedBoxBrush(Color, CornerRadius, BorderThickness);
        });
}

TSharedPtr<FSlateColorBrush> FConvaiStyleResources::CreateTemporaryColorBrush(const FLinearColor &Color)
{
    return FConvaiBrushUtils::CreateColorBrush(Color);
}

TSharedPtr<FSlateRoundedBoxBrush> FConvaiStyleResources::CreateTemporaryRoundedBoxBrush(const FLinearColor &Color, float CornerRadius)
{
    return FConvaiBrushUtils::CreateRoundedBoxBrush(Color, CornerRadius);
}

void FConvaiStyleResources::ClearBrush(const FName &Key)
{
    FWriteScopeLock WriteLock(BrushCacheLock);

    FString KeyString = Key.ToString();
    ColorBrushCache.Remove(KeyString);
    ImageBrushCache.Remove(KeyString);
    RoundedBoxBrushCache.Remove(KeyString);
}

void FConvaiStyleResources::ClearAllBrushes()
{
    FWriteScopeLock WriteLock(BrushCacheLock);

    const int32 TotalBrushes = ColorBrushCache.Num() + ImageBrushCache.Num() + RoundedBoxBrushCache.Num();

    ColorBrushCache.Empty();
    ImageBrushCache.Empty();
    RoundedBoxBrushCache.Empty();
}

FConvaiStyleResources::FBrushStats FConvaiStyleResources::GetBrushStats() const
{
    FReadScopeLock ReadLock(BrushCacheLock);

    FBrushStats Stats;
    Stats.ColorBrushCount = ColorBrushCache.Num();
    Stats.ImageBrushCount = ImageBrushCache.Num();
    Stats.RoundedBoxBrushCount = RoundedBoxBrushCache.Num();

    Stats.TotalMemoryUsage =
        (Stats.ColorBrushCount * sizeof(FSlateColorBrush)) +
        (Stats.ImageBrushCount * sizeof(FSlateImageBrush)) +
        (Stats.RoundedBoxBrushCount * sizeof(FSlateRoundedBoxBrush));

    return Stats;
}

bool FConvaiStyleResources::ValidateImagePath(const FString &ImagePath) const
{
    return FConvaiBrushUtils::ValidateImagePath(ImagePath);
}

bool FConvaiStyleResources::ValidateColor(const FLinearColor &Color) const
{
    return FConvaiBrushUtils::ValidateColor(Color);
}

FString FConvaiStyleResources::GenerateBrushKey(const FString &Prefix, const FString &Identifier) const
{
    return FConvaiBrushUtils::GenerateBrushKey(Prefix, Identifier).ToString();
}
