/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SPageHeader.h
 *
 * Page header composite with title, subtitle, and optional actions.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"

/** Page header composite with title, subtitle, and optional actions. */
class CONVAIEDITOR_API SPageHeader : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SPageHeader)
        : _Title(FText::GetEmpty()), _SubTitle(FText::GetEmpty()), _ShowDivider(true), _Padding(FMargin(0, 0, 0, 16))
    {
    }
    SLATE_ATTRIBUTE(FText, Title)
    SLATE_ATTRIBUTE(FText, SubTitle)
    SLATE_ATTRIBUTE(bool, ShowDivider)
    SLATE_ATTRIBUTE(FMargin, Padding)
    SLATE_NAMED_SLOT(FArguments, Actions)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);
};
