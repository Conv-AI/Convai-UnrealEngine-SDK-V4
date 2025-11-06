/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SCard.cpp
 *
 * Implementation of the card widget.
 */

#include "UI/Widgets/SCard.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/ConvaiStyleResources.h"
#include "Utility/ConvaiConstants.h"
#include "Utility/ConvaiValidationUtils.h"
#include "MVVM/SamplesViewModel.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Styling/SlateStyleRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "Styling/CoreStyle.h"
#include "Engine/Texture2D.h"
#include "ConvaiEditor.h"

DEFINE_LOG_CATEGORY_STATIC(LogCard, Log, All);

void SCard::Construct(const FArguments &InArgs)
{
    ECardDisplayMode RequestedDisplayMode = InArgs._DisplayMode.Get();
    CustomTitleFontSize = InArgs._CustomTitleFontSize.Get();
    bShouldShowTags = InArgs._bShowTags.Get();
    bShouldCenterTitle = InArgs._bCenterTitle.Get();
    DynamicImageBrushAttribute = InArgs._DynamicImageBrush;

    if (InArgs._SampleItem.IsValid())
    {
        if (RequestedDisplayMode != ECardDisplayMode::Default)
        {
            EffectiveDisplayMode = RequestedDisplayMode;
        }
        else
        {
            EffectiveDisplayMode = ECardDisplayMode::SamplesWithTags;
        }
    }
    else
    {
        EffectiveDisplayMode = RequestedDisplayMode;
    }

    if (EffectiveDisplayMode == ECardDisplayMode::HomepageSimple)
    {
        bShouldShowTags = false;
        bShouldCenterTitle = true;
    }
    else if (EffectiveDisplayMode == ECardDisplayMode::SamplesWithTags)
    {
        bShouldShowTags = true;
        bShouldCenterTitle = false;
    }

    if (InArgs._SampleItem.IsValid())
    {
        if (EffectiveDisplayMode == ECardDisplayMode::HomepageSimple ||
            (EffectiveDisplayMode == ECardDisplayMode::Default && InArgs._SampleItem->Tags.Num() == 1))
        {
            BorderRadiusPx = ConvaiEditor::Constants::Layout::Radius::HomePageCard;
            BorderThicknessPx = ConvaiEditor::Constants::Layout::Components::HomePageCard::BorderThickness;
        }
        else
        {
            BorderRadiusPx = ConvaiEditor::Constants::Layout::Radius::SampleCard;
            BorderThicknessPx = ConvaiEditor::Constants::Layout::Components::SampleCard::BorderThickness;
        }
    }
    else
    {
        BorderRadiusPx = InArgs._BorderRadius.IsSet() ? InArgs._BorderRadius.Get() : ConvaiEditor::Constants::Layout::Radius::StandardCard;
        BorderThicknessPx = InArgs._BorderThickness.IsSet() ? InArgs._BorderThickness.Get() : ConvaiEditor::Constants::Layout::Components::StandardCard::BorderThickness;
    }

    ContentPaddingMargin = InArgs._ContentPadding.Get();
    MaskMID = CreateRoundedMaskMID(BorderRadiusPx);

    if (InArgs._SampleItem.IsValid())
    {
        bool bIsHomePageCard = InArgs._SampleItem->Tags.Num() == 1;
        const FVector2D CardSize = bIsHomePageCard ? ConvaiEditor::Constants::Layout::Components::HomePageCard::Dimensions : ConvaiEditor::Constants::Layout::Components::SampleCard::Dimensions;
        CardFixedSize = CardSize;
        const FSlateBrush *OutlineBrush = FConvaiStyle::GetSampleCardOutlineBrush();

        RetainerWidget = SNew(SRetainerWidget)
                             .Phase(0)
                             .PhaseCount(1)
                             .RenderOnPhase(false)
                             .RenderOnInvalidation(true);

        if (RetainerWidget.IsValid() && MaskMID)
        {
            RetainerWidget->SetEffectMaterial(MaskMID);
            RetainerWidget->SetTextureParameter(FName("UITexture"));
            MaskMID->SetScalarParameterValue(TEXT("UIScale"),
                                             FSlateApplication::Get().GetApplicationScale());
        }

        if (EffectiveDisplayMode == ECardDisplayMode::HomepageSimple)
        {
            RetainerWidget->SetContent(BuildSimpleOverlay(InArgs._SampleItem));
        }
        else
        {
            RetainerWidget->SetContent(BuildSampleOverlay(InArgs._SampleItem));
        }

        ChildSlot
            [SNew(SBox)
                 .WidthOverride(CardSize.X)
                 .HeightOverride(CardSize.Y)
                 .MaxDesiredWidth(CardSize.X)
                 .MaxDesiredHeight(CardSize.Y)
                 .Visibility(EVisibility::SelfHitTestInvisible)
                     [SNew(SOverlay) + SOverlay::Slot()
                                           .HAlign(HAlign_Center)
                                           .VAlign(VAlign_Center)
                                               [SNew(SBox)
                                                    .WidthOverride(CardSize.X)
                                                    .HeightOverride(CardSize.Y)
                                                    .MaxDesiredWidth(CardSize.X)
                                                    .MaxDesiredHeight(CardSize.Y)
                                                        [SNew(SButton)
                                                             .OnClicked(InArgs._OnClicked)
                                                             .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                                                             .ContentPadding(0)
                                                             .Content()
                                                                 [SNew(SBox)
                                                                      .WidthOverride(CardSize.X)
                                                                      .HeightOverride(CardSize.Y)
                                                                          [SNew(SBorder)
                                                                               .BorderImage(OutlineBrush)
                                                                               .Padding(FMargin(BorderThicknessPx))
                                                                               .Content()
                                                                                   [SNew(SBorder)
                                                                                        .BorderImage(FConvaiStyle::GetTransparentBrush())
                                                                                        .Padding(0)
                                                                                        .Content()
                                                                                            [RetainerWidget.ToSharedRef()]]]]]]]];

        if (RetainerWidget.IsValid() && MaskMID)
        {
            FVector2D InnerSize(CardSize.X - 2 * BorderThicknessPx, CardSize.Y - 2 * BorderThicknessPx);
            LastKnownInnerSize = InnerSize;
            UpdateMaterialParameters(InnerSize);
        }
    }
    else
    {
        TOptional<FLinearColor> CustomBorderColor;

        if (InArgs._BorderColor.IsSet())
        {
            FLinearColor AdjustedBorderColor = InArgs._BorderColor.Get();
            AdjustedBorderColor.A = 1.0f;
            CustomBorderColor = AdjustedBorderColor;
        }

        const FSlateBrush *OuterBorderBrush = FConvaiStyle::GetStandardCardOutlineBrush(CustomBorderColor);

        if (InArgs._Content.Widget != SNullWidget::NullWidget)
        {
            TSharedRef<SWidget> ContentWidget = InArgs._Content.Widget;

            RetainerWidget = SNew(SRetainerWidget)
                                 .Phase(0)
                                 .PhaseCount(1)
                                 .RenderOnPhase(false)
                                 .RenderOnInvalidation(true);

            if (RetainerWidget.IsValid() && MaskMID)
            {
                RetainerWidget->SetEffectMaterial(MaskMID);
                RetainerWidget->SetTextureParameter(FName("UITexture"));
                MaskMID->SetScalarParameterValue(
                    TEXT("UIScale"),
                    FSlateApplication::Get().GetApplicationScale());
            }

            RetainerWidget->SetContent(ContentWidget);

            ChildSlot
                [SNew(SBox)
                     .Visibility(EVisibility::SelfHitTestInvisible)
                         [SNew(SButton)
                              .OnClicked(InArgs._OnClicked)
                              .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                              .ContentPadding(0)
                              .Content()
                                  [SNew(SBorder)
                                       .BorderImage(OuterBorderBrush)
                                       .Padding(FMargin(BorderThicknessPx))
                                           [SNew(SBorder)
                                                .BorderImage(FConvaiStyle::GetTransparentBrush())
                                                .Padding(0)
                                                .Content()
                                                    [RetainerWidget.ToSharedRef()]]]]];
        }
        else
        {
            ChildSlot
                [SNew(SButton)
                     .OnClicked(InArgs._OnClicked)
                     .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                     .ContentPadding(0)
                     .Content()
                         [SNew(SBorder)
                              .BorderImage(OuterBorderBrush)
                              .Padding(FMargin(BorderThicknessPx))
                                  [SNew(SBorder)
                                       .BorderImage(FConvaiStyle::GetTransparentBrush())
                                       .Padding(0)
                                           [SNew(SBox)]]]];
        }
    }

    if (FSlateApplication::IsInitialized())
    {
        auto PlatformApp = FSlateApplication::Get().GetPlatformApplication();
        if (PlatformApp.IsValid())
        {
            MetricsHandle = PlatformApp->OnDisplayMetricsChanged().AddRaw(
                this, &SCard::HandleDisplayMetricsChanged);
        }
    }

    bMaterialParametersInitialized = false;
}

