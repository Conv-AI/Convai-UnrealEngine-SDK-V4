/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SSettingsPage.h
 *
 * Settings page for the Convai Editor.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Pages/SBasePage.h"

/**
 * Settings page for the Convai Editor.
 */
class SSettingsPage : public SBasePage
{
public:
    SLATE_BEGIN_ARGS(SSettingsPage)
    {
    }
    SLATE_END_ARGS()

    /** Constructs this widget */
    void Construct(const FArguments &InArgs);
};
