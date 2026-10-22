// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Gaze/ConvaiGazeCursorWidget.h"

#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"

UConvaiGazeCursorWidget::UConvaiGazeCursorWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// We want NativeTick to drive the fade interpolation; UUserWidget ticks by default.
	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UConvaiGazeCursorWidget::SetGazeActive(bool bInGazeActive)
{
	if (bGazeActive == bInGazeActive)
	{
		return;
	}
	bGazeActive = bInGazeActive;
	OnGazeStateChanged(bInGazeActive);
}

void UConvaiGazeCursorWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	const float Target = bGazeActive ? 1.0f : 0.0f;
	if (FMath::IsNearlyEqual(StateAlpha, Target, KINDA_SMALL_NUMBER))
	{
		StateAlpha = Target;
		return;
	}

	const float Duration = bGazeActive ? FadeInTime : FadeOutTime;
	if (Duration <= 0.0f)
	{
		StateAlpha = Target;
		return;
	}

	const float Step = InDeltaTime / Duration;
	if (Target > StateAlpha)
	{
		StateAlpha = FMath::Min(StateAlpha + Step, Target);
	}
	else
	{
		StateAlpha = FMath::Max(StateAlpha - Step, Target);
	}
}

int32 UConvaiGazeCursorWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	int32 NewLayerId = Super::NativePaint(Args, AllottedGeometry, MyCullingRect,
		OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);

	// Interpolated tint between idle and active. StateAlpha is driven by NativeTick.
	const FLinearColor Blended = FMath::Lerp(IdleColor, ActiveColor, FMath::Clamp(StateAlpha, 0.0f, 1.0f));
	if (Blended.A <= KINDA_SMALL_NUMBER)
	{
		return NewLayerId;
	}

	const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
	if (!WhiteBrush)
	{
		return NewLayerId;
	}

	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
	const FVector2D Size(DotSize, DotSize);
	const FVector2D Pos = (LocalSize - Size) * 0.5f;

	FSlateDrawElement::MakeBox(
		OutDrawElements,
		NewLayerId,
		AllottedGeometry.ToPaintGeometry(Pos, Size),
		WhiteBrush,
		ESlateDrawEffect::None,
		Blended);

	return NewLayerId + 1;
}