SCard::~SCard()
{
    if (FSlateApplication::IsInitialized())
    {
        auto PlatformApp = FSlateApplication::Get().GetPlatformApplication();
        if (PlatformApp.IsValid() && MetricsHandle.IsValid())
        {
            PlatformApp->OnDisplayMetricsChanged().Remove(MetricsHandle);
        }
    }
    MetricsHandle.Reset();
}

UMaterialInstanceDynamic *SCard::CreateRoundedMaskMID(float InOuterCornerRadius)
{
    RoundedMaskMat = FConvaiValidationUtils::LoadMaterialInterface(
        ConvaiEditor::Constants::Materials::RoundedMask,
        TEXT("SCard::CreateRoundedMaskMID"));

    if (RoundedMaskMat == nullptr)
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Failed to load rounded mask material - rounded corners disabled"));
        return nullptr;
    }

    UMaterialInstanceDynamic *DynamicMID = UMaterialInstanceDynamic::Create(RoundedMaskMat, GetTransientPackage());
    if (DynamicMID)
    {
        const float SmoothingFactor = ConvaiEditor::Constants::Layout::Radius::CardCornerSmoothing;
        const float InnerCornerRadius = FMath::Max(0.f, InOuterCornerRadius - BorderThicknessPx);
        DynamicMID->SetScalarParameterValue(TEXT("CornerRadiusPx"), FMath::Max(1.f, InnerCornerRadius));
        DynamicMID->SetScalarParameterValue(TEXT("SmoothingPx"), SmoothingFactor);
    }
    else
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Failed to create dynamic material instance for mask"));
    }

    return DynamicMID;
}

