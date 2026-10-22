// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiCreateActionHandlerSpawner.h"
#include "K2Node_ConvaiCreateActionHandler.h"
#include "SConvaiCreateActionHandlerDialog.h"

#include "BlueprintNodeTemplateCache.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"

#include "ConvaiChatbotComponent.h"
#include "ConvaiDefinitions.h"

#define LOCTEXT_NAMESPACE "ConvaiCreateActionHandlerSpawner"

namespace
{
	const TCHAR* HandleActionCompletionFunctionName = TEXT("HandleActionCompletion");

	/** Walks the Blueprint's SCS for the first UConvaiChatbotComponent and returns its
	 *  auto-generated variable name. NAME_None if no chatbot is present. */
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

	/** Finds the first output exec pin on a node — works for both K2Node_CustomEvent
	 *  and K2Node_FunctionEntry, whose "then" pin names differ slightly between
	 *  UE versions but are always the unique exec-typed output. */
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

	/** Adds a node to a graph and runs the placement boilerplate. */
	template <typename TNode>
	TNode* AddNode(UEdGraph* Graph, int32 X, int32 Y)
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

	/** The runtime (UConvaiChatbotComponent::TriggerNamedBlueprintAction → TryCallFunction)
	 *  invokes the user's named event/function with a single FConvaiResultAction argument.
	 *  Adds that as an output pin on the entry node so the handler can read action params. */
	void AddConvaiResultActionParameter(UK2Node_EditablePinBase* Entry)
	{
		if (!Entry) { return; }
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = FConvaiResultAction::StaticStruct();
		Entry->CreateUserDefinedPin(TEXT("ConvaiResultAction"), PinType, EGPD_Output, /*bUseUniqueName*/ true);
	}

	/** Spawns the Event or FunctionEntry node and returns the graph it's on plus
	 *  the entry node itself (whose exec output we'll wire from). */
	UEdGraphNode* CreateEntryNode(UBlueprint* BP, bool bIsEvent, const FString& Name,
		UEdGraph* ContextGraph, FVector2D const& ClickLocation, UEdGraph*& OutGraph)
	{
		OutGraph = nullptr;
		if (!BP) { return nullptr; }

		if (bIsEvent)
		{
			UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
			if (!EventGraph) { return nullptr; }

			// Place at the click location only when the user is already on the event graph;
			// otherwise the X/Y from a different graph's coordinate space would be meaningless.
			const FVector2D Pos = (ContextGraph == EventGraph)
				? ClickLocation
				: FVector2D::ZeroVector;

			UK2Node_CustomEvent* Event = AddNode<UK2Node_CustomEvent>(EventGraph,
				static_cast<int32>(Pos.X), static_cast<int32>(Pos.Y));
			Event->CustomFunctionName = FName(*Name);
			Event->bIsEditable = true;
			AddConvaiResultActionParameter(Event);
			Event->ReconstructNode();

			OutGraph = EventGraph;
			return Event;
		}

		// New function graph — UE auto-creates the FunctionEntry inside it.
		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated*/ true, /*SignatureClass*/ nullptr);

