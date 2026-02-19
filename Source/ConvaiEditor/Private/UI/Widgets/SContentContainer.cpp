/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SContentContainer.cpp
 *
 * Implementation of the content container widget.
 */

#include "UI/Widgets/SContentContainer.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Styling/SlateTypes.h"
#include "Styling/CoreStyle.h"

void SContentContainer::Construct(const FArguments &InArgs)
{
    const FTextBlockStyle *TitleStyle = InArgs._TitleTextStyle;
    if (!TitleStyle)
    {
        static FTextBlockStyle HeaderTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
        HeaderTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 24));
        HeaderTextStyle.SetColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.primary"));
        TitleStyle = &HeaderTextStyle;
    }

    ChildSlot
        [SNew(SVerticalBox)

         + SVerticalBox::Slot()
               .AutoHeight()
               .Padding(InArgs._TitlePadding.Get())
               .HAlign(HAlign_Left)
                   [SNew(STextBlock)
                        .Text(InArgs._Title)
                        .TextStyle(TitleStyle)
                        .Visibility(InArgs._Title.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)]

         + SVerticalBox::Slot()
               .FillHeight(1.0f)
                   [SNew(SRoundedBox)
                        .BorderRadius(InArgs._BorderRadius)
                        .BackgroundColor(InArgs._BackgroundColor)
                        .BorderColor(InArgs._BorderColor)
                        .BorderThickness(InArgs._BorderThickness)
                        .ContentPadding(InArgs._ContentPadding)
                        .MinDesiredWidth(InArgs._MinWidth)
                        .MinDesiredHeight(InArgs._MinHeight)
                            [InArgs._Content.Widget]]];
}
