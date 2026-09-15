// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"

class SEditableTextBox;
class STextBlock;
class UBlueprint;

/** Output of the Create Convai Action Handler dialog. */
struct FConvaiCreateActionHandlerResult
{
	bool    bSubmitted = false;
	bool    bIsEvent   = true;   // true = K2_CustomEvent on the ubergraph; false = new function graph.
	FString ActionName;          // the action's canonical Name — used verbatim as the event/function name.
};

/**
 * Modal dialog that asks the designer whether to create an Event or a Function
 * handler for a Convai action, what to name it, and surfaces a live warning when
 * the chosen name collides with an existing event/function on the Blueprint.
 *
 * When a UConvaiChatbotComponent is present in the Blueprint's SCS, the dialog
 * pulls suggested action names from its EnvironmentData.Actions array into a
 * dropdown — designer can pick one or type freely.
 */
class SConvaiCreateActionHandlerDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SConvaiCreateActionHandlerDialog) {}
		/** The Blueprint whose graph is being edited. Used for name-collision check and to scan SCS for action suggestions. */
		SLATE_ARGUMENT(TWeakObjectPtr<UBlueprint>, Blueprint)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Opens the dialog modally and returns the user's choice. bSubmitted = false when cancelled. */
	static FConvaiCreateActionHandlerResult OpenModal(UBlueprint* Blueprint);

private:
	TWeakObjectPtr<UBlueprint> Blueprint;

	bool    bIsEvent = true;
	FString ActionName;

	TSharedPtr<SEditableTextBox>             ActionNameTextBox;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> SuggestionCombo;
	TSharedPtr<STextBlock>                   ConflictWarning;
	TSharedPtr<SButton>                      SubmitButton;

	TArray<TSharedPtr<FString>> SuggestionItems;
	FConvaiCreateActionHandlerResult Result;
	TWeakPtr<SWindow> OwnerWindow;

	/** Walks the Blueprint's SCS looking for a UConvaiChatbotComponent and reads
	 *  its EnvironmentData.Actions for the names to suggest. Empty when no chatbot
	 *  is present — the dialog still works, just without autocomplete. */
	void GatherSuggestedActionNames();

	/** Live: looks up Blueprint functions and ubergraph events for a name match.
	 *  Returns a non-empty explanation when there's a conflict. */
	FString FindNameConflict(const FString& Name) const;

	void OnActionNameChanged(const FText& NewText);
	void OnSuggestionPicked(TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo);
	void OnRadioChanged(ECheckBoxState NewState, bool bSelectingEvent);
	FReply OnSubmit();
	FReply OnCancel();

	void UpdateValidationUI();
};
