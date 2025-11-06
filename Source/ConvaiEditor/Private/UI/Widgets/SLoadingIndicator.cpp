/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SLoadingIndicator.cpp
 *
 * Implementation of the loading indicator widget.
 */

#include "UI/Widgets/SLoadingIndicator.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/SOverlay.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/ConvaiStyleResources.h"
#include "Utility/ConvaiConstants.h"
#include "Rendering/DrawElements.h"
#include "Framework/Application/SlateApplication.h"

void SLoadingIndicator::Construct(const FArguments &InArgs)
{
    CurrentSize = InArgs._Size;
    CurrentStyle = InArgs._Style;
    MessageAttribute = InArgs._Message;
    bShowMessage = InArgs._ShowMessage;
    bShowOverlay = InArgs._ShowOverlay;
    bCenterContent = InArgs._CenterContent;

    LoadingWidget = CreateLoadingWidget();

    TSharedRef<SWidget> ContentWidget = SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().HAlign(bCenterContent ? HAlign_Center : HAlign_Left).VAlign(VAlign_Center)[SNew(SBox).WidthOverride(GetIndicatorSize()).HeightOverride(GetIndicatorSize())[LoadingWidget.ToSharedRef()]] + SVerticalBox::Slot().AutoHeight().HAlign(bCenterContent ? HAlign_Center : HAlign_Left).VAlign(VAlign_Center).Padding(FMargin(0.f, GetMessageSpacing(), 0.f, 0.f))[SAssignNew(MessageTextBlock, STextBlock).Text(MessageAttribute).Font(GetMessageFont()).ColorAndOpacity(FConvaiStyle::RequireColor(TEXT("Convai.Color.text.secondary"))).Justification(bCenterContent ? ETextJustify::Center : ETextJustify::Left).Visibility(bShowMessage && !MessageAttribute.Get().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed)];

    if (bShowOverlay)
    {
        const FLinearColor OverlayColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.surface.window"));
        auto BrushResult = FConvaiStyleResources::Get().GetOrCreateColorBrush(
            FName("LoadingIndicator.Overlay"),
            OverlayColor.CopyWithNewOpacity(0.92f));
        const FSlateBrush *BgBrush = BrushResult.IsSuccess() ? BrushResult.GetValue().Get() : FConvaiStyle::GetTransparentBrush();

        ChildSlot
            [SNew(SBorder)
                 .BorderImage(BgBrush)
                 .HAlign(HAlign_Center)
                 .VAlign(VAlign_Center)
                 .Padding(ConvaiEditor::Constants::Layout::Spacing::Window)
                     [ContentWidget]];
    }
    else
    {
        ChildSlot
            [ContentWidget];
    }
}

void SLoadingIndicator::SetMessage(const FText &NewMessage)
{
    MessageAttribute = NewMessage;
    if (MessageTextBlock.IsValid())
    {
        MessageTextBlock->SetText(NewMessage);
        MessageTextBlock->SetVisibility(
            bShowMessage && !NewMessage.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed);
    }
}

void SLoadingIndicator::SetSize(ELoadingIndicatorSize NewSize)
{
    if (CurrentSize != NewSize)
    {
        CurrentSize = NewSize;
        // Trigger a rebuild by invalidating the widget
        Invalidate(EInvalidateWidget::Layout);
    }
}

void SLoadingIndicator::SetStyle(ELoadingIndicatorStyle NewStyle)
{
    if (CurrentStyle != NewStyle)
    {
        CurrentStyle = NewStyle;

        LoadingWidget = CreateLoadingWidget();

        Invalidate(EInvalidateWidget::LayoutAndVolatility);

        ChildSlot.DetachWidget();
        Construct(FArguments()
                      .Size(CurrentSize)
                      .Style(CurrentStyle)
                      .Message(MessageAttribute)
                      .ShowMessage(bShowMessage)
                      .ShowOverlay(bShowOverlay)
                      .CenterContent(bCenterContent));
    }
}

