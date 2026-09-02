// Copyright 2022 Convai Inc. All Rights Reserved.

#include "K2Node_ConvaiCreateActionHandler.h"
#include "ConvaiCreateActionHandlerSpawner.h"

#include "BlueprintActionDatabaseRegistrar.h"

#define LOCTEXT_NAMESPACE "K2Node_ConvaiCreateActionHandler"

void UK2Node_ConvaiCreateActionHandler::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* ActionKey = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UConvaiCreateActionHandlerSpawner* Spawner = UConvaiCreateActionHandlerSpawner::Create();
		check(Spawner);
		ActionRegistrar.AddBlueprintAction(ActionKey, Spawner);
	}
}

FText UK2Node_ConvaiCreateActionHandler::GetMenuCategory() const
{
	return LOCTEXT("ConvaiCategory", "Convai");
}

#undef LOCTEXT_NAMESPACE
