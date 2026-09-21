/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiAccessibility.cpp
 *
 * Implementation of accessibility utilities for UI widgets.
 */

#include "UI/Utility/ConvaiAccessibility.h"
#include "Widgets/SWidget.h"
#include "Widgets/Input/SButton.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SToolTip.h"

void FConvaiAccessibility::ApplyAccessibleParams(
    TSharedRef<SWidget> Widget,
    const FText &AccessibleText,
    const FText &AccessibleSummaryText,
    const FText &AccessibleHelpText)
{
    if (!AccessibleHelpText.IsEmpty())
    {
        Widget->SetToolTipText(AccessibleHelpText);
    }
    else if (!AccessibleText.IsEmpty())
    {
        Widget->SetToolTipText(AccessibleText);
    }
}

void FConvaiAccessibility::SetupKeyboardFocusTraversal(
    const TArray<TSharedRef<SWidget>> &Widgets,
    bool bLoopTraversal)
{
    if (Widgets.Num() <= 1)
    {
        return;
    }

    for (int32 i = 0; i < Widgets.Num(); ++i)
    {
        if (Widgets[i]->GetType().ToString().Contains(TEXT("SButton")))
        {
            TSharedRef<SButton> Button = StaticCastSharedRef<SButton>(Widgets[i]);
        }
    }
}

void FConvaiAccessibility::AddAriaAttributes(
    TSharedRef<SWidget> Widget,
    const FString &Role,
    const TMap<FString, FString> &AriaAttributes)
{
    if (Role.Equals(TEXT("button")))
    {
        if (Widget->GetType().ToString().Contains(TEXT("SButton")))
        {
            for (const auto &Attribute : AriaAttributes)
            {
                if (Attribute.Key.Equals(TEXT("aria-label")) || Attribute.Key.Equals(TEXT("aria-description")))
                {
                    Widget->SetToolTipText(FText::FromString(Attribute.Value));
                    break;
                }
            }
        }
    }
    else if (Role.Equals(TEXT("textbox")))
    {
        if (Widget->GetType().ToString().Contains(TEXT("SEditableText")))
        {
            TSharedRef<SEditableText> EditableText = StaticCastSharedRef<SEditableText>(Widget);

            for (const auto &Attribute : AriaAttributes)
            {
                if (Attribute.Key.Equals(TEXT("aria-readonly")))
                {
                    EditableText->SetIsReadOnly(Attribute.Value.ToBool());
                    break;
                }
            }
        }
    }
}

TSharedRef<IToolTip> FConvaiAccessibility::CreateAccessibleTooltip(const FText &TooltipText)
{
    TSharedRef<SToolTip> Tooltip = SNew(SToolTip)
                                       .Text(TooltipText)
                                       .BorderImage(FCoreStyle::Get().GetBrush("ToolTip.BrightBackground"));

    return Tooltip;
}

void FConvaiAccessibility::SetupScreenReaderAnnouncement(
    TSharedRef<SWidget> Widget,
    const FText &AnnouncementText,
    ConvaiEditor::AccessibleAnnouncement::Priority AnnouncementPriority)
{
    Widget->SetToolTipText(AnnouncementText);
}