TSharedRef<SWidget> SLoadingIndicator::CreateLoadingWidget()
{
    switch (CurrentStyle)
    {
    case ELoadingIndicatorStyle::BouncingDots:
        return CreateBouncingDotsWidget();
    case ELoadingIndicatorStyle::Pulse:
        return CreatePulseWidget();
    case ELoadingIndicatorStyle::CircularProgress:
        return CreateCircularProgressWidget();
    case ELoadingIndicatorStyle::Wave:
        return CreateWaveWidget();
    case ELoadingIndicatorStyle::BrandSpinner:
        return CreateBrandSpinnerWidget();
    case ELoadingIndicatorStyle::Spinner:
    default:
        return CreateSpinnerWidget();
    }
}

TSharedRef<SWidget> SLoadingIndicator::CreateSpinnerWidget()
{
    // Use Slate's built-in throbber with custom styling
    return SNew(SThrobber)
        .Animate(SThrobber::EAnimation::Opacity)
        .NumPieces(8);
}

TSharedRef<SWidget> SLoadingIndicator::CreateBouncingDotsWidget()
{
    const FLinearColor BrandColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.loadingIndicator.spinner"));

    return SNew(SBouncingDotsWidget)
        .Size(GetIndicatorSize())
        .Color(BrandColor);
}

TSharedRef<SWidget> SLoadingIndicator::CreatePulseWidget()
{
    // Pulse uses a circular throbber with different animation
    return SNew(SThrobber)
        .Animate(SThrobber::EAnimation::VerticalAndOpacity)
        .NumPieces(1);
}

TSharedRef<SWidget> SLoadingIndicator::CreateCircularProgressWidget()
{
    const FLinearColor BrandColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.loadingIndicator.spinner"));

    return SNew(SCircularProgressWidget)
        .Size(GetIndicatorSize())
        .Color(BrandColor);
}

TSharedRef<SWidget> SLoadingIndicator::CreateWaveWidget()
{
    const FLinearColor BrandColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.loadingIndicator.spinner"));

    return SNew(SWaveLoadingWidget)
        .Size(GetIndicatorSize())
        .Color(BrandColor);
}

TSharedRef<SWidget> SLoadingIndicator::CreateBrandSpinnerWidget()
{
    const FLinearColor BrandColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.action.hover"));

    return SNew(SCircularProgressWidget)
        .Size(GetIndicatorSize())
        .Color(BrandColor);
}

float SLoadingIndicator::GetIndicatorSize() const
{
    switch (CurrentSize)
    {
    case ELoadingIndicatorSize::Small:
        return 16.0f;
    case ELoadingIndicatorSize::Medium:
        return 32.0f;
    case ELoadingIndicatorSize::Large:
        return 48.0f;
    case ELoadingIndicatorSize::ExtraLarge:
        return 64.0f;
    default:
        return 32.0f;
    }
}

FSlateFontInfo SLoadingIndicator::GetMessageFont() const
{
    int32 FontSize = 12;

    switch (CurrentSize)
    {
    case ELoadingIndicatorSize::Small:
        FontSize = 10;
        break;
    case ELoadingIndicatorSize::Medium:
        FontSize = 12;
        break;
    case ELoadingIndicatorSize::Large:
        FontSize = 14;
        break;
    case ELoadingIndicatorSize::ExtraLarge:
        FontSize = 16;
        break;
    }

    return FCoreStyle::GetDefaultFontStyle("Regular", FontSize);
}

float SLoadingIndicator::GetMessageSpacing() const
{
    switch (CurrentSize)
    {
    case ELoadingIndicatorSize::Small:
        return 6.0f;
    case ELoadingIndicatorSize::Medium:
        return 12.0f;
    case ELoadingIndicatorSize::Large:
        return 16.0f;
    case ELoadingIndicatorSize::ExtraLarge:
        return 20.0f;
    default:
        return 12.0f;
    }
}

