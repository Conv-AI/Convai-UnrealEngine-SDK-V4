/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SFormField.h
 *
 * Form field composite with label, help text, and validation states.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"

/** Form field composite with label, help text, and validation states. */
class CONVAIEDITOR_API SFormField : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SFormField)
        : _Label(FText::GetEmpty()), _HelperText(FText::GetEmpty()), _ErrorText(FText::GetEmpty()), _IsRequired(false), _LabelMinWidth(160.0f), _Padding(FMargin(0, 0, 0, 16)), _LabelPosition(EHorizontalAlignment::HAlign_Left)
    {
    }
    SLATE_ATTRIBUTE(FText, Label)
    SLATE_ATTRIBUTE(FText, HelperText)
    SLATE_ATTRIBUTE(FText, ErrorText)
    SLATE_ATTRIBUTE(bool, IsRequired)
    SLATE_ATTRIBUTE(float, LabelMinWidth)
    SLATE_ATTRIBUTE(FMargin, Padding)
    SLATE_ATTRIBUTE(EHorizontalAlignment, LabelPosition)
    SLATE_DEFAULT_SLOT(FArguments, Content)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);
    void SetErrorText(const FText &InErrorText);
    void SetHelperText(const FText &InHelperText);

private:
    TSharedPtr<STextBlock> ErrorTextBlock;
    TSharedPtr<STextBlock> HelperTextBlock;
};
