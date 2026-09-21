// Copyright 2026 Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarSourceMap.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Workspace/ConvaiAvatarWorkspace.h"

namespace ConvaiAvatarSourceMap
{
namespace
{
	constexpr int32 MaximumEntries = 15000;
	constexpr const TCHAR* Field = TEXT("source_to_destination_packages");
	bool ValidPath(const FString& Path)
	{
		for (TCHAR Character : Path) if (Character < 32 || Character == 127) return false;
		return Path.Len() < NAME_SIZE && FPackageName::IsValidTextForLongPackageName(Path)
			&& Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1) != INDEX_NONE;
	}
	bool ValidPair(const FString& Source, const FString& Destination, const FString& PluginName)
	{
		const FString Mount = TEXT("/") + PluginName + TEXT("/");
		return ValidPath(Source) && ValidPath(Destination) && !Source.StartsWith(TEXT("/Script/"))
			&& !Source.StartsWith(Mount) && Destination.Equals(Mount + Source.Mid(1), ESearchCase::CaseSensitive);
	}
	bool Invalid(FString& Error)
	{
		Error = TEXT("The source archive's copied-asset map is invalid. Each external package must name its exact copy inside this avatar plugin.");
		return false;
	}
}

bool ValidateJsonText(const FString& Text, FString& Error)
{
	// FJsonObject keys can be case-insensitive. Check the decoded tokens before
	// deserialization loses duplicate ownership claims; unrelated nested objects are ignored.
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	TSet<FString> Sources;
	int32 Depth = 0;
	int32 MapDepth = INDEX_NONE;
	bool bSeenMap = false;
	EJsonNotation Notation;
	while (Reader->ReadNext(Notation))
	{
		if (Notation == EJsonNotation::Error) return Invalid(Error);
		if (Notation == EJsonNotation::ObjectEnd || Notation == EJsonNotation::ArrayEnd)
		{
			if (Depth == MapDepth) MapDepth = INDEX_NONE;
			--Depth;
			continue;
		}
		const FString& Identifier = Reader->GetIdentifier();
		if (Depth == MapDepth)
		{
			const FString Identity = Identifier.ToLower();
			if (Sources.Contains(Identity) || Sources.Num() >= MaximumEntries) return Invalid(Error);
			Sources.Add(Identity);
		}
		if (Depth == 1 && Identifier.Equals(Field, ESearchCase::IgnoreCase))
		{
			if (bSeenMap) return Invalid(Error);
			bSeenMap = true;
			if (Notation == EJsonNotation::ObjectStart) MapDepth = Depth + 1;
		}
		if (Notation == EJsonNotation::ObjectStart || Notation == EJsonNotation::ArrayStart) ++Depth;
	}
	return Reader->GetErrorMessage().IsEmpty() || Invalid(Error);
}

bool Validate(const TMap<FName, FName>& Map, const FString& PluginName, FString& Error)
{
	if (!FConvaiAvatarWorkspace::IsSafePluginName(PluginName) || Map.Num() > MaximumEntries) return Invalid(Error);
	TSet<FName> Destinations;
	for (const auto& Pair : Map)
	{
		if (!ValidPair(Pair.Key.ToString(), Pair.Value.ToString(), PluginName) || Destinations.Contains(Pair.Value)) return Invalid(Error);
		Destinations.Add(Pair.Value);
	}
	return true;
}

bool Read(const FJsonObject& Json, const FString& PluginName, TOptional<TMap<FName, FName>>& OutMap, FString& Error)
{
	OutMap.Reset();
	const TSharedPtr<FJsonValue> Value = Json.TryGetField(Field);
	if (!Value.IsValid()) return true;
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (Value->Type != EJson::Object || !Value->TryGetObject(Object) || (*Object)->Values.Num() > MaximumEntries) return Invalid(Error);
	TMap<FName, FName> Map;
	for (const auto& Pair : (*Object)->Values)
	{
		const FString Source(*Pair.Key);
		FString Destination;
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String || !Pair.Value->TryGetString(Destination)
			|| !ValidPair(Source, Destination, PluginName) || Map.Contains(FName(*Source))) return Invalid(Error);
		Map.Add(FName(*Source), FName(*Destination));
	}
	if (!Validate(Map, PluginName, Error)) return false;
	OutMap = MoveTemp(Map);
	return true;
}

void Write(FJsonObject& Json, const TOptional<TMap<FName, FName>>& Map)
{
	if (!Map.IsSet()) return;
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	for (const auto& Pair : Map.GetValue()) Object->SetStringField(Pair.Key.ToString(), Pair.Value.ToString());
	Json.SetObjectField(Field, Object);
}
}
