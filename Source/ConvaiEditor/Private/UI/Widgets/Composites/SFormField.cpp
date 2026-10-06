/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SFormField.cpp
 *
 * Implementation of the form field composite widget.
 */

#include "UI/Widgets/Composites/SFormField.h"
#include "UI/Utility/ConvaiWidgetFactory.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ConvaiEditorFormField"

void SFormField::Construct(const FArguments &InArgs)
{
    const FText Label = InArgs._Label.Get(FText::GetEmpty());
    const FText HelperText = InArgs._HelperText.Get(FText::GetEmpty());
    const FText ErrorText = InArgs._ErrorText.Get(FText::GetEmpty());
    const bool bIsRequired = InArgs._IsRequired.Get(false);
    const float LabelMinWidth = InArgs._LabelMinWidth.Get(160.0f);
    const FMargin Padding = InArgs._Padding.Get(FMargin(0, 0, 0, 16));
    const EHorizontalAlignment LabelPosition = InArgs._LabelPosition.Get(EHorizontalAlignment::HAlign_Left);

    TSharedRef<SWidget> LabelWidget = SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth()[SNew(STextBlock).TextStyle(FConvaiStyle::Get(), "Convai.Text.Body").Text(Label).AutoWrapText(true)] + SHorizontalBox::Slot().AutoWidth().Padding(2, 0, 0, 0)[SNew(STextBlock).Text(LOCTEXT("RequiredIndicator", "*")).ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.Error")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 12)).Visibility(bIsRequired ? EVisibility::Visible : EVisibility::Collapsed)];

    HelperTextBlock = SNew(STextBlock)
                          .Text(HelperText)
                          .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                          .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.TextSecondary"))
                          .Visibility(HelperText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);

    ErrorTextBlock = SNew(STextBlock)
                         .Text(ErrorText)
                         .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                         .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.Error"))
                         .Visibility(ErrorText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);

    TSharedRef<SWidget> FormLayout = SNullWidget::NullWidget;

    if (LabelPosition == EHorizontalAlignment::HAlign_Left)
    {
        FormLayout = SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).HAlign(HAlign_Left)[SNew(SBox).MinDesiredWidth(LabelMinWidth)[LabelWidget]] + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[InArgs._Content.Widget]] + SVerticalBox::Slot().AutoHeight().Padding(FMargin(LabelMinWidth, 4, 0, 0))[SNew(SBox).Visibility(HelperText.IsEmpty() && ErrorText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[HelperTextBlock.ToSharedRef()] + SVerticalBox::Slot().AutoHeight()[ErrorTextBlock.ToSharedRef()]]];
    }
    else
    {
        FormLayout = SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)[LabelWidget] + SVerticalBox::Slot().AutoHeight()[InArgs._Content.Widget] + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)[SNew(SBox).Visibility(HelperText.IsEmpty() && ErrorText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[HelperTextBlock.ToSharedRef()] + SVerticalBox::Slot().AutoHeight()[ErrorTextBlock.ToSharedRef()]]];
    }

    ChildSlot
        .Padding(Padding)
            [FormLayout];

    SetToolTipText(Label);
}

void SFormField::SetErrorText(const FText &InErrorText)
{
    if (ErrorTextBlock.IsValid())
    {
        ErrorTextBlock->SetText(InErrorText);
        ErrorTextBlock->SetVisibility(InErrorText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
    }
}

void SFormField::SetHelperText(const FText &InHelperText)
{
    if (HelperTextBlock.IsValid())
    {
        HelperTextBlock->SetText(InHelperText);
        HelperTextBlock->SetVisibility(InHelperText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
    }
}

#undef LOCTEXT_NAMESPACE
