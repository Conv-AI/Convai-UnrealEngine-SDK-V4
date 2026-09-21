/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SContentContainer.h
 *
 * Content container widget with title and rounded box styling.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "UI/Widgets/SRoundedBox.h"

/** Content container widget with title and rounded box styling. */
class CONVAIEDITOR_API SContentContainer : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SContentContainer)
        : _Title(FText::GetEmpty()), _TitlePadding(FMargin(0, 0, 0, 8)), _ContentPadding(FMargin(12.0f)), _BorderRadius(8.0f), _BackgroundColor(FLinearColor(0.1f, 0.1f, 0.1f, 1.0f)), _BorderColor(FLinearColor(0.2f, 0.2f, 0.2f, 1.0f)), _BorderThickness(1.0f), _TitleTextStyle(nullptr), _MinWidth(0.0f), _MinHeight(0.0f)
    {
    }
    SLATE_ATTRIBUTE(FText, Title)
    SLATE_ATTRIBUTE(FMargin, TitlePadding)
    SLATE_ATTRIBUTE(FMargin, ContentPadding)
    SLATE_ATTRIBUTE(float, BorderRadius)
    SLATE_ATTRIBUTE(FLinearColor, BackgroundColor)
    SLATE_ATTRIBUTE(FLinearColor, BorderColor)
    SLATE_ATTRIBUTE(float, BorderThickness)
    SLATE_ARGUMENT(const FTextBlockStyle *, TitleTextStyle)
    SLATE_ATTRIBUTE(float, MinWidth)
    SLATE_ATTRIBUTE(float, MinHeight)
    SLATE_DEFAULT_SLOT(FArguments, Content)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);
};
