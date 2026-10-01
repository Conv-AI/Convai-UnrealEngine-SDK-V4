/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SCircularAvatar.h
 *
 * Circular avatar widget displaying user initials with colored backgrounds.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/SlateColor.h"

/** Circular avatar widget displaying user initials with colored backgrounds. */
class CONVAIEDITOR_API SCircularAvatar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCircularAvatar)
        : _Username(TEXT("")), _Size(32.0f), _FontSize(14.0f)
    {
    }
    SLATE_ATTRIBUTE(FString, Username)
    SLATE_ARGUMENT(float, Size)
    SLATE_ARGUMENT(float, FontSize)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);
    void SetUsername(const FString &InUsername);
    FString GetUsername() const { return Username; }

private:
    TAttribute<FString> UsernameAttribute;
    FString Username;
    float Size;
    float FontSize;
    float VerticalOffset;
    FLinearColor BackgroundColor;
    FString Initials;
    TSharedPtr<struct FSlateRoundedBoxBrush> CachedCircleBrush;

    void UpdateAvatarProperties();
    FString GetCurrentUsername() const;
    void UpdateCircleBrush();
};
