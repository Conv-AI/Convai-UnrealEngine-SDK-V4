/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SHeaderBar.h
 *
 * Header bar widget with navigation and window controls.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Styling/SlateColor.h"
#include "Styling/ConvaiStyle.h"
#include "Services/NavigationService.h"
#include "Services/Routes.h"
#include "Containers/Ticker.h"
#include "Delegates/Delegate.h"

struct FPointerEvent;
struct FGeometry;
struct FConvaiMenuEntry;

/**
 * Dropdown hover state management structure.
 */
struct FDropdownHoverState
{
    TSharedPtr<SMenuAnchor> anchor;
    bool bAnchorHovered = false;
    bool bMenuHovered = false;
    FTSTicker::FDelegateHandle TickerHandle;

    void Shutdown();
    void ClearTicker();
    void CloseIfNotHovered();
};

/**
 * Header bar widget with navigation and window controls.
 */
class CONVAIEDITOR_API SHeaderBar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SHeaderBar) {}
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    virtual ~SHeaderBar();

    void OnSettingsClicked();
    void OnMinimizeClicked();
    void OnMaximizeClicked();
    void OnCloseClicked();
    void OnSignOutClicked();
    bool IsWindowMaximized() const;

private:
    TSharedRef<SWidget> BuildNav();
    TSharedRef<SWidget> BuildSamplesDropdown();
    TSharedRef<SWidget> BuildFeaturesDropdown();
    TSharedRef<SWidget> BuildDropdownMenu(
        const TArray<FConvaiMenuEntry> &Entries,
        const FName &FontStyle,
        FDropdownHoverState &HoverState);
    TSharedRef<SWidget> BuildNestedDropdownMenu(
        const TArray<FConvaiMenuEntry> &Entries,
        const FName &FontStyle,
        FDropdownHoverState &HoverState);

    FSlateColor GetNavColor() const;
    FReply OnNavButtonClicked(ConvaiEditor::Route::E Route);
    bool AnchorValidAndOpen(const FName &Route) const;
    void SetupAnchorHoverBehavior(
        TSharedPtr<SButton> Button,
        FDropdownHoverState &HoverState);

    ConvaiEditor::Route::E ActiveRoute = static_cast<ConvaiEditor::Route::E>(-1);
    TMap<FName, TSharedPtr<SWidget>> NavWidgets;
    FDropdownHoverState SamplesHoverState;
    FDropdownHoverState FeaturesHoverState;
    FDelegateHandle RouteChangedHandle;

    TSharedRef<SWidget> MakeNavItem(ConvaiEditor::Route::E Route, const FText &Label, const FSlateBrush *Icon = nullptr);
};
