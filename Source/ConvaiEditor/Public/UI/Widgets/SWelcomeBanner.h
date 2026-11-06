/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SWelcomeBanner.h
 *
 * Banner widget displaying Convai welcome graphic with optional gradient overlay.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/SlateBrush.h"

/** Banner widget displaying Convai welcome graphic with optional gradient overlay. */
class CONVAIEDITOR_API SWelcomeBanner : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SWelcomeBanner) {}
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);

private:
    const FSlateBrush *BannerBrush = nullptr;
    const FSlateBrush *GradientBrush = nullptr;
};