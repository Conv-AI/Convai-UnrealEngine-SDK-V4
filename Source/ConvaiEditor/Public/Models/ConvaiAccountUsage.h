/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiAccountUsage.h
 *
 * Data model for Convai account usage information.
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Data model for Convai account usage information.
 */
struct FConvaiAccountUsage
{
	FString UserName;
	FString Email;
	FString PlanName;
	FString RenewDate;

	float InteractionUsageCurrent = 0.0f;
	float InteractionUsageLimit = 0.0f;

	float ElevenlabsUsageCurrent = 0.0f;
	float ElevenlabsUsageLimit = 0.0f;

	float CoreAPIUsageCurrent = 0.0f;
	float CoreAPIUsageLimit = 0.0f;

	float PixelStreamingUsageCurrent = 0.0f;
	float PixelStreamingUsageLimit = 0.0f;
};
