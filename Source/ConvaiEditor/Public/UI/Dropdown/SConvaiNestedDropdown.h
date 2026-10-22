/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiNestedDropdown.h
 *
 * Nested dropdown widget supporting multi-level menu hierarchies.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Dropdown/SConvaiDropdown.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Containers/Ticker.h"

/**
 * Hover state management for nested dropdown menus.
 */
struct FNestedDropdownHoverState
{
    TSharedPtr<SMenuAnchor> SubMenuAnchor;
    bool bParentHovered = false;
    bool bSubMenuHovered = false;
    bool bSubMenuOpen = false;
    FTSTicker::FDelegateHandle TickerHandle;

    void Shutdown();
    void ClearTicker();
    void CloseIfNotHovered();
    void OpenSubMenu();
    void CloseSubMenu();
};

/**
 * Nested dropdown widget supporting multi-level menu hierarchies.
 */
class CONVAIEDITOR_API SConvaiNestedDropdown : public SConvaiDropdown
{
public:
    SLATE_BEGIN_ARGS(SConvaiNestedDropdown) {}
    SLATE_ARGUMENT(TArray<FConvaiMenuEntry>, Entries)
    SLATE_ARGUMENT(TWeakPtr<SWindow>, OwningWindow)
    SLATE_ARGUMENT(FName, FontStyle)
    SLATE_ARGUMENT(int32, NestingLevel)
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    virtual ~SConvaiNestedDropdown();

protected:
    TSharedRef<SWidget> BuildNestedEntry(const FConvaiMenuEntry &Entry) override;
    TSharedRef<SWidget> CreateSubMenuContent(const FConvaiMenuEntry &Entry, TSharedPtr<FNestedDropdownHoverState> HoverState);
    void SetupNestedHoverBehavior(
        TSharedPtr<SButton> Button,
        TSharedPtr<FNestedDropdownHoverState> HoverState,
        const FConvaiMenuEntry &Entry);

private:
    int32 CurrentNestingLevel = 0;
    TArray<TSharedPtr<FNestedDropdownHoverState>> NestedHoverStates;
    static constexpr int32 MaxNestingDepth = 3;
};
