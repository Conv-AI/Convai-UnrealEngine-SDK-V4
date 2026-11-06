/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiLoadingScreen.h
 *
 * Translucent overlay with throbber and text for loading states.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/** Translucent overlay with throbber and text for loading states. */
class CONVAIEDITOR_API SConvaiLoadingScreen : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SConvaiLoadingScreen) {}
    SLATE_ATTRIBUTE(FText, Message)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);
};