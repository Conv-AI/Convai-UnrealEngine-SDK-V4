/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SContentCard.cpp
 *
 * Implementation of the content card widget.
 */

#include "UI/Widgets/SContentCard.h"
#include "UI/Widgets/SCard.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Styling/CoreStyle.h"
#include "Styling/ConvaiStyle.h"
#include "Utility/ConvaiConstants.h"

void SContentCard::Construct(const FArguments &InArgs)
{
    TSharedRef<SWidget> CardContent = BuildCardContent(InArgs);

    const float BorderRadius = ConvaiEditor::Constants::Layout::Radius::StandardCard;
    const FLinearColor BgColor = InArgs._BackgroundColor.IsSet() ? InArgs._BackgroundColor.Get() : FConvaiStyle::RequireColor(FName("Convai.Color.surface.content"));
    const FLinearColor BorderColor = InArgs._BorderColor.IsSet() ? InArgs._BorderColor.Get() : FConvaiStyle::RequireColor(FName("Convai.Color.component.standardCard.outline"));
    const float BorderThickness = ConvaiEditor::Constants::Layout::Components::StandardCard::BorderThickness;
    const FMargin ContentPadding = InArgs._ContentPadding.IsSet() ? InArgs._ContentPadding.Get() : FMargin(12.0f);

    SCard::Construct(
        SCard::FArguments()
            .BorderRadius(BorderRadius)
            .BorderThickness(BorderThickness)
            .BackgroundColor(BgColor)
            .BorderColor(BorderColor)
            .ContentPadding(ContentPadding)
            .Content()
                [CardContent]);
}

TSharedRef<SWidget> SContentCard::BuildCardContent(const FArguments &InArgs)
{
    return SNew(SButton)
        .OnClicked(InArgs._OnClicked)
        .ButtonStyle(FCoreStyle::Get(), "NoBorder")
        .ContentPadding(0)
        .Content()
            [SNew(SHorizontalBox)

             + SHorizontalBox::Slot()
                   .AutoWidth()
                   .VAlign(VAlign_Center)
                   .Padding(0, 0, 8, 0)
                       [SNew(SBox)
                            .WidthOverride(InArgs._ImageSize.X)
                            .HeightOverride(InArgs._ImageSize.Y)
                            .Visibility(InArgs._ContentImage != nullptr ? EVisibility::Visible : EVisibility::Collapsed)
                                [SNew(SScaleBox)
                                     .Stretch(EStretch::ScaleToFit)
                                         [SNew(SImage)
                                              .Image(InArgs._ContentImage)]]]

             + SHorizontalBox::Slot()
                   .FillWidth(1.0f)
                   .VAlign(VAlign_Center)
                       [SNew(SVerticalBox)

                        + SVerticalBox::Slot()
                              .AutoHeight()
                              .Padding(InArgs._TitlePadding)
                                  [SNew(STextBlock)
                                       .Text(InArgs._Title)
                                       .TextStyle(FCoreStyle::Get(), "NormalText")
                                       .AutoWrapText(true)]

                        + SVerticalBox::Slot()
                              .AutoHeight()
                                  [SNew(STextBlock)
                                       .Text(InArgs._Description)
                                       .TextStyle(FCoreStyle::Get(), "SmallText")
                                       .AutoWrapText(true)]]];
}
