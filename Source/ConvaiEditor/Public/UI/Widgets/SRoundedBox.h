/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SRoundedBox.h
 *
 * Flexible rounded box container widget for consistent visual styling.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/ConvaiStyle.h"

/** Flexible rounded box container widget for consistent visual styling. */
class CONVAIEDITOR_API SRoundedBox : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SRoundedBox)
        : _BorderRadius(8.0f), _BackgroundColor(FLinearColor(0.1f, 0.1f, 0.1f, 1.0f)), _BorderColor(FLinearColor(0.2f, 0.2f, 0.2f, 1.0f)), _BorderThickness(1.0f), _ContentPadding(FMargin(12.0f)), _UseOutlineBrush(false), _MinDesiredWidth(0.0f), _MinDesiredHeight(0.0f), _HAlign(HAlign_Fill), _VAlign(VAlign_Fill)
    {
    }
    SLATE_ATTRIBUTE(float, BorderRadius)
    SLATE_ATTRIBUTE(FLinearColor, BackgroundColor)
    SLATE_ATTRIBUTE(FLinearColor, BorderColor)
    SLATE_ATTRIBUTE(float, BorderThickness)
    SLATE_ATTRIBUTE(FMargin, ContentPadding)
    SLATE_ATTRIBUTE(bool, UseOutlineBrush)
    SLATE_ATTRIBUTE(float, MinDesiredWidth)
    SLATE_ATTRIBUTE(float, MinDesiredHeight)
    SLATE_ATTRIBUTE(EHorizontalAlignment, HAlign)
    SLATE_ATTRIBUTE(EVerticalAlignment, VAlign)
    SLATE_DEFAULT_SLOT(FArguments, Content)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);

private:
    mutable TSharedPtr<FSlateBrush> CachedBoxBrush;
};
