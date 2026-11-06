// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ConvaiGazeCursorWidget.generated.h"

/**
 * Tiny center-of-screen reticle drawn by UConvaiPlayerComponent while gaze tracking is active.
 *
 * Two states:
 *   - Idle:   gaze is not over a Convai object. Drawn with IdleColor (alpha 0 by default — i.e.
 *             invisible — so the cursor fades out completely when looking at nothing).
 *   - Active: gaze is on a Convai object. Drawn with ActiveColor (white by default).
 *
 * Pure C++ — uses FCoreStyle's WhiteBrush so no texture asset ships with the plugin. Override
 * GazeCursorWidgetClass on UConvaiPlayerComponent to swap in a Blueprint widget for fancier
 * visuals; SetGazeActive will still be called on the override at runtime.
 */
UCLASS(Blueprintable, ClassGroup = (Convai))
class CONVAI_API UConvaiGazeCursorWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UConvaiGazeCursorWidget(const FObjectInitializer& ObjectInitializer);

	/** Called by the player component when gaze enters or leaves a Convai object. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Gaze")
	void SetGazeActive(bool bInGazeActive);

	/** Read current gaze state — useful for BP subclasses that want to read it in OnGazeStateChanged. */
	UFUNCTION(BlueprintPure, Category = "Convai|Gaze")
	bool IsGazeActive() const { return bGazeActive; }

	/**
	 * BlueprintImplementableEvent fired on every state change. Lets BP subclasses run custom
	 * transitions (e.g. swap an Image's brush, play an animation) without rewriting NativePaint.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Convai|Gaze")
	void OnGazeStateChanged(bool bInGazeActive);

	// ── Visual config ────────────────────────────────────────────────

	/** Color of the dot while gaze is on a Convai object. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze")
	FLinearColor ActiveColor = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);

	/** Color of the dot while gaze is on nothing. Default alpha 0 means "invisible when idle". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze")
	FLinearColor IdleColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.0f);

	/** Edge length of the cursor square, in unscaled Slate units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze", meta = (ClampMin = "1.0"))
	float DotSize = 6.0f;

	/** Seconds to fade from current state into Active. 0 = snap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze", AdvancedDisplay, meta = (ClampMin = "0.0"))
	float FadeInTime = 0.1f;

	/** Seconds to fade from current state into Idle. 0 = snap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Gaze", AdvancedDisplay, meta = (ClampMin = "0.0"))
	float FadeOutTime = 0.25f;

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	bool bGazeActive = false;
	float StateAlpha = 0.0f; // 0 = fully idle, 1 = fully active. Interpolated by NativeTick.
};