void SCard::HandleDisplayMetricsChanged(const FDisplayMetrics &NewMetrics)
{
    if (MaskMID != nullptr)
    {
        const float NewScale = FSlateApplication::Get().GetApplicationScale();
        MaskMID->SetScalarParameterValue(TEXT("UIScale"), NewScale);

        if (RetainerWidget.IsValid())
        {
            FVector2D WidgetSize = RetainerWidget->GetCachedGeometry().GetLocalSize();
            UpdateMaterialParameters(WidgetSize);
        }
    }
    else
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Cannot update UIScale - mask material instance is null"));
    }
}

void SCard::OnArrangeChildren(const FGeometry &AllottedGeometry, FArrangedChildren &ArrangedChildren) const
{
    SCompoundWidget::OnArrangeChildren(AllottedGeometry, ArrangedChildren);

    if (MaskMID && RetainerWidget.IsValid())
    {
        FVector2D InnerSize = RetainerWidget->GetCachedGeometry().GetLocalSize();

        if (!CardFixedSize.IsZero())
        {
            InnerSize = CardFixedSize - FVector2D(2 * BorderThicknessPx, 2 * BorderThicknessPx);
        }

        if (InnerSize.X > 0 && InnerSize.Y > 0)
        {
            if (!const_cast<SCard *>(this)->bMaterialParametersInitialized ||
                !InnerSize.Equals(const_cast<SCard *>(this)->LastKnownInnerSize, 0.5f))
            {
                const_cast<SCard *>(this)->LastKnownInnerSize = InnerSize;
                const_cast<SCard *>(this)->UpdateMaterialParameters(InnerSize);
                const_cast<SCard *>(this)->bMaterialParametersInitialized = true;
            }
        }
    }
}

