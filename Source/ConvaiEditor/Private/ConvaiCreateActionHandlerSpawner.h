// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BlueprintNodeSpawner.h"
#include "ConvaiCreateActionHandlerSpawner.generated.h"

class UEdGraph;
class UEdGraphNode;

/**
 * Action-menu spawner for "Create Convai Action Handler".
 *
 * Overrides Invoke so that selecting the palette entry does NOT drop a node at
 * the click location — instead it opens SConvaiCreateActionHandlerDialog and
 * synthesizes the chosen Event/Function plus the HandleActionCompletion call
 * on the appropriate graph.
 */
UCLASS()
class UConvaiCreateActionHandlerSpawner : public UBlueprintNodeSpawner
{
	GENERATED_BODY()

public:
	static UConvaiCreateActionHandlerSpawner* Create();

	// UBlueprintNodeSpawner interface
	virtual UEdGraphNode* Invoke(UEdGraph* ParentGraph, FBindingSet const& Bindings, FVector2D const Location) const override;
};