		TArray<UK2Node_FunctionEntry*> Entries;
		NewGraph->GetNodesOfClass(Entries);
		UK2Node_FunctionEntry* Entry = Entries.Num() > 0 ? Entries[0] : nullptr;
		if (Entry)
		{
			AddConvaiResultActionParameter(Entry);
			Entry->ReconstructNode();
		}
		OutGraph = NewGraph;
		return Entry;
	}

	UEdGraphNode* RunDialogAndCreateHandler(UBlueprint* Blueprint, UEdGraph* ContextGraph, FVector2D const& Location)
	{
		if (!Blueprint) { return nullptr; }

		const FConvaiCreateActionHandlerResult Choice =
			SConvaiCreateActionHandlerDialog::OpenModal(Blueprint);

		if (!Choice.bSubmitted || Choice.ActionName.IsEmpty())
		{
			return nullptr;
		}

		FScopedTransaction Transaction(LOCTEXT("CreateActionHandlerTransaction", "Create Convai Action Handler"));
		Blueprint->Modify();

		// 1. Spawn the entry node (event or function entry) on its graph.
		UEdGraph* TargetGraph = nullptr;
		UEdGraphNode* EntryNode = CreateEntryNode(Blueprint, Choice.bIsEvent, Choice.ActionName,
			ContextGraph, Location, TargetGraph);
		if (!EntryNode || !TargetGraph)
		{
			Transaction.Cancel();
			return nullptr;
		}

		// 2. Spawn the HandleActionCompletion call, positioned to the right of the entry.
		UFunction* HandleFn = UConvaiChatbotComponent::StaticClass()
			->FindFunctionByName(HandleActionCompletionFunctionName);
		if (!HandleFn)
		{
			// HandleActionCompletion is a stable public UFUNCTION — missing it means
			// the build is misconfigured. Surface, don't silently no-op.
			Transaction.Cancel();
			return nullptr;
		}

		const int32 CallX = EntryNode->NodePosX + 320;
		const int32 CallY = EntryNode->NodePosY;
		UK2Node_CallFunction* CallNode = AddNode<UK2Node_CallFunction>(TargetGraph, CallX, CallY);
		CallNode->SetFromFunction(HandleFn);
		CallNode->ReconstructNode();

		// 3. Wire entry-exec → CallNode exec-in.
		UEdGraphPin* EntryExec = FindOutputExecPin(EntryNode);
		UEdGraphPin* CallExec  = CallNode->GetExecPin();
		if (EntryExec && CallExec)
		{
			EntryExec->MakeLinkTo(CallExec);
		}

		// 4. Auto-wire the chatbot self pin from the BP's component variable (if any).
		const FName ChatbotVar = FindChatbotVariableName(Blueprint);
		UEdGraphPin* CallSelf  = CallNode->FindPin(UEdGraphSchema_K2::PN_Self);
		if (CallSelf && !ChatbotVar.IsNone())
		{
			const int32 GetX = CallX - 220;
			const int32 GetY = CallY + 96;
			UK2Node_VariableGet* GetVar = AddNode<UK2Node_VariableGet>(TargetGraph, GetX, GetY);
			GetVar->VariableReference.SetSelfMember(ChatbotVar);
			GetVar->ReconstructNode();

			if (UEdGraphPin* VarOutput = GetVar->FindPin(ChatbotVar))
			{
				VarOutput->MakeLinkTo(CallSelf);
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		return EntryNode;
	}
}

UConvaiCreateActionHandlerSpawner* UConvaiCreateActionHandlerSpawner::Create()
{
	UConvaiCreateActionHandlerSpawner* Spawner = NewObject<UConvaiCreateActionHandlerSpawner>(GetTransientPackage());
	check(Spawner);
	Spawner->NodeClass = UK2Node_ConvaiCreateActionHandler::StaticClass();

	FBlueprintActionUiSpec& Sig = Spawner->DefaultMenuSignature;
	Sig.MenuName = LOCTEXT("MenuName", "Create Convai Action Handler");
	Sig.Category = LOCTEXT("Category", "Convai");
	Sig.Tooltip  = LOCTEXT("Tooltip",
		"Generate an Event or Function on this Blueprint named after a Convai action, "
		"with HandleActionCompletion wired to its exec output. Suggests names from "
		"the chatbot component on this actor when available.");
	Sig.Keywords = LOCTEXT("Keywords", "convai action handler chatbot");

	return Spawner;
}

UEdGraphNode* UConvaiCreateActionHandlerSpawner::Invoke(UEdGraph* ParentGraph, FBindingSet const& Bindings, FVector2D const Location) const
{
	if (!ParentGraph) { return nullptr; }

	// The action database calls Invoke with a transient graph during menu construction
	// (via UBlueprintNodeSpawner::GetTemplateNode → FBlueprintNodeTemplateCache) to build a
	// preview/filter-test node. We must NOT open the modal dialog in that path — only run the
	// real action when the user explicitly picks the entry and Invoke is called with the user's
	// real BP graph. Return a bare stub for the template case.
	if (FBlueprintNodeTemplateCache::IsTemplateOuter(ParentGraph))
	{
		return NewObject<UK2Node_ConvaiCreateActionHandler>(ParentGraph);
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(ParentGraph);
	if (!Blueprint) { return nullptr; }

	// Returning the entry node is required: when ParentGraph IS the Event Graph and the user
	// picked Event, our handler adds nodes to ParentGraph itself, and the action-menu post-spawn
	// path in BlueprintActionMenuItem.cpp asserts SpawnedNode != nullptr in that case. For the
	// Function case, ParentGraph gets no new nodes (everything lands on the new function graph),
	// the engine takes the "existing node" branch instead and BringKismetToFocusAttentionOnObject
	// is invoked on the returned entry — which jumps the editor to the new function graph for us.
	return RunDialogAndCreateHandler(Blueprint, ParentGraph, Location);
}

#undef LOCTEXT_NAMESPACE
