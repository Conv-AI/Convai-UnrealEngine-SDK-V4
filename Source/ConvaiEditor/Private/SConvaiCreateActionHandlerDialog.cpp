// Copyright 2022 Convai Inc. All Rights Reserved.

#include "SConvaiCreateActionHandlerDialog.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "ConvaiChatbotComponent.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "ConvaiCreateActionHandler"

void SConvaiCreateActionHandlerDialog::Construct(const FArguments& InArgs)
{
	Blueprint = InArgs._Blueprint;

	GatherSuggestedActionNames();

	const bool bHasSuggestions = SuggestionItems.Num() > 0;

	ChildSlot
	[
		SNew(SBorder)
		.Padding(12.f)
		.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
		[
			SNew(SVerticalBox)

			// ── Handler type ──────────────────────────────────────
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("HandlerTypeLabel", "Handler type:"))
				.Font(FAppStyle::Get().GetFontStyle("BoldFont"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsChecked_Lambda([this]() { return bIsEvent ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged(this, &SConvaiCreateActionHandlerDialog::OnRadioChanged, true)
					[
						SNew(STextBlock).Text(LOCTEXT("EventOption", "Event (on Event Graph)"))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsChecked_Lambda([this]() { return !bIsEvent ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged(this, &SConvaiCreateActionHandlerDialog::OnRadioChanged, false)
					[
						SNew(STextBlock).Text(LOCTEXT("FunctionOption", "Function (new function graph)"))
					]
				]
			]

			// ── Action name ───────────────────────────────────────
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ActionNameLabel", "Action name:"))
				.Font(FAppStyle::Get().GetFontStyle("BoldFont"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SAssignNew(ActionNameTextBox, SEditableTextBox)
				.HintText(LOCTEXT("ActionNameHint", "e.g. Wave, OpenDoor, PickUp..."))
				.OnTextChanged(this, &SConvaiCreateActionHandlerDialog::OnActionNameChanged)
			]

			// ── Suggestions (only when chatbot found) ─────────────
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				bHasSuggestions
				? StaticCastSharedRef<SWidget>(
					SAssignNew(SuggestionCombo, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&SuggestionItems)
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
					{
						return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
					})
					.OnSelectionChanged(this, &SConvaiCreateActionHandlerDialog::OnSuggestionPicked)
					.Content()
					[
						SNew(STextBlock).Text(LOCTEXT("PickFromChatbot", "Pick from chatbot's actions..."))
					])
				: StaticCastSharedRef<SWidget>(
					SNew(STextBlock)
					.Text(LOCTEXT("NoChatbotHint", "No UConvaiChatbotComponent found in this Blueprint's components — type the action name manually."))
					.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f))
					.AutoWrapText(true))
			]

			// ── Conflict warning ──────────────────────────────────
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SAssignNew(ConflictWarning, STextBlock)
				.ColorAndOpacity(FLinearColor(1.f, 0.55f, 0.f))
				.AutoWrapText(true)
				.Visibility(EVisibility::Collapsed)
			]

			// ── Buttons ───────────────────────────────────────────
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SUniformGridPanel)
				.SlotPadding(FAppStyle::Get().GetMargin("StandardDialog.SlotPadding"))
				.MinDesiredSlotWidth(FAppStyle::Get().GetFloat("StandardDialog.MinDesiredSlotWidth"))
				.MinDesiredSlotHeight(FAppStyle::Get().GetFloat("StandardDialog.MinDesiredSlotHeight"))
				+ SUniformGridPanel::Slot(0, 0)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.ContentPadding(FAppStyle::Get().GetMargin("StandardDialog.ContentPadding"))
					.Text(LOCTEXT("Cancel", "Cancel"))
					.OnClicked(this, &SConvaiCreateActionHandlerDialog::OnCancel)
				]
				+ SUniformGridPanel::Slot(1, 0)
				[
					SAssignNew(SubmitButton, SButton)
					.HAlign(HAlign_Center)
					.ContentPadding(FAppStyle::Get().GetMargin("StandardDialog.ContentPadding"))
					.Text(LOCTEXT("Create", "Create"))
					.IsEnabled(false)
					.OnClicked(this, &SConvaiCreateActionHandlerDialog::OnSubmit)
				]
			]
		]
	];

	UpdateValidationUI();
}

