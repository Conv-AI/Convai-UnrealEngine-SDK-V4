/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiBrushUtils.cpp
 *
 * Implementation of centralized brush creation utilities.
 */

#include "UI/Utility/ConvaiBrushUtils.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "ConvaiEditor.h"

TSharedPtr<FSlateColorBrush> FConvaiBrushUtils::CreateColorBrush(const FLinearColor &Color)
{
    if (!ValidateColor(Color))
    {
        return nullptr;
    }

    return MakeShared<FSlateColorBrush>(Color);
}

TSharedPtr<FSlateImageBrush> FConvaiBrushUtils::CreateImageBrush(const FString &ImagePath, const FVector2D &ImageSize)
{
    if (!ValidateImagePath(ImagePath))
    {
        return nullptr;
    }

    if (ImageSize.X <= 0.0f || ImageSize.Y <= 0.0f)
    {
        return nullptr;
    }

    return MakeShared<FSlateImageBrush>(FName(*ImagePath), ImageSize);
}

TSharedPtr<FSlateRoundedBoxBrush> FConvaiBrushUtils::CreateRoundedBoxBrush(
    const FLinearColor &Color,
    float CornerRadius,
    const FVector4 &BorderThickness)
{
    if (!ValidateColor(Color))
    {
        return nullptr;
    }

    if (CornerRadius < 0.0f)
    {
        return nullptr;
    }

    TSharedPtr<FSlateRoundedBoxBrush> NewBrush;

    if (BorderThickness == FVector4(0.0f))
    {
        NewBrush = MakeShared<FSlateRoundedBoxBrush>(Color, CornerRadius);
    }
    else
    {
        NewBrush = MakeShared<FSlateRoundedBoxBrush>(
            FLinearColor::Transparent,
            CornerRadius,
            Color,
            BorderThickness.X);
    }

    return NewBrush;
}

bool FConvaiBrushUtils::ValidateColor(const FLinearColor &Color)
{
    if (!FMath::IsFinite(Color.R) || !FMath::IsFinite(Color.G) ||
        !FMath::IsFinite(Color.B) || !FMath::IsFinite(Color.A))
    {
        return false;
    }

    if (Color.R < 0.0f || Color.G < 0.0f || Color.B < 0.0f || Color.A < 0.0f ||
        Color.R > 10.0f || Color.G > 10.0f || Color.B > 10.0f || Color.A > 1.0f)
    {
        return false;
    }

    return true;
}

bool FConvaiBrushUtils::ValidateImagePath(const FString &ImagePath)
{
    if (ImagePath.IsEmpty())
    {
        return false;
    }

    if (!FPaths::FileExists(ImagePath))
    {
        return false;
    }

    FString Extension = FPaths::GetExtension(ImagePath, true);
    if (!Extension.Equals(TEXT(".png"), ESearchCase::IgnoreCase) &&
        !Extension.Equals(TEXT(".jpg"), ESearchCase::IgnoreCase) &&
        !Extension.Equals(TEXT(".jpeg"), ESearchCase::IgnoreCase))
    {
        return false;
    }

    return true;
}

FName FConvaiBrushUtils::GenerateBrushKey(const FString &Prefix, const FString &Identifier)
{
    return FName(*FString::Printf(TEXT("%s.%s"), *Prefix, *Identifier));
}

TConvaiResult<void> FConvaiBrushUtils::ValidateConfig(const FConvaiBrushConfig &Config)
{
    if (Config.CornerRadius < 0.0f)
    {
        return TConvaiResult<void>::Failure(TEXT("Corner radius cannot be negative"));
    }

    if (Config.ImageSize.X <= 0.0f || Config.ImageSize.Y <= 0.0f)
    {
        return TConvaiResult<void>::Failure(TEXT("Image size must be positive"));
    }

    if (Config.BorderThickness.X < 0.0f || Config.BorderThickness.Y < 0.0f ||
        Config.BorderThickness.Z < 0.0f || Config.BorderThickness.W < 0.0f)
    {
        return TConvaiResult<void>::Failure(TEXT("Border thickness cannot be negative"));
    }

    return TConvaiResult<void>::Success();
}
