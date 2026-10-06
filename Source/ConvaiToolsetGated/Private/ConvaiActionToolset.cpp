// Copyright Convai Inc. All Rights Reserved.

#include "ConvaiToolset.h"
#include "ConvaiToolsetCommon.h"

#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"

#include "ConvaiChatbotComponent.h"
#include "ConvaiDefinitions.h"

#define LOCTEXT_NAMESPACE "ConvaiActionToolset"

namespace
{
	const TCHAR* HandleActionCompletionFunctionName = TEXT("HandleActionCompletion");

	/** Walks the Blueprint's SCS for the first UConvaiChatbotComponent and returns its
	 *  auto-generated variable name. NAME_None if no chatbot is present.
	 *  Replicated from ConvaiCreateActionHandlerSpawner. */
	FName FindChatbotVariableName(UBlueprint* Blueprint)
	{
		if (!Blueprint || !Blueprint->SimpleConstructionScript)
		{
			return NAME_None;
		}
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Cast<UConvaiChatbotComponent>(Node->ComponentTemplate))
			{
				return Node->GetVariableName();
			}
		}
		return NAME_None;
	}

	/** Finds the unique exec output pin on an entry node (custom event / function entry). */
	UEdGraphPin* FindOutputExecPin(UEdGraphNode* Node)
	{
		if (!Node) { return nullptr; }
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	template <typename TNode>
	TNode* AddGraphNode(UEdGraph* Graph, int32 X, int32 Y)
	{
		TNode* Node = NewObject<TNode>(Graph);
		Graph->AddNode(Node, /*bFromUI*/ true, /*bSelectNewNode*/ false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		return Node;
	}

	/** Adds the single FConvaiResultAction output param that the runtime
	 *  (TriggerNamedBlueprintAction -> TryCallFunction) passes to the named event. */
	void AddConvaiResultActionParameter(UK2Node_EditablePinBase* Entry)
	{
		if (!Entry) { return; }
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = FConvaiResultAction::StaticStruct();
		Entry->CreateUserDefinedPin(TEXT("ConvaiResultAction"), PinType, EGPD_Output, /*bUseUniqueName*/ true);
	}

	/** Finds an existing custom event with this exact name on the event graph. */
	UK2Node_CustomEvent* FindExistingCustomEvent(UEdGraph* EventGraph, const FString& Name)
	{
		if (!EventGraph) { return nullptr; }
		TArray<UK2Node_CustomEvent*> Events;
		EventGraph->GetNodesOfClass(Events);
		for (UK2Node_CustomEvent* Event : Events)
		{
			if (Event && Event->CustomFunctionName == FName(*Name))
			{
				return Event;
			}
		}
		return nullptr;
	}
}

FString UConvaiActionToolset::AddConvaiAction(const FString& CharacterBlueprintPath, const FString& ActionName,
	const FString& Description, const TArray<FConvaiToolsetActionParam>& Parameters)
{
	if (ActionName.IsEmpty())
	{
		return TEXT("Error: ActionName is empty.");
	}

	FString Error;
	UBlueprint* Blueprint = ConvaiToolsetCommon::LoadBlueprintByPath(CharacterBlueprintPath, Error);
	if (!Blueprint)
	{
		return FString::Printf(TEXT("Error: %s"), *Error);
	}

	USCS_Node* ChatbotNode = ConvaiToolsetCommon::FindSCSNodeOfClass(Blueprint, UConvaiChatbotComponent::StaticClass());
	UConvaiChatbotComponent* ChatbotTemplate = ChatbotNode ? Cast<UConvaiChatbotComponent>(ChatbotNode->ComponentTemplate) : nullptr;
	if (!ChatbotTemplate)
	{
		return FString::Printf(TEXT("Error: no UConvaiChatbotComponent found on '%s'. Run SetupConvaiCharacter first."), *Blueprint->GetName());
	}

	FScopedTransaction Transaction(LOCTEXT("AddActionTx", "Add Convai Action"));
	Blueprint->Modify();

	// Build the FConvaiAction from the agent-facing param structs.
	TArray<FConvaiActionParam> ActionParams;
	for (const FConvaiToolsetActionParam& P : Parameters)
	{
		FConvaiActionParam NewParam;
		NewParam.Name = P.Name;
		NewParam.Description = P.Description;
		NewParam.Choices = P.Choices;
		// AddConvaiAction cannot supply a UEnum enumType, so a Type=Enum param would render to the LLM
		// as "ERROR: EnumType not set" with no options. For a fixed choice set the correct shape is
		// String + Choices (like the built-in Move action). Coerce Enum -> String so this tool can never
		// emit a broken enum parameter; the choices still come through.
		NewParam.Type = (P.Type == EConvaiActionParamType::Enum) ? EConvaiActionParamType::String : P.Type;
		ActionParams.Add(NewParam);
	}
	FConvaiAction NewAction(ActionName, Description, ActionParams);

	// Edit the EnvironmentData struct on the template AND propagate to placed instances (those
	// still holding the template's old EnvironmentData), preserving per-instance overrides.
	bool bReplaced = false;
	const int32 NumPropagated = ConvaiToolsetCommon::SetTemplatePropertyAndPropagate(Blueprint, ChatbotNode, TEXT("EnvironmentData"),
		[&]()
		{
			// Read-modify-write the Actions array, replacing a same-named entry in place.
			TArray<FConvaiAction>& Actions = ChatbotTemplate->EnvironmentData.Actions;
			for (FConvaiAction& Existing : Actions)
			{
				if (Existing.Name == ActionName)
				{
					Existing = NewAction;
					bReplaced = true;
					break;
				}
			}
			if (!bReplaced)
			{
				Actions.Add(NewAction);
			}
			ChatbotTemplate->EnvironmentData.bEnableActions = true;
		});

	FString SaveError;
	ConvaiToolsetCommon::CompileAndSaveBlueprint(Blueprint, /*bStructural*/ false, SaveError);

	FString Result = FString::Printf(TEXT("%s action '%s' (%d param(s)) on '%s' and enabled actions%s."),
		bReplaced ? TEXT("Replaced") : TEXT("Added"), *ActionName, ActionParams.Num(), *Blueprint->GetName(),
		NumPropagated > 0 ? *FString::Printf(TEXT(" (propagated to %d placed instance(s))"), NumPropagated) : TEXT(""));
	if (!SaveError.IsEmpty())
	{
		Result += FString::Printf(TEXT(" WARNING: %s"), *SaveError);
	}
	return Result;
}

FString UConvaiActionToolset::CreateConvaiActionHandler(const FString& BlueprintPath, const FString& ActionName)
{
	if (ActionName.IsEmpty())
	{
		return TEXT("Error: ActionName is empty.");
	}

	FString Error;
	UBlueprint* Blueprint = ConvaiToolsetCommon::LoadBlueprintByPath(BlueprintPath, Error);
	if (!Blueprint)
	{
		return FString::Printf(TEXT("Error: %s"), *Error);
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!EventGraph)
	{
		return FString::Printf(TEXT("Error: '%s' has no event graph."), *Blueprint->GetName());
	}

	// Idempotent: don't create a duplicate event if one with this exact name already exists.
	if (FindExistingCustomEvent(EventGraph, ActionName))
	{
		return FString::Printf(TEXT("Custom Event '%s' already exists on '%s'; no change."), *ActionName, *Blueprint->GetName());
	}

	UFunction* HandleFn = UConvaiChatbotComponent::StaticClass()->FindFunctionByName(HandleActionCompletionFunctionName);
	if (!HandleFn)
	{
		return TEXT("Error: HandleActionCompletion not found on UConvaiChatbotComponent (build misconfigured).");
	}

	FScopedTransaction Transaction(LOCTEXT("CreateHandlerTx", "Create Convai Action Handler"));
	Blueprint->Modify();

	// 1. Spawn the Custom Event named EXACTLY ActionName with the FConvaiResultAction param.
	UK2Node_CustomEvent* Event = AddGraphNode<UK2Node_CustomEvent>(EventGraph, 0, 0);
	Event->CustomFunctionName = FName(*ActionName);
	Event->bIsEditable = true;
	AddConvaiResultActionParameter(Event);
	Event->ReconstructNode();

	// 2. Spawn the HandleActionCompletion call to the right of the event.
	const int32 CallX = Event->NodePosX + 320;
	const int32 CallY = Event->NodePosY;
	UK2Node_CallFunction* CallNode = AddGraphNode<UK2Node_CallFunction>(EventGraph, CallX, CallY);
	CallNode->SetFromFunction(HandleFn);
	CallNode->ReconstructNode();

	// 3. Wire event-exec -> call exec-in.
	UEdGraphPin* EventExec = FindOutputExecPin(Event);
	UEdGraphPin* CallExec = CallNode->GetExecPin();
	if (EventExec && CallExec)
	{
		EventExec->MakeLinkTo(CallExec);
	}

	// 4. Auto-wire the chatbot self pin from the BP's component variable, when present.
	bool bWiredSelf = false;
	const FName ChatbotVar = FindChatbotVariableName(Blueprint);
	UEdGraphPin* CallSelf = CallNode->FindPin(UEdGraphSchema_K2::PN_Self);
	if (CallSelf && !ChatbotVar.IsNone())
	{
		const int32 GetX = CallX - 220;
		const int32 GetY = CallY + 96;
		UK2Node_VariableGet* GetVar = AddGraphNode<UK2Node_VariableGet>(EventGraph, GetX, GetY);
		GetVar->VariableReference.SetSelfMember(ChatbotVar);
		GetVar->ReconstructNode();

		if (UEdGraphPin* VarOutput = GetVar->FindPin(ChatbotVar))
		{
			VarOutput->MakeLinkTo(CallSelf);
			bWiredSelf = true;
		}
	}

	FString SaveError;
	ConvaiToolsetCommon::CompileAndSaveBlueprint(Blueprint, /*bStructural*/ true, SaveError);

	FString Result = FString::Printf(TEXT("Created Custom Event '%s' wired to HandleActionCompletion on '%s'%s."),
		*ActionName, *Blueprint->GetName(),
		bWiredSelf ? TEXT(" (chatbot self pin auto-wired)") : TEXT(" (no chatbot variable found; self pin left for manual wiring)"));
	if (!SaveError.IsEmpty())
	{
		Result += FString::Printf(TEXT(" WARNING: %s"), *SaveError);
	}
	// Return the created event name as the primary result, per the tool contract.
	return Result;
}

#undef LOCTEXT_NAMESPACE
