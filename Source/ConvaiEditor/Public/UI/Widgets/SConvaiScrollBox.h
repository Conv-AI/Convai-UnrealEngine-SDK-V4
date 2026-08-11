/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiScrollBox.h
 *
 * Scroll box with Convai theme styling applied automatically.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Styling/ConvaiStyle.h"
#include "Utility/ConvaiConstants.h"

/** Scroll box with Convai theme styling applied automatically. */
class CONVAIEDITOR_API SConvaiScrollBox : public SScrollBox
{
public:
    SLATE_BEGIN_ARGS(SConvaiScrollBox)
        : _ScrollBarAlwaysVisible(false), _Orientation(Orient_Vertical), _ShowShadow(false), _CustomScrollBarPadding(TOptional<FMargin>())
    {
    }
    SLATE_ARGUMENT(bool, ScrollBarAlwaysVisible)
    SLATE_ARGUMENT(EOrientation, Orientation)
    SLATE_ARGUMENT(bool, ShowShadow)
    SLATE_ARGUMENT(TOptional<FMargin>, CustomScrollBarPadding)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);
};
