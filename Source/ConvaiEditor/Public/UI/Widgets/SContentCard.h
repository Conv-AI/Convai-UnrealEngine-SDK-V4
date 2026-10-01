/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SContentCard.h
 *
 * Specialized card widget for displaying title, description, and optional image.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Widgets/SCard.h"
#include "Styling/SlateTypes.h"

/** Specialized card widget for displaying title, description, and optional image. */
class CONVAIEDITOR_API SContentCard : public SCard
{
public:
    SLATE_BEGIN_ARGS(SContentCard)
        : _Title(FText::GetEmpty()), _Description(FText::GetEmpty()), _TitlePadding(FMargin(0, 0, 0, 4)), _ContentImage(nullptr), _ImageSize(FVector2D(64, 64))
    {
    }
    SLATE_ARGUMENT(FText, Title)
    SLATE_ARGUMENT(FText, Description)
    SLATE_ARGUMENT(FMargin, TitlePadding)
    SLATE_ARGUMENT(const FSlateBrush *, ContentImage)
    SLATE_ARGUMENT(FVector2D, ImageSize)
    SLATE_EVENT(FOnClicked, OnClicked)
    SLATE_ATTRIBUTE(float, BorderRadius)
    SLATE_ATTRIBUTE(FMargin, ContentPadding)
    SLATE_ATTRIBUTE(FLinearColor, BackgroundColor)
    SLATE_ATTRIBUTE(FLinearColor, BorderColor)
    SLATE_ATTRIBUTE(float, BorderThickness)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);

private:
    TSharedRef<SWidget> BuildCardContent(const FArguments &InArgs);
};
