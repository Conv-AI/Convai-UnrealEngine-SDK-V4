/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * HoverColorHelper.cpp
 *
 * Implementation of hover color utilities for buttons.
 */

#include "UI/Utility/HoverColorHelper.h"
#include "Styling/ConvaiStyle.h"

TAttribute<FSlateColor> FHoverColorHelper::CreateHoverAwareColor(
    const TWeakPtr<SButton> &Button,
    const FHoverColorConfig &Config)
{
    return TAttribute<FSlateColor>::CreateLambda([Button, Config]() -> FSlateColor
                                                 {
        auto ButtonPin = Button.Pin();
        if (ButtonPin.IsValid() && ButtonPin->IsHovered())
        {
            if (!Config.HoverColorThemeKey.IsNone())
            {
                return FSlateColor(GetThemeColorOrFallback(Config.HoverColorThemeKey, Config.HoverColor));
            }
            return FSlateColor(Config.HoverColor);
        }
        
        if (!Config.NormalColorThemeKey.IsNone())
        {
            return FSlateColor(GetThemeColorOrFallback(Config.NormalColorThemeKey, Config.NormalColor));
        }
        return FSlateColor(Config.NormalColor); });
}

TAttribute<FSlateColor> FHoverColorHelper::CreateHoverAwareColorFromTheme(
    const TWeakPtr<SButton> &Button,
    const FName &NormalThemeKey,
    const FName &HoverThemeKey)
{
    FHoverColorConfig Config;
    Config.NormalColorThemeKey = NormalThemeKey;
    Config.HoverColorThemeKey = HoverThemeKey;
    return CreateHoverAwareColor(Button, Config);
}

TAttribute<FSlateColor> FHoverColorHelper::CreateHoverAwareColorExplicit(
    const TWeakPtr<SButton> &Button,
    const FLinearColor &NormalColor,
    const FLinearColor &HoverColor)
{
    FHoverColorConfig Config(NormalColor, HoverColor);
    return CreateHoverAwareColor(Button, Config);
}

FLinearColor FHoverColorHelper::GetThemeColorOrFallback(const FName &ThemeKey, const FLinearColor &Fallback)
{
    const ISlateStyle &Style = FConvaiStyle::Get();
    return Style.GetColor(ThemeKey);
}
