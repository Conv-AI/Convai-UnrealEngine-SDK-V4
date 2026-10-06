/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiLoadingScreen.cpp
 *
 * Implementation of the loading screen widget.
 */

#include "UI/Widgets/SConvaiLoadingScreen.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/ConvaiStyleResources.h"
#include "Utility/ConvaiConstants.h"

void SConvaiLoadingScreen::Construct(const FArguments &InArgs)
{
    const FLinearColor OverlayColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.surface.window"));
    auto BrushResult = FConvaiStyleResources::Get().GetOrCreateColorBrush(FName("LoadingScreen.Bg"), OverlayColor.CopyWithNewOpacity(0.85f));
    const FSlateBrush *BgBrush = BrushResult.IsSuccess() ? BrushResult.GetValue().Get() : FConvaiStyle::GetTransparentBrush();

    const FSlateFontInfo Font = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountSectionTitle"));

    ChildSlot
        [SNew(SBorder)
             .BorderImage(BgBrush)
             .Padding(ConvaiEditor::Constants::Layout::Spacing::Window)
                 [SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)[SNew(SThrobber)] + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 16.f, 0.f, 0.f)).HAlign(HAlign_Center)[SNew(STextBlock).Text(InArgs._Message.Get(FText::FromString("Loading..."))).Font(Font).ColorAndOpacity(FConvaiStyle::RequireColor(TEXT("Convai.Color.text.primary"))).Justification(ETextJustify::Center)]]];
}