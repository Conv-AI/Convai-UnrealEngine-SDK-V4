/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SVerticalDivider.h
 *
 * Vertical divider widget for UI separation.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateBrush.h"

/**
 * Visual styles for dividers based on context.
 */
UENUM()
enum class EDividerType : uint8
{
    /** Standard divider for general UI separation */
    General,
    /** Subtle divider for window controls and toolbars */
    WindowControl,
    /** Navigation divider for header navigation */
    HeaderNav
};

/**
 * Vertical divider widget for UI separation.
 */
class SVerticalDivider : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SVerticalDivider)
        : _DividerType(EDividerType::General), _Color(FLinearColor::Transparent), _Thickness(0.f), _Radius(0.f), _Margin(FMargin(4.f, 0.f)), _MinDesiredHeight(24.f)
    {
    }
    /** Type of divider style to use (General or WindowControl) */
    SLATE_ARGUMENT(EDividerType, DividerType)
    /** Optional override for divider color (defaults to style) */
    SLATE_ATTRIBUTE(FLinearColor, Color)
    /** Optional override for divider thickness (defaults to style) */
    SLATE_ATTRIBUTE(float, Thickness)
    /** Optional override for divider border radius (defaults to style) */
    SLATE_ATTRIBUTE(float, Radius)
    /** Margin around the divider */
    SLATE_ATTRIBUTE(FMargin, Margin)
    /** Optional minimum desired height for the divider */
    SLATE_ATTRIBUTE(float, MinDesiredHeight)
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

private:
    EDividerType DividerType;
    TAttribute<FLinearColor> Color;
    TAttribute<float> Thickness;
    TAttribute<float> Radius;
    TAttribute<FMargin> Margin;
    TAttribute<float> MinDesiredHeight;
};