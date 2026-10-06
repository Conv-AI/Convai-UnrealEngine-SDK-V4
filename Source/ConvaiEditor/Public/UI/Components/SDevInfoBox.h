/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SDevInfoBox.h
 *
 * Widget displaying development or coming soon information boxes.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Widget displaying development or coming soon information boxes.
 */
class CONVAIEDITOR_API SDevInfoBox : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SDevInfoBox)
        : _Emoji(TEXT("🚧")), _InfoText(NSLOCTEXT("DevInfoBox", "DefaultText", "Coming Soon")), _bWrapText(true), _WrapTextAt(0.0f)
    {
    }
    /** The emoji to display (default: 🚧) */
    SLATE_ARGUMENT(FString, Emoji)

    /** The informational text to display */
    SLATE_ARGUMENT(FText, InfoText)

    /** Whether the text should auto wrap */
    SLATE_ARGUMENT(bool, bWrapText)

    /** Explicit width to wrap text at (0 = auto) */
    SLATE_ARGUMENT(float, WrapTextAt)
    SLATE_END_ARGS()

    /** Constructs this widget with InArgs */
    void Construct(const FArguments &InArgs);
};
