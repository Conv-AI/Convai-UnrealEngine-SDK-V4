/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SAuthStatusOverlay.cpp
 *
 * Implementation of the authentication status overlay widget.
 */

#include "UI/Widgets/SAuthStatusOverlay.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Images/SImage.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/ConvaiStyleResources.h"
#include "Utility/ConvaiConstants.h"

void SAuthStatusOverlay::Construct(const FArguments &InArgs)
{
    CurrentMessage = InArgs._Message;
    CurrentSubMessage = InArgs._SubMessage;

    const FLinearColor OverlayColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.surface.window"));
    auto BrushResult = FConvaiStyleResources::Get().GetOrCreateColorBrush(FName("AuthOverlay.Bg"), OverlayColor.CopyWithNewOpacity(1.0f));
    const FSlateBrush *BgBrush = BrushResult.IsSuccess() ? BrushResult.GetValue().Get() : FConvaiStyle::GetTransparentBrush();

    FSlateFontInfo TitleFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountSectionTitle"));
    FSlateFontInfo SubtitleFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountValue"));
    TitleFont.Size = static_cast<int32>(TitleFont.Size * 1.3f);
    SubtitleFont.Size = static_cast<int32>(SubtitleFont.Size * 1.1f);

    ChildSlot
        [SNew(SOverlay) + SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)[SNew(SBorder).BorderImage(BgBrush)] + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)[SNew(SImage).Image(FConvaiStyle::Get().GetBrush(TEXT("Convai.Logo"))).RenderTransform(FSlateRenderTransform(FScale2D(2.f))).RenderTransformPivot(FVector2D(0.5f, 0.5f))] + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 24.f, 0.f, 0.f)).HAlign(HAlign_Center)[SNew(SThrobber).RenderTransform(FSlateRenderTransform(FScale2D(1.5f))).RenderTransformPivot(FVector2D(0.5f, 0.5f))] + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 16.f, 0.f, 0.f)).HAlign(HAlign_Center)[SNew(STextBlock).Text_Lambda([this]()
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             { return CurrentMessage.Get(); })
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    .Font(TitleFont)
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    .ColorAndOpacity(FConvaiStyle::RequireColor(TEXT("Convai.Color.text.primary")))
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    .Justification(ETextJustify::Center)] +
                                                                                                                                                                                      SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f)).HAlign(HAlign_Center)[SNew(STextBlock).Text_Lambda([this]()
                                                                                                                                                                                                                                                                                                                { return CurrentSubMessage.Get(); })
                                                                                                                                                                                                                                                                                       .Font(SubtitleFont)
                                                                                                                                                                                                                                                                                       .ColorAndOpacity(FConvaiStyle::RequireColor(TEXT("Convai.Color.text.primary")))
                                                                                                                                                                                                                                                                                       .Justification(ETextJustify::Center)
                                                                                                                                                                                                                                                                                       .Visibility_Lambda([this]()
                                                                                                                                                                                                                                                                                                          { return CurrentSubMessage.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })]]];
}

void SAuthStatusOverlay::SetStatus(const FText &NewMessage, const FText &NewSubMessage)
{
    CurrentMessage = TAttribute<FText>(NewMessage);
    CurrentSubMessage = TAttribute<FText>(NewSubMessage);

    Construct(SAuthStatusOverlay::FArguments()
                  .Message(CurrentMessage)
                  .SubMessage(CurrentSubMessage));
}