/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IThemeManager.h
 *
 * Interface for theme management in the Convai Editor.
 */

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"
#include "Delegates/Delegate.h"
#include "ConvaiEditor.h"

/**
 * Interface for theme management in the Convai Editor.
 */
class CONVAIEDITOR_API IThemeManager : public IConvaiService
{
public:
    virtual ~IThemeManager() = default;

    /** Sets the active theme by ID */
    virtual void SetActiveTheme(const FString &ThemeId) = 0;

    /** Returns the active style set */
    virtual const TSharedPtr<FSlateStyleSet> &GetStyle() const = 0;

    /** Returns the current theme ID */
    virtual FString GetCurrentThemeId() const = 0;

    DECLARE_MULTICAST_DELEGATE(FOnThemeChanged);

    /** Returns the theme changed delegate */
    virtual FOnThemeChanged &OnThemeChanged() = 0;

    static FName StaticType() { return TEXT("IThemeManager"); }
};
