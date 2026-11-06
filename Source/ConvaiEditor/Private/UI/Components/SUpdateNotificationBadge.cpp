/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SUpdateNotificationBadge.cpp
 *
 * Implementation of the update notification badge.
 */

#include "UI/Components/SUpdateNotificationBadge.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/StyleColors.h"
#include "Styling/CoreStyle.h"

void SUpdateNotificationBadge::Construct(const FArguments &InArgs)
{
	BadgeColor = InArgs._BadgeColor;
	BadgeSize = InArgs._BadgeSize;
	bEnableAnimation = InArgs._bEnableAnimation;
	AnimationStartTime = FPlatformTime::Seconds();

	SetVisibility(EVisibility::Collapsed);
	SetToolTipText(InArgs._ToolTipText);

	ChildSlot
		[SNew(SBox)
			 .WidthOverride(BadgeSize)
			 .HeightOverride(BadgeSize)
				 [SNew(SImage)
					  .Image(FCoreStyle::Get().GetBrush("Icons.FilledCircle"))
					  .ColorAndOpacity(TAttribute<FSlateColor>::Create(
						  TAttribute<FSlateColor>::FGetter::CreateSP(this, &SUpdateNotificationBadge::GetAnimatedColor)))]];
}

void SUpdateNotificationBadge::Show(bool bAnimated)
{
	SetVisibility(EVisibility::Visible);

	if (bAnimated && bEnableAnimation)
	{
		AnimationStartTime = FPlatformTime::Seconds();
	}
}

void SUpdateNotificationBadge::Hide()
{
	SetVisibility(EVisibility::Collapsed);
}

FSlateColor SUpdateNotificationBadge::GetAnimatedColor() const
{
	if (!bEnableAnimation)
	{
		return FSlateColor(BadgeColor);
	}

	double CurrentTime = FPlatformTime::Seconds();
	double ElapsedTime = CurrentTime - AnimationStartTime;
	float Cycle = FMath::Fmod((float)ElapsedTime, 1.0f);

	float Alpha = (FMath::Sin(Cycle * PI * 2.0f) + 1.0f) * 0.5f;

	float MinAlpha = 0.5f;
	float MaxAlpha = 1.0f;
	float AnimatedAlpha = FMath::Lerp(MinAlpha, MaxAlpha, Alpha);

	FLinearColor AnimatedColor = BadgeColor;
	AnimatedColor.A = AnimatedAlpha;

	return FSlateColor(AnimatedColor);
}
