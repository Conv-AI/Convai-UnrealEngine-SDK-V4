// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

class FJsonObject;

/** Portable copy identities only. File presence is checked against the archive and again on registration. */
namespace ConvaiAvatarSourceMap
{
	/** Check raw property identities before the JSON DOM can collapse case aliases. */
	bool ValidateJsonText(const FString& Text, FString& Error);
	bool Validate(const TMap<FName, FName>& Map, const FString& PluginName, FString& Error);
	bool Read(const FJsonObject& Json, const FString& PluginName, TOptional<TMap<FName, FName>>& OutMap, FString& Error);
	void Write(FJsonObject& Json, const TOptional<TMap<FName, FName>>& Map);
}
