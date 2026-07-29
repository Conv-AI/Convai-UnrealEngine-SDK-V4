// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Environment/ConvaiEnvironment.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString FConvaiEnvironmentData::ToActionConfigJson() const
{
	// No advertised actions means no action_config (the server treats absence as a
	// non-action bot). Objects and characters without actions are not useful to the LLM.
	if (Actions.Num() == 0)
	{
		return FString();
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> ActionValues;
	ActionValues.Reserve(Actions.Num());
	for (const FConvaiAction& A : Actions)
	{
		if (!A.bEnabled || A.Name.IsEmpty())
		{
			continue;
		}
		ActionValues.Add(MakeShared<FJsonValueString>(A.ToActionConfigString()));
	}
	if (ActionValues.Num() == 0)
	{
		return FString();
	}
	Root->SetArrayField(TEXT("actions"), ActionValues);

	auto SerializeEntries = [](const TArray<FConvaiObjectEntry>& Entries, const TCHAR* DescriptionField)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Entries.Num());
		for (const FConvaiObjectEntry& E : Entries)
		{
			if (E.Name.IsEmpty())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), E.Name);
			Obj->SetStringField(DescriptionField, E.Description);
			Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Out;
	};

	Root->SetArrayField(TEXT("objects"),    SerializeEntries(Objects,    TEXT("description")));
	Root->SetArrayField(TEXT("characters"), SerializeEntries(Characters, TEXT("bio")));

	if (!CurrentAttentionObject.Name.IsEmpty())
	{
		Root->SetStringField(TEXT("current_attention_object"), CurrentAttentionObject.Name);
	}

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}

const FConvaiObjectEntry* FConvaiEnvironmentData::FindObject(const FString& ObjectName) const
{
	for (const FConvaiObjectEntry& O : Objects)
	{
		if (O.Name == ObjectName)
		{
			return &O;
		}
	}
	return nullptr;
}

const FConvaiObjectEntry* FConvaiEnvironmentData::FindCharacter(const FString& CharacterName) const
{
	for (const FConvaiObjectEntry& C : Characters)
	{
		if (C.Name == CharacterName)
		{
			return &C;
		}
	}
	return nullptr;
}
