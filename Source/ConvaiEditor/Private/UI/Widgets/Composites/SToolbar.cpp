/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SToolbar.cpp
 *
 * Implementation of the toolbar composite widget.
 */

#include "UI/Widgets/Composites/SToolbar.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SBox.h"

#define LOCTEXT_NAMESPACE "ConvaiEditorToolbar"

void SToolbar::Construct(const FArguments &InArgs)
{
    const EToolbarPosition Position = InArgs._Position.Get(EToolbarPosition::Top);
    const bool bShowDivider = InArgs._ShowDivider.Get(true);
    const FMargin Padding = InArgs._Padding.Get(FMargin(FConvaiStyle::Get().GetFloat("Convai.Spacing.content")));

    TSharedRef<SWidget> Divider = SNullWidget::NullWidget;

    if (bShowDivider)
    {
        const FSlateColorBrush DividerBrush(FConvaiStyle::RequireColor("Convai.Color.divider.general"));

        switch (Position)
        {
        case EToolbarPosition::Top:
            Divider = SNew(SSeparator)
                          .Orientation(Orient_Horizontal)
                          .Thickness(FConvaiStyle::Get().GetFloat("Convai.Size.separatorThickness"))
                          .SeparatorImage(&DividerBrush);
            break;

        case EToolbarPosition::Bottom:
            Divider = SNew(SSeparator)
                          .Orientation(Orient_Horizontal)
                          .Thickness(FConvaiStyle::Get().GetFloat("Convai.Size.separatorThickness"))
                          .SeparatorImage(&DividerBrush);
            break;

        case EToolbarPosition::Left:
            Divider = SNew(SSeparator)
                          .Orientation(Orient_Vertical)
                          .Thickness(FConvaiStyle::Get().GetFloat("Convai.Size.separatorThickness"))
                          .SeparatorImage(&DividerBrush);
            break;

        case EToolbarPosition::Right:
            Divider = SNew(SSeparator)
                          .Orientation(Orient_Vertical)
                          .Thickness(FConvaiStyle::Get().GetFloat("Convai.Size.separatorThickness"))
                          .SeparatorImage(&DividerBrush);
            break;
        }
    }

    TSharedRef<SWidget> ToolbarWidget = SNullWidget::NullWidget;

    const FLinearColor BackgroundColor = FConvaiStyle::RequireColor("Convai.Color.ToolbarBackground");

    switch (Position)
    {
    case EToolbarPosition::Top:
        ToolbarWidget = SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[SNew(SBox).Padding(Padding).HAlign(HAlign_Fill)[InArgs._Content.Widget]] + SVerticalBox::Slot().AutoHeight()[Divider];
        break;

    case EToolbarPosition::Bottom:
        ToolbarWidget = SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[Divider] + SVerticalBox::Slot().AutoHeight()[SNew(SBox).Padding(Padding).HAlign(HAlign_Fill)[InArgs._Content.Widget]];
        break;

    case EToolbarPosition::Left:
        ToolbarWidget = SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).Padding(Padding).VAlign(VAlign_Fill)[InArgs._Content.Widget]] + SHorizontalBox::Slot().AutoWidth()[Divider];
        break;

    case EToolbarPosition::Right:
        ToolbarWidget = SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth()[Divider] + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).Padding(Padding).VAlign(VAlign_Fill)[InArgs._Content.Widget]];
        break;
    }

    ChildSlot
        [SNew(SBorder)
             .BorderImage(new FSlateColorBrush(BackgroundColor))
             .Padding(0)
                 [ToolbarWidget]];

    SetToolTipText(LOCTEXT("ToolbarA11yText", "Toolbar"));
}

#undef LOCTEXT_NAMESPACE