void SCard::Tick(const FGeometry &AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
    if (!bMaterialParametersInitialized && MaskMID && RetainerWidget.IsValid())
    {
        SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

        FVector2D InnerSize = RetainerWidget->GetCachedGeometry().GetLocalSize();

        if (!CardFixedSize.IsZero())
        {
            InnerSize = CardFixedSize - FVector2D(2 * BorderThicknessPx, 2 * BorderThicknessPx);
        }

        if (InnerSize.X > 0 && InnerSize.Y > 0)
        {
            LastKnownInnerSize = InnerSize;
            UpdateMaterialParameters(InnerSize);
            bMaterialParametersInitialized = true;
        }
    }
}

void SCard::UpdateMaterialParameters(const FVector2D &SizePx)
{
    if (!MaskMID)
        return;

    MaskMID->SetScalarParameterValue(TEXT("WidgetWidth"), SizePx.X);
    MaskMID->SetScalarParameterValue(TEXT("WidgetHeight"), SizePx.Y);

    const float InnerCornerRadius = FMath::Max(0.f, BorderRadiusPx - BorderThicknessPx);
    MaskMID->SetScalarParameterValue(TEXT("CornerRadiusPx"), FMath::Max(1.0f, InnerCornerRadius));

    const float SmoothingFactor = ConvaiEditor::Constants::Layout::Radius::CardCornerSmoothing;
    MaskMID->SetScalarParameterValue(TEXT("SmoothingPx"), SmoothingFactor);
}

TSharedRef<SWidget> SCard::BuildSampleOverlay(const TSharedPtr<FSampleItem> &InSampleItem) const
{
    const FLinearColor CardBackgroundColor = FConvaiStyle::RequireColor(FName("Convai.Color.component.sampleCard.bg"));
    const float CardPadding = ConvaiEditor::Constants::Layout::Spacing::SampleCardPadding;
    const FLinearColor TextColor = FConvaiStyle::RequireColor(FName("Convai.Color.text.sampleCard"));

    return SNew(SBorder)
        .BorderImage(new FSlateColorBrush(CardBackgroundColor))
        .Padding(0)
        .Content()
            [SNew(SOverlay) + SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)[SNew(SScaleBox).Stretch(EStretch::ScaleToFill)[SNew(SImage).Image(GetSampleImageBrush(InSampleItem)).ColorAndOpacity(FLinearColor::White)]]

             + SOverlay::Slot()
                   .VAlign(VAlign_Bottom)
                   .HAlign(HAlign_Fill)
                       [SNew(SBox)
                            .HeightOverride(ConvaiEditor::Constants::Layout::Components::SampleCard::GradientHeight)
                                [SNew(SImage)
                                     .Image(GetGradientBrush())
                                     .Visibility(EVisibility::HitTestInvisible)]]

             + SOverlay::Slot()
                   .VAlign(VAlign_Bottom)
                       [SNew(SVerticalBox)

                        + SVerticalBox::Slot()
                              .AutoHeight()
                              .Padding(FMargin(CardPadding))
                                  [SNew(STextBlock)
                                       .Text(InSampleItem.IsValid() ? InSampleItem->Name : FText::GetEmpty())
                                       .ColorAndOpacity(TextColor)
                                       .Font(FConvaiStyle::Get().GetFontStyle("Convai.Font.sampleCardTitle"))]

                        + SVerticalBox::Slot()
                              .AutoHeight()
                              .Padding(FMargin(CardPadding, CardPadding * -0.75f, CardPadding, CardPadding))
                                  [SNew(SHorizontalBox)
                                       .Visibility(bShouldShowTags && InSampleItem.IsValid() && InSampleItem->Tags.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed) +
                                   SHorizontalBox::Slot()
                                       .HAlign(HAlign_Left)
                                           [BuildTagRow(InSampleItem)]]]];
}

