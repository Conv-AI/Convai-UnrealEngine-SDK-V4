/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SToolbar.h
 *
 * Toolbar composite with flexible positioning and consistent styling.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/** Toolbar positioning options. */
enum class EToolbarPosition : uint8
{
    Top,
    Bottom,
    Left,
    Right
};

/** Toolbar composite with flexible positioning and consistent styling. */
class CONVAIEDITOR_API SToolbar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SToolbar)
        : _Position(EToolbarPosition::Top), _ShowDivider(true), _Padding(FMargin(8))
    {
    }
    SLATE_ATTRIBUTE(EToolbarPosition, Position)
    SLATE_ATTRIBUTE(bool, ShowDivider)
    SLATE_ATTRIBUTE(FMargin, Padding)
    SLATE_DEFAULT_SLOT(FArguments, Content)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);
};
