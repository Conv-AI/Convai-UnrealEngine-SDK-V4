/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiStyle.cpp
 *
 * Implementation of the ConvaiStyle system for managing Slate UI styles.
 */

#include "Styling/ConvaiStyle.h"
#include "Styling/IConvaiStyleRegistry.h"
#include "Services/ConvaiDIContainer.h"
#include "Logging/ConvaiEditorThemeLog.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/CoreStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "Utility/ConvaiConstants.h"
#include "SlateBasics.h"
#include "Interfaces/IPluginManager.h"

bool FConvaiStyle::bIsInitialized = false;
TWeakPtr<IConvaiStyleRegistry> FConvaiStyle::StyleRegistryService;

TSharedPtr<FSlateBrush> FConvaiStyle::CachedDropdownBrush;
TSharedPtr<FSlateBrush> FConvaiStyle::CachedSampleCardOutlineBrush;
TSharedPtr<FSlateBrush> FConvaiStyle::CachedStandardCardOutlineBrush;
TSharedPtr<FSlateBrush> FConvaiStyle::CachedContentContainerBrush;
TSharedPtr<FSlateBrush> FConvaiStyle::CachedTransparentBrush;
TSharedPtr<FSlateBrush> FConvaiStyle::CachedDevInfoBoxBrush;

FScrollBarStyle FConvaiStyle::CachedScrollBarStyle;
FScrollBoxStyle FConvaiStyle::CachedScrollBoxStyleWithShadow;
FScrollBoxStyle FConvaiStyle::CachedScrollBoxStyleNoShadow;

void FConvaiStyle::Initialize(const TSharedPtr<FJsonObject> &ThemeJson)
{
    if (bIsInitialized)
    {
        UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyle: already initialized"));
        return;
    }

    if (!InitializeServices(ThemeJson))
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ConvaiStyle: failed to initialize services"));
        return;
    }

    if (FSlateApplication::IsInitialized())
    {
        CachedScrollBoxStyleWithShadow = FCoreStyle::Get().GetWidgetStyle<FScrollBoxStyle>("ScrollBox");
        CachedScrollBoxStyleNoShadow = CachedScrollBoxStyleWithShadow;
        CachedScrollBoxStyleNoShadow.TopShadowBrush = FSlateNoResource();
        CachedScrollBoxStyleNoShadow.BottomShadowBrush = FSlateNoResource();
        CachedScrollBoxStyleNoShadow.LeftShadowBrush = FSlateNoResource();
        CachedScrollBoxStyleNoShadow.RightShadowBrush = FSlateNoResource();
    }

    bIsInitialized = true;
}

void FConvaiStyle::Shutdown()
{
    if (!bIsInitialized)
    {
        return;
    }

    CachedDropdownBrush.Reset();
    CachedSampleCardOutlineBrush.Reset();
    CachedStandardCardOutlineBrush.Reset();
    CachedContentContainerBrush.Reset();
    CachedTransparentBrush.Reset();
    CachedDevInfoBoxBrush.Reset();

    auto Registry = GetStyleRegistry();
    if (Registry.IsValid())
    {
        auto Result = Registry->ShutdownStyleRegistry();
        if (Result.IsFailure())
        {
            UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyle: error shutting down style registry - %s"), *Result.GetError());
        }
    }

    ShutdownServices();

    bIsInitialized = false;
}

const ISlateStyle &FConvaiStyle::Get()
{
    EnsureInitialized();

    auto Registry = GetStyleRegistry();
    if (Registry.IsValid())
    {
        auto StyleSet = Registry->GetStyleSet();
        if (StyleSet.IsValid())
        {
            return *StyleSet;
        }
    }

    UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyle: services unavailable, falling back to CoreStyle"));
    return FCoreStyle::Get();
}

FName FConvaiStyle::GetStyleSetName()
{
    return FName(TEXT("ConvaiStyle"));
}

const FSlateBrush *FConvaiStyle::GetRoundedDropdownBrush()
{
    if (!CachedDropdownBrush.IsValid())
    {
        EnsureInitialized();

        FLinearColor DropdownColor = RequireColor(FName("Convai.Color.surface.dropdown"));
        float DropdownRadius = ConvaiEditor::Constants::Layout::Radius::Dropdown;

        CachedDropdownBrush = MakeShared<FSlateRoundedBoxBrush>(DropdownColor, DropdownRadius);
    }

    return CachedDropdownBrush.Get();
}

const FSlateBrush *FConvaiStyle::GetSampleCardOutlineBrush()
{
    if (!CachedSampleCardOutlineBrush.IsValid())
    {
        EnsureInitialized();

        FLinearColor OutlineColor = RequireColor(FName("Convai.Color.component.sampleCard.outline"));
        float CardRadius = ConvaiEditor::Constants::Layout::Radius::SampleCard;
        float BorderThickness = ConvaiEditor::Constants::Layout::Components::SampleCard::BorderThickness;

        CachedSampleCardOutlineBrush = MakeShared<FSlateRoundedBoxBrush>(
            FLinearColor::Transparent,
            CardRadius,
            OutlineColor,
            BorderThickness);
    }

    return CachedSampleCardOutlineBrush.Get();
}

