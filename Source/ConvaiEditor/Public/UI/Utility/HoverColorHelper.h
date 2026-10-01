/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * HoverColorHelper.h
 *
 * Utility for creating hover-aware color attributes.
 */

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateColor.h"
#include "Widgets/Input/SButton.h"

/**
 * Hover color configuration.
 */
struct FHoverColorConfig
{
    FLinearColor NormalColor;
    FLinearColor HoverColor;
    FName NormalColorThemeKey;
    FName HoverColorThemeKey;

    FHoverColorConfig()
        : NormalColor(FLinearColor::White), HoverColor(FLinearColor::Green), NormalColorThemeKey(NAME_None), HoverColorThemeKey(NAME_None)
    {
    }

    FHoverColorConfig(const FLinearColor &InNormalColor, const FLinearColor &InHoverColor)
        : NormalColor(InNormalColor), HoverColor(InHoverColor), NormalColorThemeKey(NAME_None), HoverColorThemeKey(NAME_None)
    {
    }

    FHoverColorConfig(const FName &InNormalThemeKey, const FName &InHoverThemeKey)
        : NormalColor(FLinearColor::White), HoverColor(FLinearColor::Green), NormalColorThemeKey(InNormalThemeKey), HoverColorThemeKey(InHoverThemeKey)
    {
    }
};

/**
 * Utility for creating hover-aware color attributes.
 */
class CONVAIEDITOR_API FHoverColorHelper
{
public:
    /** Creates a hover-aware color attribute */
    static TAttribute<FSlateColor> CreateHoverAwareColor(
        const TWeakPtr<SButton> &Button,
        const FHoverColorConfig &Config);

    /** Creates a hover-aware color using theme colors */
    static TAttribute<FSlateColor> CreateHoverAwareColorFromTheme(
        const TWeakPtr<SButton> &Button,
        const FName &NormalThemeKey,
        const FName &HoverThemeKey);

    /** Creates a hover-aware color with explicit colors */
    static TAttribute<FSlateColor> CreateHoverAwareColorExplicit(
        const TWeakPtr<SButton> &Button,
        const FLinearColor &NormalColor,
        const FLinearColor &HoverColor);

private:
    static FLinearColor GetThemeColorOrFallback(const FName &ThemeKey, const FLinearColor &Fallback);
};