TSharedRef<SWidget> SCard::BuildTagRow(const TSharedPtr<FSampleItem> &InSampleItem) const
{
    const float CardPadding = ConvaiEditor::Constants::Layout::Spacing::SampleCardPadding;
    TSharedRef<SHorizontalBox> TagsBox = SNew(SHorizontalBox);

    if (InSampleItem.IsValid())
    {
        for (const FString &TagText : InSampleItem->Tags)
        {
            TagsBox->AddSlot()
                .AutoWidth()
                .Padding(FMargin(0, 0, CardPadding * 0.5f, 0))
                    [CreateTagWidget(TagText)];
        }
    }
    return TagsBox;
}

TSharedRef<SWidget> SCard::CreateTagWidget(const FString &TagText) const
{
    const float TagBorderRadius = ConvaiEditor::Constants::Layout::Radius::SampleCardTag;
    const FLinearColor TagBgColor = FConvaiStyle::RequireColor(FName("Convai.Color.component.sampleCard.tagBg"));
    const FLinearColor TagTextColor = FConvaiStyle::RequireColor(FName("Convai.Color.component.sampleCard.tagText"));

    FName TagBrushKey = FName(*FString::Printf(TEXT("Tag.%s.%f"), *TagText, TagBorderRadius));
    auto BrushResult = FConvaiStyleResources::Get().GetOrCreateRoundedBoxBrush(TagBrushKey, TagBgColor, TagBorderRadius);

    const FSlateBrush *TagBrush = nullptr;
    if (BrushResult.IsSuccess())
    {
        TagBrush = BrushResult.GetValue().Get();
    }
    else
    {
        TagBrush = FConvaiStyle::GetTransparentBrush();
    }

    return SNew(SBorder)
        .BorderImage(TagBrush)
        .Padding(FMargin(6.0f, 3.0f))
        .Content()
            [SNew(STextBlock)
                 .Text(FText::FromString(TagText))
                 .ColorAndOpacity(TagTextColor)
                 .Font(FConvaiStyle::Get().GetFontStyle("Convai.Font.sampleCardTag"))];
}

const FSlateBrush *SCard::GetSampleImageBrush(const TSharedPtr<FSampleItem> &InSampleItem) const
{
    if (DynamicImageBrushAttribute.IsSet())
    {
        const FSlateBrush *DynamicBrush = DynamicImageBrushAttribute.Get();
        if (DynamicBrush)
        {
            return DynamicBrush;
        }
    }

    if (!SampleImageBrush.IsValid() && InSampleItem.IsValid() && !InSampleItem->ImagePath.IsEmpty())
    {
        FString BasePath = IPluginManager::Get().FindPlugin("Convai")->GetBaseDir();
        FString ResourcesPath = FPaths::Combine(BasePath, ConvaiEditor::Constants::PluginResources::Root);
        FString FullImagePath = FPaths::Combine(ResourcesPath, InSampleItem->ImagePath);

        FVector2D ImageSize = (InSampleItem->Tags.Num() == 1) ? ConvaiEditor::Constants::Layout::Components::HomePageCard::Dimensions : ConvaiEditor::Constants::Layout::Components::SampleCard::Dimensions;

        SampleImageBrush = MakeShared<FSlateDynamicImageBrush>(FName(*FullImagePath), ImageSize);
        SampleImageBrush->DrawAs = ESlateBrushDrawType::Image;
    }
    return SampleImageBrush.IsValid() ? SampleImageBrush.Get() : FCoreStyle::Get().GetBrush("WhiteBrush");
}

const FSlateBrush *SCard::GetGradientBrush() const
{
    if (!GradientBrush.IsValid())
    {
        const FString PluginDir = IPluginManager::Get().FindPlugin(TEXT("Convai"))->GetBaseDir();
        const FString ImagePath = FPaths::Combine(PluginDir, ConvaiEditor::Constants::PluginResources::Root, ConvaiEditor::Constants::Images::Gradient_1x256);

        GradientBrush = MakeShared<FSlateImageBrush>(FName(*ImagePath), FVector2D(1.f, 256.f));
    }

    return GradientBrush.Get();
}

