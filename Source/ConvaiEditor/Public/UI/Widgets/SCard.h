/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SCard.h
 *
 * Unified card component with material-based rounded corners.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Slate/SRetainerWidget.h"

class SButton;
class SOverlay;
class SBorder;
class STextBlock;
class SImage;
class SWrapBox;
struct FSlateBrush;
class UMaterialInterface;
class UMaterialInstanceDynamic;
struct FSampleItem;
struct FDisplayMetrics;
class FHomePageViewModel;

enum class ECardDisplayMode : uint8
{
    Default,
    HomepageSimple,
    SamplesWithTags,
    Custom
};

/** Unified card component with material-based rounded corners. */
class CONVAIEDITOR_API SCard : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCard)
        : _BorderRadius(8.0f), _BorderThickness(2.0f), _BackgroundColor(FLinearColor(0.1f, 0.1f, 0.1f, 1.0f)), _BorderColor(FLinearColor(0.2f, 0.2f, 0.2f, 1.0f)), _ContentPadding(FMargin(8.0f)), _SampleItem(nullptr), _DisplayMode(ECardDisplayMode::Default), _CustomTitleFontSize(24.0f), _bShowTags(true), _bCenterTitle(false)
    {
    }
    SLATE_ATTRIBUTE(float, BorderRadius)
    SLATE_ATTRIBUTE(float, BorderThickness)
    SLATE_ATTRIBUTE(FLinearColor, BackgroundColor)
    SLATE_ATTRIBUTE(FLinearColor, BorderColor)
    SLATE_ATTRIBUTE(FMargin, ContentPadding)
    SLATE_ARGUMENT(TSharedPtr<FSampleItem>, SampleItem)
    SLATE_ATTRIBUTE(ECardDisplayMode, DisplayMode)
    SLATE_ATTRIBUTE(float, CustomTitleFontSize)
    SLATE_ATTRIBUTE(bool, bShowTags)
    SLATE_ATTRIBUTE(bool, bCenterTitle)
    SLATE_ATTRIBUTE(const FSlateBrush *, DynamicImageBrush)
    SLATE_EVENT(FOnClicked, OnClicked)
    SLATE_DEFAULT_SLOT(FArguments, Content)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);
    virtual ~SCard();

    virtual void OnArrangeChildren(const FGeometry &AllottedGeometry, FArrangedChildren &ArrangedChildren) const override;
    virtual void Tick(const FGeometry &AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

protected:
    UMaterialInstanceDynamic *CreateRoundedMaskMID(float CornerRadius);
    void HandleDisplayMetricsChanged(const FDisplayMetrics &NewMetrics);
    void UpdateMaterialParameters(const FVector2D &WidgetSize);
    TSharedRef<SWidget> BuildSampleOverlay(const TSharedPtr<FSampleItem> &InSampleItem) const;
    TSharedRef<SWidget> BuildSimpleOverlay(const TSharedPtr<FSampleItem> &InSampleItem) const;
    TSharedRef<SWidget> CreateTagWidget(const FString &TagText) const;
    TSharedRef<SWidget> BuildTagRow(const TSharedPtr<FSampleItem> &InSampleItem) const;
    const FSlateBrush *GetSampleImageBrush(const TSharedPtr<FSampleItem> &InSampleItem) const;
    const FSlateBrush *GetGradientBrush() const;
    const FSlateBrush *MakeOutlineBrush(const FLinearColor &InColor, float InRadius, float InThickness) const;

protected:
    TSharedPtr<SRetainerWidget> RetainerWidget;
    TObjectPtr<UMaterialInterface> RoundedMaskMat = nullptr;
    TObjectPtr<UMaterialInstanceDynamic> MaskMID = nullptr;
    float BorderRadiusPx = 8.0f;
    float BorderThicknessPx = 2.0f;
    FMargin ContentPaddingMargin;
    FVector2D CardFixedSize = FVector2D::ZeroVector;
    FDelegateHandle MetricsHandle;
    mutable TSharedPtr<FSlateBrush> SampleImageBrush;
    mutable TSharedPtr<FSlateBrush> GradientBrush;
    FVector2D LastKnownInnerSize = FVector2D::ZeroVector;
    bool bMaterialParametersInitialized = false;
    ECardDisplayMode EffectiveDisplayMode = ECardDisplayMode::Default;
    float CustomTitleFontSize = 24.0f;
    bool bShouldShowTags = true;
    bool bShouldCenterTitle = false;
    TAttribute<const FSlateBrush *> DynamicImageBrushAttribute;
};
