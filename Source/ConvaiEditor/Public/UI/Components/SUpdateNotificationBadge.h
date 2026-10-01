/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SUpdateNotificationBadge.h
 *
 * Update notification badge widget for the settings button.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/SlateTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

/** Update notification badge widget with pulsing animation. */
class CONVAIEDITOR_API SUpdateNotificationBadge : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUpdateNotificationBadge)
		: _BadgeColor(FLinearColor(0.2f, 0.8f, 0.2f, 1.0f)),
		  _BadgeSize(8.0f), _bEnableAnimation(true)
	{
	}
	SLATE_ARGUMENT(FLinearColor, BadgeColor)
	SLATE_ARGUMENT(float, BadgeSize)
	SLATE_ARGUMENT(bool, bEnableAnimation)
	SLATE_ATTRIBUTE(FText, ToolTipText)
	SLATE_END_ARGS()

	void Construct(const FArguments &InArgs);
	void Show(bool bAnimated = true);
	void Hide();
	bool IsVisible() const { return GetVisibility() == EVisibility::Visible; }

private:
	FLinearColor BadgeColor;
	float BadgeSize;
	bool bEnableAnimation;
	double AnimationStartTime;

	FSlateColor GetAnimatedColor() const;
};
