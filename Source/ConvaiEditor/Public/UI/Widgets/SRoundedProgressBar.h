/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SRoundedProgressBar.h
 *
 * Progress bar widget with rounded corners using nested SRoundedBox widgets.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/SlateTypes.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/SBoxPanel.h"

class SRoundedBox;

/** Progress bar widget with rounded corners using nested SRoundedBox widgets. */
class CONVAIEDITOR_API SRoundedProgressBar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SRoundedProgressBar)
        : _BarHeight(8.0f), _BorderRadius(4.0f), _Percent(0.0f), _BackgroundColor(FLinearColor(0.1f, 0.1f, 0.1f, 1.0f)), _FillColor(FLinearColor(0.0f, 1.0f, 0.0f, 1.0f)), _FillColorAttribute(), _BackgroundColorAttribute()
    {
    }
    SLATE_ATTRIBUTE(float, BarHeight)
    SLATE_ATTRIBUTE(float, BorderRadius)
    SLATE_ATTRIBUTE(float, Percent)
    SLATE_ATTRIBUTE(FLinearColor, BackgroundColor)
    SLATE_ATTRIBUTE(FLinearColor, FillColor)
    SLATE_ATTRIBUTE(TOptional<FLinearColor>, FillColorAttribute)
    SLATE_ATTRIBUTE(TOptional<FLinearColor>, BackgroundColorAttribute)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);

    void SetPercent(TAttribute<float> InPercent);
    float GetPercent() const;
    FLinearColor GetBackgroundColor() const;
    FLinearColor GetFillColor() const;
    float GetFillWidth() const;
    virtual FVector2D ComputeDesiredSize(float) const override;

private:
    TAttribute<float> PercentAttribute;
    TAttribute<float> BarHeightAttribute;
    TAttribute<float> BorderRadiusAttribute;
    TAttribute<FLinearColor> BackgroundColorAttribute;
    TAttribute<FLinearColor> FillColorAttribute;
    TAttribute<TOptional<FLinearColor>> FillColorOverrideAttribute;
    TAttribute<TOptional<FLinearColor>> BackgroundColorOverrideAttribute;
    TSharedPtr<SRoundedBox> BackgroundBox;
    TSharedPtr<SRoundedBox> FillBox;
};
