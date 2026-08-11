/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiApiKeyInputBox.cpp
 *
 * Implementation of the API key input box widget.
 */

#include "UI/Widgets/SConvaiApiKeyInputBox.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "UI/Widgets/SRoundedBox.h"
#include "Utility/ConvaiConstants.h"

void SConvaiApiKeyInputBox::Construct(const FArguments &InArgs)
{
	const FSlateFontInfo ValueFont = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountValue"));
	const float BorderRadius = ConvaiEditor::Constants::Layout::Radius::StandardCard;
	const float BorderThickness = ConvaiEditor::Constants::Layout::Components::StandardCard::BorderThickness;
	const FLinearColor BoxBackgroundColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.account.boxBackground"));
	const FLinearColor BorderColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.account.boxBorder"));
	const FLinearColor InputBackgroundColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.component.account.keyBackground"));
	const FLinearColor TextColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.text.primary"));
	const FLinearColor IconColor = FConvaiStyle::RequireColor(TEXT("Convai.Color.icon.base"));
	const float PaddingHorizontal = ConvaiEditor::Constants::Layout::Spacing::AccountBox::Horizontal;
	const float PaddingVertical = ConvaiEditor::Constants::Layout::Spacing::AccountBox::VerticalOuter * 0.7f;
	const float IconPadding = ConvaiEditor::Constants::Layout::Spacing::ApiKeyIconUniformPadding;

	static FEditableTextBoxStyle EditableTextBoxStyle;
	static bool bStyleInitialized = false;
	if (!bStyleInitialized)
	{
		EditableTextBoxStyle
			.SetBackgroundImageNormal(FSlateColorBrush(InputBackgroundColor))
			.SetBackgroundImageHovered(FSlateColorBrush(InputBackgroundColor))
			.SetBackgroundImageFocused(FSlateColorBrush(InputBackgroundColor))
			.SetBackgroundImageReadOnly(FSlateColorBrush(InputBackgroundColor))
			.SetForegroundColor(TextColor);
		bStyleInitialized = true;
	}

	ChildSlot
		[SNew(SRoundedBox)
			 .BorderRadius(BorderRadius)
			 .BorderThickness(BorderThickness)
			 .BackgroundColor(BoxBackgroundColor)
			 .BorderColor(BorderColor)
				 [SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(FMargin(PaddingHorizontal, PaddingVertical, 4.0f, PaddingVertical))[SAssignNew(EditableTextBox, SEditableTextBox).Text(InArgs._Text).Font(ValueFont).ForegroundColor(TextColor).Style(&EditableTextBoxStyle).IsPassword(InArgs._IsPassword).OnTextChanged(InArgs._OnTextChanged).OnTextCommitted(InArgs._OnTextCommitted).MinDesiredWidth(400.0f).HintText(InArgs._HintText).IsEnabled(InArgs._IsEnabled)] + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.0f, PaddingVertical + 1, PaddingHorizontal, PaddingVertical))[SNew(SButton).OnClicked(InArgs._OnTogglePassword).ButtonStyle(FCoreStyle::Get(), "NoBorder").ContentPadding(IconPadding).ToolTipText(NSLOCTEXT("ConvaiEditor", "ToggleApiKeyVisibility", "Toggle API key visibility"))[SNew(SImage).Image_Lambda([IsPassword = InArgs._IsPassword]()
																																																																																																																																																																																																																													 { return IsPassword.Get() ? FConvaiStyle::Get().GetBrush(TEXT("Convai.Icon.eyeHidden")) : FConvaiStyle::Get().GetBrush(TEXT("Convai.Icon.eyeVisible")); })
																																																																																																																																																																																																																							   .ColorAndOpacity(IconColor)]]]];
}
