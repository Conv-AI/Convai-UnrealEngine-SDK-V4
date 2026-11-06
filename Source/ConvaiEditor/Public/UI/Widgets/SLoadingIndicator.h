/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SLoadingIndicator.h
 *
 * Loading indicator widget with multiple size and style variants.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/** Loading indicator size variants. */
UENUM()
enum class ELoadingIndicatorSize : uint8
{
    Small,
    Medium,
    Large,
    ExtraLarge
};

/** Loading indicator style variants. */
UENUM()
enum class ELoadingIndicatorStyle : uint8
{
    Spinner,
    BouncingDots,
    Pulse,
    CircularProgress,
    Wave,
    BrandSpinner
};

/** Loading indicator widget with multiple size and style variants. */
class CONVAIEDITOR_API SLoadingIndicator : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SLoadingIndicator)
        : _Size(ELoadingIndicatorSize::Medium), _Style(ELoadingIndicatorStyle::Spinner), _Message(FText::GetEmpty()), _ShowMessage(true), _ShowOverlay(false), _CenterContent(true)
    {
    }
    SLATE_ARGUMENT(ELoadingIndicatorSize, Size)
    SLATE_ARGUMENT(ELoadingIndicatorStyle, Style)
    SLATE_ATTRIBUTE(FText, Message)
    SLATE_ARGUMENT(bool, ShowMessage)
    SLATE_ARGUMENT(bool, ShowOverlay)
    SLATE_ARGUMENT(bool, CenterContent)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);

    void SetMessage(const FText &NewMessage);
    void SetSize(ELoadingIndicatorSize NewSize);
    void SetStyle(ELoadingIndicatorStyle NewStyle);

private:
    TSharedRef<SWidget> CreateLoadingWidget();
    TSharedRef<SWidget> CreateSpinnerWidget();
    TSharedRef<SWidget> CreateBouncingDotsWidget();
    TSharedRef<SWidget> CreatePulseWidget();
    TSharedRef<SWidget> CreateCircularProgressWidget();
    TSharedRef<SWidget> CreateWaveWidget();
    TSharedRef<SWidget> CreateBrandSpinnerWidget();
    float GetIndicatorSize() const;
    FSlateFontInfo GetMessageFont() const;
    float GetMessageSpacing() const;

    ELoadingIndicatorSize CurrentSize;
    ELoadingIndicatorStyle CurrentStyle;
    TAttribute<FText> MessageAttribute;
    bool bShowMessage;
    bool bShowOverlay;
    bool bCenterContent;
    TSharedPtr<STextBlock> MessageTextBlock;
    TSharedPtr<SWidget> LoadingWidget;
};

/** Base class for custom animated loading widgets with Slate paint API. */
class SAnimatedLoadingWidget : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SAnimatedLoadingWidget)
        : _Size(32.0f), _Color(FLinearColor::White)
    {
    }
    SLATE_ARGUMENT(float, Size)
    SLATE_ARGUMENT(FLinearColor, Color)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs)
    {
        Size = InArgs._Size;
        Color = InArgs._Color;
        AnimationTime = 0.0f;
    }

    virtual void Tick(const FGeometry &AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
    {
        SLeafWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
        AnimationTime += InDeltaTime;
    }

    virtual FVector2D ComputeDesiredSize(float) const override
    {
        return FVector2D(Size, Size);
    }

protected:
    float Size;
    FLinearColor Color;
    float AnimationTime;
};

/** Circular progress indicator with smooth animation. */
class SCircularProgressWidget : public SAnimatedLoadingWidget
{
public:
    virtual int32 OnPaint(const FPaintArgs &Args, const FGeometry &AllottedGeometry,
                          const FSlateRect &MyCullingRect, FSlateWindowElementList &OutDrawElements,
                          int32 LayerId, const FWidgetStyle &InWidgetStyle, bool bParentEnabled) const override;
};

/** Wave animation with multiple bars. */
class SWaveLoadingWidget : public SAnimatedLoadingWidget
{
public:
    virtual int32 OnPaint(const FPaintArgs &Args, const FGeometry &AllottedGeometry,
                          const FSlateRect &MyCullingRect, FSlateWindowElementList &OutDrawElements,
                          int32 LayerId, const FWidgetStyle &InWidgetStyle, bool bParentEnabled) const override;
};

/** Bouncing dots animation. */
class SBouncingDotsWidget : public SAnimatedLoadingWidget
{
public:
    virtual int32 OnPaint(const FPaintArgs &Args, const FGeometry &AllottedGeometry,
                          const FSlateRect &MyCullingRect, FSlateWindowElementList &OutDrawElements,
                          int32 LayerId, const FWidgetStyle &InWidgetStyle, bool bParentEnabled) const override;
};
