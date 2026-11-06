// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Utility/ConvaiConnectionLibrary.h"
#include "ConvaiSubsystem.h"
#include "ConvaiUtils.h"

EC_PrepResult UConvaiConnectionLibrary::PrepareCharacterConnection(
	UObject* WorldContextObject,
	const FString& CharacterID,
	const float PrepTTLOverrideSeconds)
{
	UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(WorldContextObject);
	if (!Subsystem)
	{
		return EC_PrepResult::InternalError;
	}

	UConvaiConnectionManager* Manager = Subsystem->GetConnectionManager();
	if (!Manager)
	{
		return EC_PrepResult::InternalError;
	}

	const float TTL = PrepTTLOverrideSeconds > 0.0f
		? PrepTTLOverrideSeconds
		: UConvaiUtils::GetPrepConnectionTTL();

	if (TTL <= 0.0f)
	{
		return EC_PrepResult::Disabled;
	}

	return Manager->PrepareConnection(CharacterID, TTL);
}
