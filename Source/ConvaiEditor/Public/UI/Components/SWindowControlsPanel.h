/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SWindowControlsPanel.h
 *
 * Window controls panel with user avatar, settings, and window buttons.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/SlateTypes.h"
#include "HAL/Platform.h"
#include "Framework/Application/IInputProcessor.h"

class SButton;
struct FConvaiMenuEntry;
class SMenuAnchor;

/**
 * Dropdown state container for menu anchors.
 */
struct FSettingsDropdownHoverState
{
    TSharedPtr<SMenuAnchor> anchor;
};

/**
 * Input processor for detecting clicks outside menus.
 */
class FMenuClickOutsideDetector : public IInputProcessor
{
public:
    FMenuClickOutsideDetector(TWeakPtr<FSettingsDropdownHoverState> InHoverState, const FString &InMenuWidgetTypeName = TEXT(""))
        : HoverState(InHoverState), MenuWidgetTypeName(InMenuWidgetTypeName)
    {
    }

    virtual void Tick(const float DeltaTime, FSlateApplication &SlateApp, TSharedRef<ICursor> Cursor) override {}
    virtual bool HandleMouseButtonDownEvent(FSlateApplication &SlateApp, const FPointerEvent &MouseEvent) override;

private:
    TWeakPtr<FSettingsDropdownHoverState> HoverState;
    FString MenuWidgetTypeName;
};

/**
 * Window controls panel with user avatar, settings, and window buttons.
 */
class SWindowControlsPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SWindowControlsPanel) {}
    SLATE_EVENT(FSimpleDelegate, OnSettingsClicked)
    SLATE_EVENT(FSimpleDelegate, OnMinimizeClicked)
    SLATE_EVENT(FSimpleDelegate, OnMaximizeClicked)
    SLATE_EVENT(FSimpleDelegate, OnCloseClicked)
    SLATE_EVENT(FSimpleDelegate, OnManageAccountClicked)
    SLATE_EVENT(FSimpleDelegate, OnSignOutClicked)
    SLATE_ATTRIBUTE(bool, IsMaximized)
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    virtual ~SWindowControlsPanel();

private:
    FSimpleDelegate OnSettingsClicked;
    FSimpleDelegate OnMinimizeClicked;
    FSimpleDelegate OnMaximizeClicked;
    FSimpleDelegate OnCloseClicked;
    FSimpleDelegate OnManageAccountClicked;
    FSimpleDelegate OnSignOutClicked;
    TAttribute<bool> IsMaximized;

    TSharedPtr<SButton> MinimizeButton;
    TSharedPtr<SButton> MaximizeButton;
    TSharedPtr<SButton> CloseButton;

    FString CachedUsername;
    FString CachedEmail;

    const FSlateBrush *GetMaximizeRestoreIcon() const;
    TSharedRef<class SWidget> BuildSettingsDropdown(TSharedPtr<FSettingsDropdownHoverState> HoverState);
    TSharedRef<class SWidget> BuildDropdownMenu(const TArray<FConvaiMenuEntry> &Entries, const FName &FontStyle);
    TSharedRef<class SWidget> BuildAccountMenu(TSharedPtr<FSettingsDropdownHoverState> HoverState);

    TMap<TWeakPtr<SMenuAnchor>, TSharedPtr<FMenuClickOutsideDetector>> ActiveMenuDetectors;

    /** Registers a menu for automatic click-outside detection */
    void RegisterMenuClickOutside(TSharedPtr<SMenuAnchor> MenuAnchor, TSharedPtr<FSettingsDropdownHoverState> HoverState, const FString &MenuWidgetTypeName);

    /** Unregisters a menu from click-outside detection */
    void UnregisterMenuClickOutside(TSharedPtr<SMenuAnchor> MenuAnchor);

    /** Cleans up all active menu detectors */
    void UnregisterAllMenuDetectors();

    void OnExportLogsClicked();
    void OnContactSupportClicked();
    void OnCheckForUpdatesClicked();
    void FetchUserAccountData();

    TSharedPtr<class SUpdateNotificationBadge> UpdateBadge;
    void OnUpdateAvailabilityChanged(bool bUpdateAvailable, const FString &LatestVersion);
    void RefreshUpdateBadge();
};