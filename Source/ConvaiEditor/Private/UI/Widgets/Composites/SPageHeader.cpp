/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SPageHeader.cpp
 *
 * Implementation of the page header composite widget.
 */

#include "UI/Widgets/Composites/SPageHeader.h"
#include "UI/Utility/ConvaiWidgetFactory.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"

#define LOCTEXT_NAMESPACE "ConvaiEditorPageHeader"

void SPageHeader::Construct(const FArguments &InArgs)
{
    const FText Title = InArgs._Title.Get(FText::GetEmpty());
    const FText SubTitle = InArgs._SubTitle.Get(FText::GetEmpty());
    const bool bShowDivider = InArgs._ShowDivider.Get(true);
    const FMargin Padding = InArgs._Padding.Get(FMargin(0, 0, 0, 16));

    TSharedPtr<SWidget> ActionsWidget = InArgs._Actions.Widget;

    TSharedRef<SWidget> TitleWidget = SNew(STextBlock)
                                          .TextStyle(FConvaiStyle::Get(), "Convai.Text.Heading")
                                          .Text(Title)
                                          .AutoWrapText(true);

    TSharedRef<SWidget> SubTitleWidget = SNew(SBox)
                                             .Visibility(SubTitle.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
                                             .Padding(FMargin(0, FConvaiStyle::Get().GetFloat("Convai.Spacing.spaceBelowTitle"), 0, 0))
                                                 [SNew(STextBlock)
                                                      .TextStyle(FConvaiStyle::Get(), "Convai.Text.Body")
                                                      .Text(SubTitle)
                                                      .AutoWrapText(true)];

    TSharedRef<SWidget> DividerWidget = SNew(SBox)
                                            .Visibility(bShowDivider ? EVisibility::Visible : EVisibility::Collapsed)
                                            .Padding(FMargin(0, FConvaiStyle::Get().GetFloat("Convai.Spacing.content"), 0, 0))
                                                [SNew(SSeparator)
                                                     .Thickness(FConvaiStyle::Get().GetFloat("Convai.Size.separatorThickness"))
                                                     .SeparatorImage(new FSlateColorBrush(FConvaiStyle::RequireColor("Convai.Color.divider.general")))];

    TSharedRef<SWidget> ActionsBox = SNew(SBox)
                                         .HAlign(HAlign_Right)
                                         .VAlign(VAlign_Center)
                                         .Visibility(ActionsWidget.IsValid() ? EVisibility::Visible : EVisibility::Collapsed)
                                             [ActionsWidget.IsValid() ? ActionsWidget.ToSharedRef() : SNullWidget::NullWidget];

    TSharedRef<SWidget> HeaderContent = SNew(SHorizontalBox) + SHorizontalBox::Slot().VAlign(VAlign_Center).AutoWidth()[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[TitleWidget] + SVerticalBox::Slot().AutoHeight()[SubTitleWidget]] + SHorizontalBox::Slot().FillWidth(1.0f)[SNew(SSpacer)] + SHorizontalBox::Slot().AutoWidth()[ActionsBox];

    ChildSlot
        [SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(Padding)[HeaderContent] + SVerticalBox::Slot().AutoHeight()[DividerWidget]];

    SetToolTipText(LOCTEXT("PageHeaderA11yText", "Page Header"));
}

#undef LOCTEXT_NAMESPACE