const FSlateBrush *FConvaiStyle::GetStandardCardOutlineBrush(const TOptional<FLinearColor> &CustomColor)
{
    if (!CachedStandardCardOutlineBrush.IsValid())
    {
        EnsureInitialized();

        FLinearColor DefaultOutlineColor = RequireColor(FName("Convai.Color.component.standardCard.outline"));
        FLinearColor BorderColor = CustomColor.Get(DefaultOutlineColor);
        float CardRadius = ConvaiEditor::Constants::Layout::Radius::StandardCard;
        float BorderThickness = ConvaiEditor::Constants::Layout::Components::StandardCard::BorderThickness;

        CachedStandardCardOutlineBrush = MakeShared<FSlateRoundedBoxBrush>(
            FLinearColor::Transparent,
            CardRadius,
            BorderColor,
            BorderThickness);
    }

    return CachedStandardCardOutlineBrush.Get();
}

const FSlateBrush *FConvaiStyle::GetContentContainerBrush(const TOptional<FLinearColor> &CustomColor)
{
    if (!CachedContentContainerBrush.IsValid())
    {
        EnsureInitialized();

        FLinearColor DefaultContentColor = RequireColor(FName("Convai.Color.surface.content"));
        FLinearColor FillColor = CustomColor.Get(DefaultContentColor);
        float ContainerRadius = ConvaiEditor::Constants::Layout::Radius::ContentContainer;

        CachedContentContainerBrush = MakeShared<FSlateRoundedBoxBrush>(FillColor, ContainerRadius);
    }

    return CachedContentContainerBrush.Get();
}

const FSlateBrush *FConvaiStyle::GetTransparentBrush()
{
    if (!CachedTransparentBrush.IsValid())
    {
        EnsureInitialized();

        CachedTransparentBrush = MakeShared<FSlateColorBrush>(FLinearColor::Transparent);
    }

    return CachedTransparentBrush.Get();
}

const FSlateBrush *FConvaiStyle::GetDevInfoBoxBrush()
{
    if (!CachedDevInfoBoxBrush.IsValid())
    {
        EnsureInitialized();

        FLinearColor BoxColor = RequireColor(FName("Convai.Color.component.devInfoBox.bg"));
        float BoxRadius = ConvaiEditor::Constants::Layout::Radius::DevInfoBox;

        CachedDevInfoBoxBrush = MakeShared<FSlateRoundedBoxBrush>(BoxColor, BoxRadius);
    }

    return CachedDevInfoBoxBrush.Get();
}

FLinearColor FConvaiStyle::RequireColor(const FName &Key)
{
    EnsureInitialized();

    auto Registry = GetStyleRegistry();
    if (Registry.IsValid())
    {
        auto StyleSet = Registry->GetStyleSet();
        if (StyleSet.IsValid())
        {
            return StyleSet->GetColor(Key);
        }
    }

    UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyle: StyleRegistry unavailable for required color: %s, using default"), *Key.ToString());
    return FLinearColor::Transparent;
}

const FScrollBarStyle &FConvaiStyle::GetScrollBarStyle()
{
    EnsureInitialized();

    auto Registry = GetStyleRegistry();
    if (Registry.IsValid())
    {
        auto StyleSet = Registry->GetStyleSet();
        if (StyleSet.IsValid())
        {
            CachedScrollBarStyle = FScrollBarStyle()
                                       .SetVerticalTopSlotImage(FSlateNoResource())
                                       .SetVerticalBottomSlotImage(FSlateNoResource())
                                       .SetHorizontalTopSlotImage(FSlateNoResource())
                                       .SetHorizontalBottomSlotImage(FSlateNoResource())
                                       .SetNormalThumbImage(FSlateColorBrush(RequireColor(FName("Convai.Color.icon.scrollBarThumb"))))
                                       .SetHoveredThumbImage(FSlateColorBrush(RequireColor(FName("Convai.Color.icon.scrollBarThumb"))))
                                       .SetDraggedThumbImage(FSlateColorBrush(RequireColor(FName("Convai.Color.icon.scrollBarThumb"))))
                                       .SetVerticalBackgroundImage(FSlateColorBrush(RequireColor(FName("Convai.Color.scrollBarTrack"))))
                                       .SetHorizontalBackgroundImage(FSlateColorBrush(RequireColor(FName("Convai.Color.scrollBarTrack"))))
                                       .SetThickness(ConvaiEditor::Constants::Layout::Components::ScrollBar::Thickness);

            return CachedScrollBarStyle;
        }
    }

    UE_LOG(LogConvaiEditorTheme, Error, TEXT("ConvaiStyle: theme system failed, using emergency fallback for ScrollBarStyle"));
    return FCoreStyle::Get().GetWidgetStyle<FScrollBarStyle>("ScrollBar");
}

const FScrollBoxStyle &FConvaiStyle::GetScrollBoxStyle(bool bShowShadow)
{
    EnsureInitialized();

    return bShowShadow ? CachedScrollBoxStyleWithShadow : CachedScrollBoxStyleNoShadow;
}

void FConvaiStyle::EnsureInitialized()
{
    if (!bIsInitialized)
    {
        UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyle: auto-initialization required"));
        Initialize(nullptr);
    }
}

TSharedPtr<IConvaiStyleRegistry> FConvaiStyle::GetStyleRegistry()
{
    auto Registry = StyleRegistryService.Pin();
    if (!Registry.IsValid())
    {
        auto Result = FConvaiDIContainerManager::Get().Resolve<IConvaiStyleRegistry>();
        if (Result.IsSuccess())
        {
            Registry = Result.GetValue();
            StyleRegistryService = Registry;
        }
    }
    return Registry;
}

bool FConvaiStyle::InitializeServices(const TSharedPtr<FJsonObject> &ThemeJson)
{
    auto Registry = GetStyleRegistry();

    if (!Registry.IsValid())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ConvaiStyle: failed to resolve StyleRegistry service"));
        return false;
    }

    return true;
}

void FConvaiStyle::ShutdownServices()
{
    StyleRegistryService.Reset();
}
