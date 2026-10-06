/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SRoundedBox.cpp
 *
 * Implementation of the rounded box widget.
 */

#include "UI/Widgets/SRoundedBox.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Brushes/SlateRoundedBoxBrush.h"

void SRoundedBox::Construct(const FArguments &InArgs)
{
    CachedBoxBrush = MakeShared<FSlateRoundedBoxBrush>(
        InArgs._BackgroundColor.Get(),
        InArgs._BorderRadius.Get(),
        InArgs._BorderColor.Get(),
        InArgs._BorderThickness.Get());

    ChildSlot
        [SNew(SBorder)
             .BorderImage(CachedBoxBrush.Get())
             .Padding(0)
                 [SNew(SBox)
                      .MinDesiredWidth(InArgs._MinDesiredWidth.Get())
                      .MinDesiredHeight(InArgs._MinDesiredHeight.Get())
                      .HAlign(InArgs._HAlign.Get())
                      .VAlign(InArgs._VAlign.Get())
                      .Padding(InArgs._ContentPadding.Get())
                          [InArgs._Content.Widget]]];
}
