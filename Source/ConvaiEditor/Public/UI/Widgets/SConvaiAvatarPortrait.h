// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Styling/SlateBrush.h"
#include "Fonts/SlateFontInfo.h"

/** Shared portrait presentation for editor character pickers. Image ownership and requests stay with the caller. */
class CONVAIEDITOR_API SConvaiAvatarPortrait : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SConvaiAvatarPortrait)
		: _Image(nullptr), _IsLoading(false), _FallbackColor(FLinearColor::White), _ImageStretch(EStretch::ScaleToFit), _ImageVAlign(VAlign_Bottom)
	{}
		SLATE_ATTRIBUTE(const FSlateBrush*, Image)
		SLATE_ATTRIBUTE(bool, IsLoading)
		SLATE_ATTRIBUTE(FText, FallbackText)
		SLATE_ARGUMENT(FSlateFontInfo, FallbackFont)
		SLATE_ATTRIBUTE(FSlateColor, FallbackColor)
		SLATE_ARGUMENT(EStretch::Type, ImageStretch)
		SLATE_ARGUMENT(EVerticalAlignment, ImageVAlign)
	SLATE_END_ARGS()
	void Construct(const FArguments& InArgs);

private:
	const FSlateBrush* GetImage() const;
	EVisibility ImageVisibility() const;
	EVisibility LoadingVisibility() const;
	EVisibility FallbackVisibility() const;
	TAttribute<const FSlateBrush*> Image;
	TAttribute<bool> IsLoading;
};