void SConvaiCreateActionHandlerDialog::GatherSuggestedActionNames()
{
	SuggestionItems.Reset();

	UBlueprint* BP = Blueprint.Get();
	if (!BP || !BP->SimpleConstructionScript)
	{
		return;
	}

	for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
	{
		if (!Node || !Node->ComponentTemplate)
		{
			continue;
		}
		UConvaiChatbotComponent* Chatbot = Cast<UConvaiChatbotComponent>(Node->ComponentTemplate);
		if (!Chatbot)
		{
			continue;
		}
		for (const FConvaiAction& Action : Chatbot->EnvironmentData.Actions)
		{
			if (!Action.Name.IsEmpty())
			{
				SuggestionItems.Add(MakeShared<FString>(Action.Name));
			}
		}
		break; // first chatbot wins — there's almost always only one
	}
}

FString SConvaiCreateActionHandlerDialog::FindNameConflict(const FString& Name) const
{
	UBlueprint* BP = Blueprint.Get();
	if (!BP || Name.IsEmpty())
	{
		return FString();
	}

	const FName AsName(*Name);

	// Function graphs (UFunction-backed event-stub functions and regular functions)
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == AsName)
		{
			return FString::Printf(TEXT("A function named '%s' already exists on this Blueprint. Pick a different action name."), *Name);
		}
	}

	// Custom events on the event graphs (ubergraph pages)
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) { continue; }
		TArray<UK2Node_Event*> Events;
		Graph->GetNodesOfClass(Events);
		for (UK2Node_Event* Evt : Events)
		{
			if (Evt && Evt->CustomFunctionName == AsName)
			{
				return FString::Printf(TEXT("An event named '%s' already exists on the event graph. Pick a different action name."), *Name);
			}
			// Also catch native overrides (e.g. ReceiveBeginPlay) just in case
			if (Evt && Evt->EventReference.GetMemberName() == AsName)
			{
				return FString::Printf(TEXT("'%s' clashes with an event already in use on this Blueprint."), *Name);
			}
		}
	}

	return FString();
}

void SConvaiCreateActionHandlerDialog::OnActionNameChanged(const FText& NewText)
{
	ActionName = NewText.ToString();
	UpdateValidationUI();
}

void SConvaiCreateActionHandlerDialog::OnSuggestionPicked(TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo)
{
	if (!Item.IsValid() || !ActionNameTextBox.IsValid())
	{
		return;
	}
	ActionNameTextBox->SetText(FText::FromString(*Item));
	// SetText doesn't fire OnTextChanged — push the value manually.
	ActionName = *Item;
	UpdateValidationUI();
}

void SConvaiCreateActionHandlerDialog::OnRadioChanged(ECheckBoxState NewState, bool bSelectingEvent)
{
	if (NewState == ECheckBoxState::Checked)
	{
		bIsEvent = bSelectingEvent;
		UpdateValidationUI();
	}
}

void SConvaiCreateActionHandlerDialog::UpdateValidationUI()
{
	const FString Conflict = FindNameConflict(ActionName);
	const bool bValid = !ActionName.IsEmpty() && Conflict.IsEmpty();

	if (SubmitButton.IsValid())
	{
		SubmitButton->SetEnabled(bValid);
	}
	if (ConflictWarning.IsValid())
	{
		if (Conflict.IsEmpty())
		{
			ConflictWarning->SetVisibility(EVisibility::Collapsed);
		}
		else
		{
			ConflictWarning->SetText(FText::FromString(Conflict));
			ConflictWarning->SetVisibility(EVisibility::Visible);
		}
	}
}

FReply SConvaiCreateActionHandlerDialog::OnSubmit()
{
	Result.bSubmitted = true;
	Result.bIsEvent   = bIsEvent;
	Result.ActionName = ActionName;

	if (TSharedPtr<SWindow> Window = OwnerWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	return FReply::Handled();
}

FReply SConvaiCreateActionHandlerDialog::OnCancel()
{
	Result.bSubmitted = false;
	if (TSharedPtr<SWindow> Window = OwnerWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	return FReply::Handled();
}

FConvaiCreateActionHandlerResult SConvaiCreateActionHandlerDialog::OpenModal(UBlueprint* Blueprint)
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WindowTitle", "Create Convai Action Handler"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);

	TSharedRef<SConvaiCreateActionHandlerDialog> Dialog = SNew(SConvaiCreateActionHandlerDialog)
		.Blueprint(Blueprint);
	Dialog->OwnerWindow = Window;

	Window->SetContent(Dialog);

	FSlateApplication::Get().AddModalWindow(Window, nullptr);

	return Dialog->Result;
}

#undef LOCTEXT_NAMESPACE