const FSlateBrush *SCard::MakeOutlineBrush(
    const FLinearColor &InColor,
    float InRadius,
    float InThickness) const
{
    float AdjustedRadius = FMath::Max(1.0f, InRadius);

    TSharedPtr<FSlateRoundedBoxBrush> TempBrush = FConvaiStyleResources::Get().CreateTemporaryRoundedBoxBrush(InColor, AdjustedRadius);
    if (!TempBrush.IsValid())
    {
        UE_LOG(LogCard, Warning, TEXT("Outline brush creation failed, using fallback."));
        return FConvaiStyle::GetTransparentBrush();
    }

    static TArray<TSharedPtr<FSlateRoundedBoxBrush>> TempBrushCache;
    TempBrushCache.Add(TempBrush);

    if (TempBrushCache.Num() > 100)
    {
        TempBrushCache.RemoveAt(0, 50);
    }

    return TempBrush.Get();
}

TSharedRef<SWidget> SCard::BuildSimpleOverlay(const TSharedPtr<FSampleItem> &InSampleItem) const
{
    const FLinearColor CardBackgroundColor = FConvaiStyle::RequireColor(FName("Convai.Color.component.sampleCard.bg"));
    const float CardPadding = ConvaiEditor::Constants::Layout::Spacing::SampleCardPadding;
    const FLinearColor TextColor = FConvaiStyle::RequireColor(FName("Convai.Color.text.sampleCard"));

    FSlateFontInfo HomepageTitleFont = FConvaiStyle::Get().GetFontStyle("Convai.Font.sampleCardTitle");
    HomepageTitleFont.Size = CustomTitleFontSize;

    FName BgBrushKey = FName(TEXT("Homepage.CardBackground"));
    auto BgBrushResult = FConvaiStyleResources::Get().GetOrCreateColorBrush(BgBrushKey, CardBackgroundColor);

    const FSlateBrush *BackgroundBrush = nullptr;
    if (BgBrushResult.IsSuccess())
    {
        BackgroundBrush = BgBrushResult.GetValue().Get();
    }
    else
    {
        BackgroundBrush = FConvaiStyle::GetTransparentBrush();
    }

    return SNew(SBorder)
        .BorderImage(BackgroundBrush)
        .Padding(0)
        .Content()
            [SNew(SOverlay) + SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)[SNew(SScaleBox).Stretch(EStretch::ScaleToFill)[SNew(SImage).Image_Lambda([this, InSampleItem]() -> const FSlateBrush *
                                                                                                                                                                { return GetSampleImageBrush(InSampleItem); })
                                                                                                                                          .ColorAndOpacity(FLinearColor::White)]]

             + SOverlay::Slot()
                   .VAlign(VAlign_Bottom)
                   .HAlign(HAlign_Fill)
                       [SNew(SBox)
                            .HeightOverride(ConvaiEditor::Constants::Layout::Components::SampleCard::GradientHeight)
                                [SNew(SImage)
                                     .Image(GetGradientBrush())
                                     .Visibility(EVisibility::HitTestInvisible)]]

             + SOverlay::Slot()
                   .VAlign(VAlign_Bottom)
                   .HAlign(bShouldCenterTitle ? HAlign_Center : HAlign_Fill)
                       [SNew(SVerticalBox)

                        + SVerticalBox::Slot()
                              .AutoHeight()
                              .Padding(FMargin(CardPadding, CardPadding, CardPadding, CardPadding * 1.5f))
                                  [SNew(STextBlock)
                                       .Text(InSampleItem.IsValid() ? InSampleItem->Name : FText::GetEmpty())
                                       .ColorAndOpacity(TextColor)
                                       .Font(HomepageTitleFont)
                                       .Justification(bShouldCenterTitle ? ETextJustify::Center : ETextJustify::Left)
                                       .AutoWrapText(true)]]];
}
