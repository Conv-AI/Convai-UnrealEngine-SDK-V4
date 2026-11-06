/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiAccessibility.h
 *
 * Accessibility utility helpers for ConvaiEditor.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWidget.h"
#include "Framework/Application/SlateApplication.h"

namespace ConvaiEditor
{
    namespace AccessibleAnnouncement
    {
        enum Priority
        {
            Low,
            Normal,
            High
        };
    }
}

/**
 * Accessibility utility helpers.
 */
class CONVAIEDITOR_API FConvaiAccessibility
{
public:
    /** Applies accessibility parameters to a widget */
    static void ApplyAccessibleParams(
        TSharedRef<SWidget> Widget,
        const FText &AccessibleText,
        const FText &AccessibleSummaryText = FText::GetEmpty(),
        const FText &AccessibleHelpText = FText::GetEmpty());

    /** Sets up keyboard focus traversal for a group of widgets */
    static void SetupKeyboardFocusTraversal(
        const TArray<TSharedRef<SWidget>> &Widgets,
        bool bLoopTraversal = true);

    /** Adds ARIA attributes to a widget */
    static void AddAriaAttributes(
        TSharedRef<SWidget> Widget,
        const FString &Role,
        const TMap<FString, FString> &AriaAttributes);

    /** Creates an accessible tooltip */
    static TSharedRef<class IToolTip> CreateAccessibleTooltip(const FText &TooltipText);

    /** Sets up screen reader announcement for a widget */
    static void SetupScreenReaderAnnouncement(
        TSharedRef<SWidget> Widget,
        const FText &AnnouncementText,
        ConvaiEditor::AccessibleAnnouncement::Priority AnnouncementPriority = ConvaiEditor::AccessibleAnnouncement::Normal);
};
