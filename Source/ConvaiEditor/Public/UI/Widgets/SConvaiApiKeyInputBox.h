/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiApiKeyInputBox.h
 *
 * API Key input box with password toggle and Convai styling.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

/** API Key input box with password toggle and Convai styling. */
class CONVAIEDITOR_API SConvaiApiKeyInputBox : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SConvaiApiKeyInputBox)
		: _Text(FText::GetEmpty()), _IsPassword(true), _IsEnabled(true), _HintText(FText::GetEmpty())
	{
	}
	SLATE_ATTRIBUTE(FText, Text)
	SLATE_EVENT(FOnTextChanged, OnTextChanged)
	SLATE_EVENT(FOnTextCommitted, OnTextCommitted)
	SLATE_ATTRIBUTE(bool, IsPassword)
	SLATE_EVENT(FOnClicked, OnTogglePassword)
	SLATE_ATTRIBUTE(FText, HintText)
	SLATE_ATTRIBUTE(bool, IsEnabled)
	SLATE_END_ARGS()

	void Construct(const FArguments &InArgs);

private:
	TSharedPtr<class SEditableTextBox> EditableTextBox;
};
