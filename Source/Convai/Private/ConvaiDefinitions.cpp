// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiDefinitions.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiUtils.h"

DEFINE_LOG_CATEGORY(ConvaiDefinitionsLog);

const TMap<EEmotionIntensity, float> FConvaiEmotionState::ScoreMultipliers = 
{
	{EEmotionIntensity::None, 0.0},
	{EEmotionIntensity::LessIntense, 0.25},
	{EEmotionIntensity::Basic, 0.6},
	{EEmotionIntensity::MoreIntense, 1}
};

FConvaiConnectionParams FConvaiConnectionParams::Create(convai::ConvaiClient* InClient, const FString& InCharacterID, UConvaiConnectionSessionProxy* SessionProxy)
{
	FConvaiConnectionParams Params;
	Params.Client = InClient;
	Params.CharacterID = InCharacterID;
	Params.LLMProvider = UConvaiUtils::GetLLMProvider();
	
	// Determine connection type
	Params.ConnectionType = UConvaiUtils::GetConnectionType();
	if (UConvaiUtils::IsAlwaysAllowVisionEnabled())
	{
		Params.ConnectionType = TEXT("video");
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Always allow vision is enabled, using video connection type for character ID: %s"), *InCharacterID);
	}
	else if (SessionProxy)
	{
		if (const TScriptInterface<IConvaiConnectionInterface> Interface = SessionProxy->GetConnectionInterface(); Interface.GetObject())
		{
			if (Interface->IsVisionSupported())
			{
				Params.ConnectionType = TEXT("video");
				CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Vision is supported by proxy, using video connection type for character ID: %s"), *InCharacterID);
			}
		}
	}
	
	// Determine blendshape provider
	Params.BlendshapeProvider = TEXT("not_provided");	
	switch (UConvaiUtils::GetLipSyncMode())
	{
	case EC_LipSyncMode::Off:
		Params.BlendshapeProvider = TEXT("not_provided");
		break;
		
	case EC_LipSyncMode::Auto:
		if (SessionProxy)
		{
			if (const TScriptInterface<IConvaiConnectionInterface> Interface = SessionProxy->GetConnectionInterface(); Interface.GetObject())
			{
				switch (Interface->GetLipSyncMode())
				{
				case EC_LipSyncMode::VisemeBased:
					Params.BlendshapeProvider = TEXT("ovr");
					break;
				case EC_LipSyncMode::BlendshapeBased:
					Params.BlendshapeProvider = TEXT("neurosync");
					break;
				case EC_LipSyncMode::Off:
					Params.BlendshapeProvider = TEXT("not_provided");
					break;
				default:
					CONVAI_LOG(ConvaiDefinitionsLog, Warning, TEXT("Invalid lip sync mode in Auto mode"));
					break;
				}
			}
		}
		break;
		
	case EC_LipSyncMode::VisemeBased:
		Params.BlendshapeProvider = TEXT("ovr");
		break;
		
	case EC_LipSyncMode::BlendshapeBased:
		Params.BlendshapeProvider = TEXT("neurosync");
		break;
	}
	
	return Params;
}