int32 SCircularProgressWidget::OnPaint(const FPaintArgs &Args, const FGeometry &AllottedGeometry,
                                       const FSlateRect &MyCullingRect, FSlateWindowElementList &OutDrawElements,
                                       int32 LayerId, const FWidgetStyle &InWidgetStyle, bool bParentEnabled) const
{
    const float Radius = Size * 0.4f;
    const float Thickness = Size * 0.08f;
    const FVector2D Center = AllottedGeometry.GetLocalSize() * 0.5f;

    TArray<FVector2D> BackgroundPoints;
    const int32 NumSegments = 64;
    for (int32 i = 0; i <= NumSegments; ++i)
    {
        const float Angle = (i / (float)NumSegments) * 2.0f * PI;
        BackgroundPoints.Add(Center + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius);
    }

    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(),
        BackgroundPoints,
        ESlateDrawEffect::None,
        Color.CopyWithNewOpacity(0.2f),
        true,
        Thickness);

    const float Progress = FMath::Fmod(AnimationTime * 1.5f, 1.0f);
    const float StartAngle = Progress * 2.0f * PI;
    const float ArcLength = PI * 0.75f;

    TArray<FVector2D> ArcPoints;
    const int32 ArcSegments = 48;
    for (int32 i = 0; i <= ArcSegments; ++i)
    {
        const float Angle = StartAngle + (i / (float)ArcSegments) * ArcLength;
        ArcPoints.Add(Center + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius);
    }

    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId + 1,
        AllottedGeometry.ToPaintGeometry(),
        ArcPoints,
        ESlateDrawEffect::None,
        Color,
        true,
        Thickness * 1.5f);

    return LayerId + 2;
}

int32 SWaveLoadingWidget::OnPaint(const FPaintArgs &Args, const FGeometry &AllottedGeometry,
                                  const FSlateRect &MyCullingRect, FSlateWindowElementList &OutDrawElements,
                                  int32 LayerId, const FWidgetStyle &InWidgetStyle, bool bParentEnabled) const
{
    const int32 NumBars = 5;
    const float BarWidth = Size / (NumBars * 2.0f);
    const float Spacing = BarWidth * 0.5f;
    const float MaxHeight = Size;

    for (int32 i = 0; i < NumBars; ++i)
    {
        const float Phase = (AnimationTime * 3.0f) + (i * 0.2f);
        const float Height = (FMath::Sin(Phase) * 0.5f + 0.5f) * MaxHeight * 0.8f + MaxHeight * 0.2f;

        const float X = i * (BarWidth + Spacing) + Spacing;
        const float Y = (MaxHeight - Height) * 0.5f;

        FSlateDrawElement::MakeBox(
            OutDrawElements,
            LayerId + i,
            AllottedGeometry.ToPaintGeometry(FVector2D(X, Y), FVector2D(BarWidth, Height)),
            FCoreStyle::Get().GetBrush("WhiteBrush"),
            ESlateDrawEffect::None,
            Color);
    }

    return LayerId + NumBars;
}

int32 SBouncingDotsWidget::OnPaint(const FPaintArgs &Args, const FGeometry &AllottedGeometry,
                                   const FSlateRect &MyCullingRect, FSlateWindowElementList &OutDrawElements,
                                   int32 LayerId, const FWidgetStyle &InWidgetStyle, bool bParentEnabled) const
{
    const int32 NumDots = 3;
    const float DotRadius = Size * 0.12f;
    const float Spacing = Size * 0.25f;
    const float CenterY = Size * 0.5f;
    const float StartX = (Size - (NumDots - 1) * Spacing) * 0.5f;

    for (int32 i = 0; i < NumDots; ++i)
    {
        const float Phase = (AnimationTime * 4.0f) + (i * 0.3f);
        const float Bounce = FMath::Abs(FMath::Sin(Phase)) * Size * 0.2f;

        const float X = StartX + i * Spacing;
        const float Y = CenterY - Bounce;

        TArray<FVector2D> CirclePoints;
        const int32 Segments = 16;
        for (int32 j = 0; j <= Segments; ++j)
        {
            const float Angle = (j / (float)Segments) * 2.0f * PI;
            CirclePoints.Add(FVector2D(
                X + FMath::Cos(Angle) * DotRadius,
                Y + FMath::Sin(Angle) * DotRadius));
        }

        FSlateDrawElement::MakeLines(
            OutDrawElements,
            LayerId + i,
            AllottedGeometry.ToPaintGeometry(),
            CirclePoints,
            ESlateDrawEffect::None,
            Color,
            true,
            DotRadius * 0.5f);
    }

    return LayerId + NumDots;
}
