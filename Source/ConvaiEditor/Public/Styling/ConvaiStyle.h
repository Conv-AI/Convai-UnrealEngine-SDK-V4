/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiStyle.h
 *
 * Main interface for the ConvaiStyle system.
 */

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateTypes.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "Templates/SharedPointer.h"
#include "Dom/JsonObject.h"
#include "ConvaiEditor.h"

class IConvaiStyleRegistry;

/** Main ConvaiStyle system. */
class CONVAIEDITOR_API FConvaiStyle
{
public:
    static void Initialize(const TSharedPtr<FJsonObject> &ThemeJson = nullptr);
    static void Shutdown();
    static const ISlateStyle &Get();
    static FName GetStyleSetName();
    static const FSlateBrush *GetRoundedDropdownBrush();
    static const FSlateBrush *GetSampleCardOutlineBrush();
    static const FSlateBrush *GetStandardCardOutlineBrush(const TOptional<FLinearColor> &CustomColor = TOptional<FLinearColor>());
    static const FSlateBrush *GetContentContainerBrush(const TOptional<FLinearColor> &CustomColor = TOptional<FLinearColor>());
    static const FSlateBrush *GetTransparentBrush();
    static const FSlateBrush *GetDevInfoBoxBrush();
    static FLinearColor RequireColor(const FName &Key);
    static const FScrollBarStyle &GetScrollBarStyle();
    static const FScrollBoxStyle &GetScrollBoxStyle(bool bShowShadow = false);
    static TSharedPtr<IConvaiStyleRegistry> GetStyleRegistry();

private:
    static void EnsureInitialized();
    static bool InitializeServices(const TSharedPtr<FJsonObject> &ThemeJson);
    static void ShutdownServices();

    static bool bIsInitialized;
    static TWeakPtr<IConvaiStyleRegistry> StyleRegistryService;

    static TSharedPtr<FSlateBrush> CachedDropdownBrush;
    static TSharedPtr<FSlateBrush> CachedSampleCardOutlineBrush;
    static TSharedPtr<FSlateBrush> CachedStandardCardOutlineBrush;
    static TSharedPtr<FSlateBrush> CachedContentContainerBrush;
    static TSharedPtr<FSlateBrush> CachedTransparentBrush;
    static TSharedPtr<FSlateBrush> CachedDevInfoBoxBrush;

    static FScrollBarStyle CachedScrollBarStyle;
    static FScrollBoxStyle CachedScrollBoxStyleWithShadow;
    static FScrollBoxStyle CachedScrollBoxStyleNoShadow;
};
