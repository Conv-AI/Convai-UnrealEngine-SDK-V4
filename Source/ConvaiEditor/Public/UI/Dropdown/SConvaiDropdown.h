/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiDropdown.h
 *
 * Dropdown menu widget with support for nested entries.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Views/STableViewBase.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/SListView.h"

struct FSlateBrush;

/**
 * Menu entry structure for dropdown items.
 */
struct FConvaiMenuEntry
{
    FText Label;
    FSimpleDelegate Action;
    TArray<FConvaiMenuEntry> Children;
    bool bHasChildren = false;
    bool bHighlight = false;

    FConvaiMenuEntry() = default;

    FConvaiMenuEntry(const FText &InLabel, const FSimpleDelegate &InAction, bool bInHighlight = false)
        : Label(InLabel), Action(InAction), bHasChildren(false), bHighlight(bInHighlight)
    {
    }

    FConvaiMenuEntry(const FText &InLabel, const TArray<FConvaiMenuEntry> &InChildren, bool bInHighlight = false)
        : Label(InLabel), Children(InChildren), bHasChildren(true), bHighlight(bInHighlight)
    {
    }

    bool HasChildren() const { return bHasChildren && Children.Num() > 0; }
};

class SConvaiNestedDropdown;

/**
 * Dropdown menu widget with support for nested entries.
 */
class CONVAIEDITOR_API SConvaiDropdown : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SConvaiDropdown) {}
    SLATE_ARGUMENT(TArray<FConvaiMenuEntry>, Entries)
    SLATE_ARGUMENT(TWeakPtr<SWindow>, OwningWindow)
    SLATE_ARGUMENT(FName, FontStyle)
    SLATE_ARGUMENT(bool, SupportsNested)
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &);

protected:
    TSharedRef<SWidget> BuildEntry(const FConvaiMenuEntry &Entry);
    virtual TSharedRef<SWidget> BuildNestedEntry(const FConvaiMenuEntry &Entry);

    TWeakPtr<SWindow> Window;
    const FSlateBrush *DropdownBackgroundBrush;
    FName FontStyleName;
    bool bSupportsNested = false;
};
