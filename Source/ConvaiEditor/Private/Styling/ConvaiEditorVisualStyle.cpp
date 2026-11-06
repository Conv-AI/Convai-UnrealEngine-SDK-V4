// Copyright Convai Inc. All Rights Reserved.
#include "Styling/ConvaiEditorVisualStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"

namespace ConvaiEditorVisual
{
FLinearColor FromHex(const TCHAR* Hex)
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
}

const FLinearColor& Window()
{
	static const FLinearColor Value = FromHex(TEXT("141A20"));
	return Value;
}

const FLinearColor& Header()
{
	static const FLinearColor Value = FromHex(TEXT("11181E"));
	return Value;
}

const FLinearColor& Toolbar()
{
	static const FLinearColor Value = FromHex(TEXT("1B242C"));
	return Value;
}

const FLinearColor& Panel()
{
	static const FLinearColor Value = FromHex(TEXT("202A32"));
	return Value;
}

const FLinearColor& Row()
{
	static const FLinearColor Value = FromHex(TEXT("1B242B"));
	return Value;
}

const FLinearColor& RowHover()
{
	static const FLinearColor Value = FromHex(TEXT("25323B"));
	return Value;
}

const FLinearColor& SelectedRow()
{
	static const FLinearColor Value = FromHex(TEXT("21444E"));
	return Value;
}

const FLinearColor& Preview()
{
	static const FLinearColor Value = FromHex(TEXT("10171C"));
	return Value;
}

const FLinearColor& Border()
{
	static const FLinearColor Value = FromHex(TEXT("33414A"));
	return Value;
}

const FLinearColor& PrimaryText()
{
	static const FLinearColor Value = FromHex(TEXT("EEF2F4"));
	return Value;
}

const FLinearColor& SecondaryText()
{
	static const FLinearColor Value = FromHex(TEXT("AAB5BC"));
	return Value;
}

const FLinearColor& MutedText()
{
	static const FLinearColor Value = FromHex(TEXT("8D999F"));
	return Value;
}

const FLinearColor& Accent()
{
	static const FLinearColor Value = FromHex(TEXT("19B8AA"));
	return Value;
}

const FLinearColor& PrimaryAction()
{
	// A darker Convai-adjacent teal keeps the brand family while preserving
	// readable white labels. The Editor theme's brighter green is reserved for
	// small accents because white text on it is too low-contrast for dense tools.
	static const FLinearColor Value = FromHex(TEXT("0C735F"));
	return Value;
}

const FLinearColor& PrimaryActionHover()
{
	static const FLinearColor Value = FromHex(TEXT("0F826C"));
	return Value;
}

const FLinearColor& PrimaryActionPressed()
{
	static const FLinearColor Value = FromHex(TEXT("095F50"));
	return Value;
}

const FLinearColor& DisabledAction()
{
	static const FLinearColor Value = FromHex(TEXT("2A343B"));
	return Value;
}

const FLinearColor& DisabledText()
{
	static const FLinearColor Value = FromHex(TEXT("89949B"));
	return Value;
}

const FButtonStyle& PrimaryButtonStyle()
{
	static const FButtonStyle Style = FButtonStyle()
		.SetNormal(FSlateRoundedBoxBrush(PrimaryAction(), 4.0f))
		.SetHovered(FSlateRoundedBoxBrush(PrimaryActionHover(), 4.0f))
		.SetPressed(FSlateRoundedBoxBrush(PrimaryActionPressed(), 4.0f))
		.SetDisabled(FSlateRoundedBoxBrush(DisabledAction(), 4.0f, Border(), 1.0f))
		.SetNormalForeground(FSlateColor(PrimaryText()))
		.SetHoveredForeground(FSlateColor(PrimaryText()))
		.SetPressedForeground(FSlateColor(PrimaryText()))
		.SetDisabledForeground(FSlateColor(DisabledText()))
		.SetNormalPadding(FMargin(0.0f))
		.SetPressedPadding(FMargin(0.0f));
	return Style;
}

const FButtonStyle& SecondaryButtonStyle()
{
	static const FButtonStyle Style = FButtonStyle()
		.SetNormal(FSlateRoundedBoxBrush(Row(), 4.0f, Border(), 1.0f))
		.SetHovered(FSlateRoundedBoxBrush(RowHover(), 4.0f, Accent(), 1.0f))
		.SetPressed(FSlateRoundedBoxBrush(SelectedRow(), 4.0f, Accent(), 1.0f))
		.SetDisabled(FSlateRoundedBoxBrush(DisabledAction(), 4.0f, Border(), 1.0f))
		.SetNormalForeground(FSlateColor(PrimaryText()))
		.SetHoveredForeground(FSlateColor(PrimaryText()))
		.SetPressedForeground(FSlateColor(PrimaryText()))
		.SetDisabledForeground(FSlateColor(DisabledText()))
		.SetNormalPadding(FMargin(0.0f))
		.SetPressedPadding(FMargin(0.0f));
	return Style;
}

const FButtonStyle& QuietDangerButtonStyle()
{
    static const FLinearColor Danger = FromHex(TEXT("D59595"));
    static const FLinearColor Hover = FromHex(TEXT("E9ABAB"));
    static const FButtonStyle Style = FButtonStyle()
        .SetNormal(FSlateRoundedBoxBrush(FLinearColor::Transparent, 4.0f))
        .SetHovered(FSlateRoundedBoxBrush(FromHex(TEXT("35282D")), 4.0f, FromHex(TEXT("69434A")), 1.0f))
        .SetPressed(FSlateRoundedBoxBrush(FromHex(TEXT("422B31")), 4.0f, Danger, 1.0f))
        .SetDisabled(FSlateRoundedBoxBrush(FLinearColor::Transparent, 4.0f))
        .SetNormalForeground(FSlateColor(Danger))
        .SetHoveredForeground(FSlateColor(Hover))
        .SetPressedForeground(FSlateColor(Hover))
        .SetDisabledForeground(FSlateColor(DisabledText()))
        .SetNormalPadding(FMargin(0.0f))
        .SetPressedPadding(FMargin(0.0f));
    return Style;
}
}
