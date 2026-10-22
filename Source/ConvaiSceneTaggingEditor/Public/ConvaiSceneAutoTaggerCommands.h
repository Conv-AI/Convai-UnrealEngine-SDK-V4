// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Framework/Commands/Commands.h"
#include "ConvaiSceneAutoTaggerStyle.h"

class FConvaiSceneAutoTaggerCommands : public TCommands<FConvaiSceneAutoTaggerCommands>
{
public:

	FConvaiSceneAutoTaggerCommands()
		: TCommands<FConvaiSceneAutoTaggerCommands>(TEXT("ConvaiSceneAutoTagger"), NSLOCTEXT("Contexts", "ConvaiSceneAutoTagger", "Convai Scene Auto Tagger"), NAME_None, FConvaiSceneAutoTaggerStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > OpenPluginWindow;
};
