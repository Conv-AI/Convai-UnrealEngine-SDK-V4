/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SRoundedProgressBar.cpp
 *
 * Implementation of the rounded progress bar widget.
 */

#include "UI/Widgets/SRoundedProgressBar.h"
#include "Styling/ConvaiStyle.h"
#include "UI/Widgets/SRoundedBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"
#include "Layout/WidgetPath.h"

void SRoundedProgressBar::Construct(const FArguments &InArgs)
{
    PercentAttribute = InArgs._Percent;
    BarHeightAttribute = InArgs._BarHeight;
    BorderRadiusAttribute = InArgs._BorderRadius;
    BackgroundColorAttribute = InArgs._BackgroundColor;
    FillColorAttribute = InArgs._FillColor;
    FillColorOverrideAttribute = InArgs._FillColorAttribute;
    BackgroundColorOverrideAttribute = InArgs._BackgroundColorAttribute;

    ChildSlot
        [SNew(SBox)
             .HeightOverride_Lambda([this]() -> FOptionalSize
                                    { return FOptionalSize(BarHeightAttribute.Get()); })
                 [SNew(SOverlay)

                  + SOverlay::Slot()
                        .HAlign(HAlign_Fill)
                        .VAlign(VAlign_Fill)
                            [SAssignNew(BackgroundBox, SRoundedBox)
                                 .BorderRadius(BorderRadiusAttribute)
                                 .BackgroundColor(this, &SRoundedProgressBar::GetBackgroundColor)
                                 .BorderColor(FLinearColor::Transparent)
                                 .BorderThickness(0)]

                  + SOverlay::Slot()
                        .HAlign(HAlign_Left)
                        .VAlign(VAlign_Fill)
                            [SNew(SBox)
                                 .WidthOverride_Lambda([this]() -> FOptionalSize
                                                       { return FOptionalSize(GetFillWidth()); })
                                     [SAssignNew(FillBox, SRoundedBox)
                                          .BorderRadius(BorderRadiusAttribute)
                                          .BackgroundColor(this, &SRoundedProgressBar::GetFillColor)
                                          .BorderColor(FLinearColor::Transparent)
                                          .BorderThickness(0)]]]];
}

void SRoundedProgressBar::SetPercent(TAttribute<float> InPercent)
{
    PercentAttribute = InPercent;
    Invalidate(EInvalidateWidgetReason::Layout);
}

float SRoundedProgressBar::GetPercent() const
{
    return PercentAttribute.Get();
}

FLinearColor SRoundedProgressBar::GetBackgroundColor() const
{
    FLinearColor BackgroundColor = BackgroundColorAttribute.Get();
    if (BackgroundColorOverrideAttribute.IsSet() && BackgroundColorOverrideAttribute.Get().IsSet())
    {
        BackgroundColor = BackgroundColorOverrideAttribute.Get().GetValue();
    }
    return BackgroundColor;
}

FLinearColor SRoundedProgressBar::GetFillColor() const
{
    FLinearColor FillColor = FillColorAttribute.Get();
    if (FillColorOverrideAttribute.IsSet() && FillColorOverrideAttribute.Get().IsSet())
    {
        FillColor = FillColorOverrideAttribute.Get().GetValue();
    }
    return FillColor;
}

float SRoundedProgressBar::GetFillWidth() const
{
    if (!BackgroundBox.IsValid())
        return 0.0f;

    const float ParentWidth = BackgroundBox->GetTickSpaceGeometry().GetLocalSize().X;

    if (ParentWidth <= 0.0f)
        return 100.0f;

    const float Percent = FMath::Clamp(PercentAttribute.Get(), 0.0f, 1.0f);

    const float BorderRadius = BorderRadiusAttribute.Get();
    return FMath::Max<float>(Percent * ParentWidth, BorderRadius * 2.0f);
}

FVector2D SRoundedProgressBar::ComputeDesiredSize(float) const
{
    return FVector2D(100.0f, BarHeightAttribute.Get());
}
