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
	
	// Get interface once and reuse it
	IConvaiConnectionInterface* Interface = nullptr;
	if (SessionProxy)
	{
		if (const TScriptInterface<IConvaiConnectionInterface> InterfaceScriptInterface = SessionProxy->GetConnectionInterface(); InterfaceScriptInterface.GetObject())
		{
			Interface = InterfaceScriptInterface.GetInterface();
		}
	}
	
	// Determine connection type
	Params.ConnectionType = UConvaiUtils::GetConnectionType();
	if (UConvaiUtils::IsAlwaysAllowVisionEnabled())
	{
		Params.ConnectionType = TEXT("video");
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Always allow vision is enabled, using video connection type for character ID: %s"), *InCharacterID);
	}
	else if (Interface && Interface->IsVisionSupported())
	{
		Params.ConnectionType = TEXT("video");
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Vision is supported by proxy, using video connection type for character ID: %s"), *InCharacterID);
	}
	
	// Determine blendshape provider and format based on lip sync mode
	Params.BlendshapeProvider = TEXT("not_provided");
	Params.BlendshapeFormat = TEXT("");

	// Check if the lip sync component requires precomputed face data from the server
	// If not (e.g., OVR LipSync generates data locally from audio), skip setting blendshape provider

	if (const bool bRequiresPrecomputedFaceData = Interface ? Interface->RequiresPrecomputedFaceData() : true)
	{
		// Helper lambda to set provider and format for a specific mode
		auto SetBlendshapeParamsForMode = [&Params](const EC_LipSyncMode Mode)
		{
			switch (Mode)
			{
			case EC_LipSyncMode::VisemeBased:
				Params.BlendshapeProvider = TEXT("ovr");
				Params.BlendshapeFormat = TEXT("");
				break;
			case EC_LipSyncMode::BS_MHA:
				Params.BlendshapeProvider = TEXT("neurosync");
				Params.BlendshapeFormat = TEXT("mha");
				break;
			case EC_LipSyncMode::BS_ARKit:
				Params.BlendshapeProvider = TEXT("neurosync");
				Params.BlendshapeFormat = TEXT("arkit");
				break;
			case EC_LipSyncMode::BS_CC4_Extended:
				Params.BlendshapeProvider = TEXT("neurosync");
				Params.BlendshapeFormat = TEXT("cc4_extended");
				break;
			case EC_LipSyncMode::Off:
			default:
				Params.BlendshapeProvider = TEXT("not_provided");
				Params.BlendshapeFormat = TEXT("");
				break;
			}
		};

		switch (const EC_LipSyncMode GlobalMode = UConvaiUtils::GetLipSyncMode())
		{
		case EC_LipSyncMode::Off:
			Params.BlendshapeProvider = TEXT("not_provided");
			break;

		case EC_LipSyncMode::Auto:
			if (Interface)
			{
				SetBlendshapeParamsForMode(Interface->GetLipSyncMode());
			}
			break;

		case EC_LipSyncMode::VisemeBased:
		case EC_LipSyncMode::BS_MHA:
		case EC_LipSyncMode::BS_ARKit:
			SetBlendshapeParamsForMode(GlobalMode);
			break;
		}
	}
	
	// Get End User ID and Metadata from interface
	if (Interface)
	{
		Params.EndUserID = Interface->GetEndUserID();
		Params.EndUserMetadata = Interface->GetEndUserMetadata();
	}
	
	// If End User ID is not provided, use device unique identifier as fallback
	if (Params.EndUserID.IsEmpty())
	{
		Params.EndUserID = UConvaiUtils::GetDeviceUniqueIdentifier();
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("End User ID not provided, using Device ID: %s"), *Params.EndUserID);
	}
	else
	{
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Using End User ID: %s"), *Params.EndUserID);
	}
	
	if (!Params.EndUserMetadata.IsEmpty())
	{
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Using End User Metadata: %s"), *Params.EndUserMetadata);
	}

	Params.ChunkSize = UConvaiUtils::GetChunkSize();
	Params.OutputFPS = UConvaiUtils::GetOutputFPS();
	Params.FramesBufferDuration = UConvaiUtils::GetFramesBufferDuration();
	
	return Params;
}