// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

class FJsonObject;

/** Portable, exact missing-reference approvals. Never allows omissions inside the avatar plugin. */
namespace ConvaiAvatarMissingPackages
{
	bool Validate(const TArray<FName>& Packages, const FString& PluginName, FString& Error);
	bool Read(const FJsonObject& Json, const FString& PluginName, TOptional<TArray<FName>>& OutPackages, FString& Error);
	void Write(FJsonObject& Json, const TOptional<TArray<FName>>& Packages);
}
