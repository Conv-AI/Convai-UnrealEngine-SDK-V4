/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SVerticalDivider.cpp
 *
 * Implementation of the vertical divider widget.
 */

#include "UI/Components/SVerticalDivider.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"

void SVerticalDivider::Construct(const FArguments &InArgs)
{
    DividerType = InArgs._DividerType;
    const ISlateStyle &Style = FConvaiStyle::Get();

    FString ColorKey, BrushKey;
    switch (DividerType)
    {
    case EDividerType::WindowControl:
        ColorKey = TEXT("Convai.Color.divider.windowControl");
        BrushKey = TEXT("Convai.ColorBrush.divider.windowControl");
        break;
    case EDividerType::HeaderNav:
        ColorKey = TEXT("Convai.Color.divider.headerNav");
        BrushKey = TEXT("Convai.ColorBrush.divider.headerNav");
        break;
    case EDividerType::General:
    default:
        ColorKey = TEXT("Convai.Color.divider.general");
        BrushKey = TEXT("Convai.ColorBrush.divider.general");
        break;
    }

    Color = (InArgs._Color.IsSet() && InArgs._Color.Get() != FLinearColor::Transparent)
                ? InArgs._Color
                : Style.GetColor(FName(*ColorKey));
    Thickness = (InArgs._Thickness.IsSet() && InArgs._Thickness.Get() > 0.f)
                    ? InArgs._Thickness
                    : Style.GetFloat("Convai.Size.separatorThickness");
    Radius = (InArgs._Radius.IsSet() && InArgs._Radius.Get() > 0.f)
                 ? InArgs._Radius
                 : Style.GetFloat("Convai.Radius.separator");
    Margin = InArgs._Margin;
    MinDesiredHeight = InArgs._MinDesiredHeight;

    ChildSlot
        .Padding(Margin)
            [SNew(SBox)
                 .WidthOverride(Thickness.Get())
                 .VAlign(VAlign_Fill)
                     [SNew(SBorder)
                          .BorderImage(Style.GetBrush(FName(*BrushKey)))
                          .BorderBackgroundColor(Color.Get())
                          .Padding(FMargin(0))
                          .VAlign(VAlign_Fill)
                              [SNew(SBox)
                                   .VAlign(VAlign_Fill)
                                   .HAlign(HAlign_Fill)
                                   .MinDesiredHeight(MinDesiredHeight.Get())]]];
}