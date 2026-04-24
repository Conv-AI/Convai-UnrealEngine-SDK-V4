/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ThemeManager.h
 *
 * Manages loading and hot-reloading of UI themes.
 */

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"
#include "Delegates/Delegate.h"
#include "Styling/IThemeManager.h"

/**
 * Manages loading and hot-reloading of UI themes.
 */
class CONVAIEDITOR_API FThemeManager : public IThemeManager
{
public:
    FThemeManager();
    virtual ~FThemeManager();

    virtual void Startup() override;
    virtual void Shutdown() override;

    virtual void SetActiveTheme(const FString &ThemeId) override;
    virtual const TSharedPtr<FSlateStyleSet> &GetStyle() const override { return Style; }
    virtual FString GetCurrentThemeId() const override { return CurrentThemeId; }
    virtual FOnThemeChanged &OnThemeChanged() override { return OnThemeChangedDelegate; }

    static FName StaticType() { return TEXT("IThemeManager"); }

private:
    /** Loads a theme file by ID */
    bool LoadThemeFile(const FString &Id);

    TSharedPtr<FSlateStyleSet> Style;
    FString CurrentThemeId;
    FOnThemeChanged OnThemeChangedDelegate;
};
