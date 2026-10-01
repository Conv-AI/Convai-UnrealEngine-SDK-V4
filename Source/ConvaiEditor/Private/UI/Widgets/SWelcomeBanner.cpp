/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SWelcomeBanner.cpp
 *
 * Implementation of the welcome banner widget.
 */

#include "UI/Widgets/SWelcomeBanner.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Images/SImage.h"
#include "Styling/SlateBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "UI/Utility/ConvaiWidgetFactory.h"

void SWelcomeBanner::Construct(const FArguments &InArgs)
{
    BannerBrush = FConvaiStyle::Get().GetBrush("Welcome.WelcomeBanner");

    ChildSlot
        [SNew(SOverlay) + SOverlay::Slot()
                              [SNew(SImage)
                                   .Image(BannerBrush)
                                   .ColorAndOpacity(FLinearColor::White)]];
}