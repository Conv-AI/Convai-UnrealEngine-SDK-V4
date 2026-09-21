// Copyright Epic Games, Inc. All Rights Reserved.

#include "ConvaiSceneAutoTaggerCommands.h"

#define LOCTEXT_NAMESPACE "FConvaiSceneAutoTaggerModule"

void FConvaiSceneAutoTaggerCommands::RegisterCommands()
{
	UI_COMMAND(
		OpenPluginWindow,
		"Scene Auto Tagger",
		"Explore the current level and review Convai scene-object metadata",
		EUserInterfaceActionType::Button,
		FInputChord());
}

#undef LOCTEXT_NAMESPACE
