/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiScrollBox.cpp
 *
 * Implementation of the custom scroll box widget.
 */

#include "UI/Widgets/SConvaiScrollBox.h"
#include "Styling/ConvaiStyle.h"
#include "Utility/ConvaiConstants.h"

void SConvaiScrollBox::Construct(const FArguments &InArgs)
{
    float ScrollBarThickness = ConvaiEditor::Constants::Layout::Components::ScrollBar::Thickness;
    const float ScrollBarVerticalPaddingAmount = ConvaiEditor::Constants::Layout::Spacing::ScrollBarVerticalPadding;

    const FVector2D ScrollBarTrackSize(ScrollBarThickness, ScrollBarThickness);

    FMargin ScrollBarWidgetPadding;
    if (InArgs._CustomScrollBarPadding.IsSet())
    {
        ScrollBarWidgetPadding = InArgs._CustomScrollBarPadding.GetValue();
    }
    else
    {
        ScrollBarWidgetPadding = FMargin(0.f, ScrollBarVerticalPaddingAmount);
    }

    SScrollBox::Construct(
        SScrollBox::FArguments()
            .Style(&FConvaiStyle::GetScrollBoxStyle(InArgs._ShowShadow))
            .ScrollBarAlwaysVisible(InArgs._ScrollBarAlwaysVisible)
            .ScrollBarStyle(&FConvaiStyle::GetScrollBarStyle())
            .ScrollBarThickness(ScrollBarTrackSize)
            .ScrollBarPadding(ScrollBarWidgetPadding)
            .Orientation(InArgs._Orientation)
            .ScrollBarVisibility(InArgs._ScrollBarAlwaysVisible ? EVisibility::Visible : EVisibility::Visible)
            .ExternalScrollbar(TSharedPtr<SScrollBar>())
            .ConsumeMouseWheel(EConsumeMouseWheel::Always)
            .AllowOverscroll(EAllowOverscroll::No)
            .AnimateWheelScrolling(true)
            .WheelScrollMultiplier(1.0f)
            .NavigationDestination(EDescendantScrollDestination::IntoView)
            .NavigationScrollPadding(0.0f));
}
