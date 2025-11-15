/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SDevInfoBox.cpp
 *
 * Implementation of the development info box widget.
 */

#include "UI/Components/SDevInfoBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Styling/ConvaiStyle.h"
#include "Utility/ConvaiConstants.h"
#include "SlateOptMacros.h"

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION

void SDevInfoBox::Construct(const FArguments &InArgs)
{
    const ISlateStyle &Style = FConvaiStyle::Get();

    ChildSlot
        [SNew(SBorder)
             .Padding(FMargin(16, 12))
             .BorderImage(FConvaiStyle::GetDevInfoBoxBrush())
                 [SNew(SHorizontalBox)

                  + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        .Padding(0, 0, 8, 0)
                            [SNew(STextBlock)
                                 .Text(FText::FromString(InArgs._Emoji))
                                 .Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))]

                  + SHorizontalBox::Slot()
                        .FillWidth(1.0f)
                        .VAlign(VAlign_Center)
                            [SNew(STextBlock)
                                 .Text(InArgs._InfoText)
                                 .Font(Style.GetFontStyle("Convai.Font.infoBox"))
                                 .ColorAndOpacity(Style.GetColor("Convai.Color.text.info"))
                                 .AutoWrapText(InArgs._bWrapText)
                                 .WrapTextAt(InArgs._WrapTextAt)
                                 .Justification(ETextJustify::Center)]]];
}

END_SLATE_FUNCTION_BUILD_OPTIMIZATION
