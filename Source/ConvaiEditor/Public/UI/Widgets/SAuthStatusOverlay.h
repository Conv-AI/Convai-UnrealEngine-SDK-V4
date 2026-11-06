/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SAuthStatusOverlay.h
 *
 * Semi-transparent overlay with throbber and message for OAuth flow.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/** Semi-transparent overlay with throbber and message for OAuth flow. */
class CONVAIEDITOR_API SAuthStatusOverlay : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAuthStatusOverlay) {}
    SLATE_ATTRIBUTE(FText, Message)
    SLATE_ATTRIBUTE(FText, SubMessage)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);

    /** Update displayed texts */
    void SetStatus(const FText &NewMessage, const FText &NewSubMessage = FText::GetEmpty());

    void SetMessage(const FText &NewMessage) { SetStatus(NewMessage); }

private:
    TAttribute<FText> CurrentMessage;
    TAttribute<FText> CurrentSubMessage;
};