// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiDefinitions.h"
#include "ConvaiChatbotComponent.h"
#include "Environment/ConvaiEnvironment.h"
#include "Environment/ConvaiEnvironmentLegacy.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	TArray<FString> ReadActionStrings(const FString& Json)
	{
		TArray<FString> Result;
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return Result;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Root->TryGetArrayField(TEXT("actions"), Values) || Values == nullptr)
		{
			return Result;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Action;
			if (Value.IsValid() && Value->TryGetString(Action))
			{
				Result.Add(Action);
			}
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiActionEnablementDefaultsTest,
	"Convai.Actions.Enablement.MetadataAndDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiActionEnablementDefaultsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("A default action is enabled"), FConvaiAction().bEnabled);
	TestTrue(TEXT("A named action is enabled"), FConvaiAction(TEXT("Inspect")).bEnabled);

#if WITH_EDITORONLY_DATA
	const FArrayProperty* ActionsProperty = FindFProperty<FArrayProperty>(
		FConvaiEnvironmentData::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiEnvironmentData, Actions));
	TestNotNull(TEXT("Actions property is reflected"), ActionsProperty);
	if (ActionsProperty != nullptr)
	{
		TestEqual(TEXT("Collapsed rows are titled by action name"),
			ActionsProperty->GetMetaData(TEXT("TitleProperty")), FString(TEXT("Name")));
	}
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiActionEnablementSerializationTest,
	"Convai.Actions.Enablement.Serialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiActionEnablementSerializationTest::RunTest(const FString& Parameters)
{
	FConvaiEnvironmentData Environment;
	Environment.Actions.Reset();

	FConvaiAction Enabled(TEXT("Inspect"), TEXT("Inspect an object"));
	FConvaiAction Disabled(TEXT("Escort To"), TEXT("Escort someone"));
	Disabled.bEnabled = false;
	Environment.Actions = { Enabled, Disabled, FConvaiAction() };

	const TArray<FString> Advertised = ReadActionStrings(Environment.ToActionConfigJson());
	TestEqual(TEXT("Only one action is advertised"), Advertised.Num(), 1);
	if (Advertised.Num() == 1)
	{
		TestTrue(TEXT("The enabled action is advertised"), Advertised[0].StartsWith(TEXT("Inspect")));
	}

	Enabled.bEnabled = false;
	Environment.Actions = { Enabled, Disabled };
	TestTrue(TEXT("An all-disabled action list emits no action_config"),
		Environment.ToActionConfigJson().IsEmpty());

	UConvaiChatbotComponent* Chatbot = NewObject<UConvaiChatbotComponent>(GetTransientPackage());
	TestNotNull(TEXT("A transient chatbot can be created"), Chatbot);
	if (Chatbot != nullptr)
	{
		Enabled.bEnabled = true;
		Chatbot->EnvironmentData.Actions = { Enabled, Disabled, FConvaiAction() };
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		const TArray<FString> LegacyNames = Chatbot->GetEnvironment()->GetActions();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		TestEqual(TEXT("The legacy action view contains one action"), LegacyNames.Num(), 1);
		if (LegacyNames.Num() == 1)
		{
			TestEqual(TEXT("The legacy action view keeps only the enabled action"),
				LegacyNames[0], FString(TEXT("Inspect")));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
