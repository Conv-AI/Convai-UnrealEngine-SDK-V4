// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"

/** Shared desktop tool palette and controls, extracted unchanged from Scene Auto Tagger. */
namespace ConvaiEditorVisual
{
CONVAIEDITOR_API FLinearColor FromHex(const TCHAR* Hex);
CONVAIEDITOR_API const FLinearColor& Window();
CONVAIEDITOR_API const FLinearColor& Header();
CONVAIEDITOR_API const FLinearColor& Toolbar();
CONVAIEDITOR_API const FLinearColor& Panel();
CONVAIEDITOR_API const FLinearColor& Row();
CONVAIEDITOR_API const FLinearColor& RowHover();
CONVAIEDITOR_API const FLinearColor& SelectedRow();
CONVAIEDITOR_API const FLinearColor& Preview();
CONVAIEDITOR_API const FLinearColor& Border();
CONVAIEDITOR_API const FLinearColor& PrimaryText();
CONVAIEDITOR_API const FLinearColor& SecondaryText();
CONVAIEDITOR_API const FLinearColor& MutedText();
CONVAIEDITOR_API const FLinearColor& Accent();
CONVAIEDITOR_API const FLinearColor& PrimaryAction();
CONVAIEDITOR_API const FLinearColor& PrimaryActionHover();
CONVAIEDITOR_API const FLinearColor& PrimaryActionPressed();
CONVAIEDITOR_API const FLinearColor& DisabledAction();
CONVAIEDITOR_API const FLinearColor& DisabledText();
CONVAIEDITOR_API const FButtonStyle& PrimaryButtonStyle();
CONVAIEDITOR_API const FButtonStyle& SecondaryButtonStyle();
CONVAIEDITOR_API const FButtonStyle& QuietDangerButtonStyle();
}
