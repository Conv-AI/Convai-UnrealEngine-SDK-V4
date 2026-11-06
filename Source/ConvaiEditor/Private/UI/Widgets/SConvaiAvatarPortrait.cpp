// Copyright Convai Inc. All Rights Reserved.
#include "UI/Widgets/SConvaiAvatarPortrait.h"

#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

void SConvaiAvatarPortrait::Construct(const FArguments& InArgs)
{
	Image = InArgs._Image;
	IsLoading = InArgs._IsLoading;
	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(SThrobber).NumPieces(3)
			.Visibility(this, &SConvaiAvatarPortrait::LoadingVisibility)
		]
		+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(InArgs._FallbackText).Font(InArgs._FallbackFont)
			.ColorAndOpacity(InArgs._FallbackColor)
			.Visibility(this, &SConvaiAvatarPortrait::FallbackVisibility)
		]
		+ SOverlay::Slot()
		[
			SNew(SScaleBox).Stretch(InArgs._ImageStretch).StretchDirection(EStretchDirection::Both)
			.HAlign(HAlign_Center).VAlign(InArgs._ImageVAlign)
			.Visibility(this, &SConvaiAvatarPortrait::ImageVisibility)
			[SNew(SImage).Image(this, &SConvaiAvatarPortrait::GetImage)]
		]
	];
}

const FSlateBrush* SConvaiAvatarPortrait::GetImage() const { return Image.Get(); }
EVisibility SConvaiAvatarPortrait::ImageVisibility() const { return GetImage() ? EVisibility::Visible : EVisibility::Collapsed; }
EVisibility SConvaiAvatarPortrait::LoadingVisibility() const
{
	const FSlateBrush* Brush = GetImage();
	return !Brush && IsLoading.Get() ? EVisibility::Visible : EVisibility::Collapsed;
}
EVisibility SConvaiAvatarPortrait::FallbackVisibility() const
{
	const FSlateBrush* Brush = GetImage();
	return !Brush && !IsLoading.Get() ? EVisibility::Visible : EVisibility::Collapsed;
}
