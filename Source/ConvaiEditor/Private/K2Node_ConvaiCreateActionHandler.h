// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "K2Node.h"
#include "K2Node_ConvaiCreateActionHandler.generated.h"

class FBlueprintActionDatabaseRegistrar;

/**
 * Action-database hook for the "Create Convai Action Handler" palette entry.
 *
 * This UK2Node is never actually placed in any graph — it exists only so the
 * Blueprint Action Database calls our GetMenuActions override, where we
 * register a custom UConvaiCreateActionHandlerSpawner that opens the dialog
 * and synthesizes the real event/function + HandleActionCompletion nodes.
 */
UCLASS()
class UK2Node_ConvaiCreateActionHandler : public UK2Node
{
	GENERATED_BODY()

public:
	// UK2Node interface
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
};
