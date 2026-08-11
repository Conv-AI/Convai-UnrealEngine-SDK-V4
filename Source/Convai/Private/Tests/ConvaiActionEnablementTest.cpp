// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiDefinitions.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiConnectionInterface.h"
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
	constexpr const TCHAR* CancelActionPlanName = TEXT("Cancel Action Plan");
	constexpr const TCHAR* EscortActionName = TEXT("Escort");

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

	bool ContainsActionNamed(const TArray<FConvaiAction>& Actions, const TCHAR* Name)
	{
		return Actions.ContainsByPredicate(
			[Name](const FConvaiAction& Action)
			{
				return Action.Name.Equals(Name, ESearchCase::IgnoreCase);
			});
	}

	const FConvaiAction* FindActionNamed(const TArray<FConvaiAction>& Actions, const TCHAR* Name)
	{
		return Actions.FindByPredicate(
			[Name](const FConvaiAction& Action)
			{
				return Action.Name.Equals(Name, ESearchCase::IgnoreCase);
			});
	}

	FConvaiAction MakeCanonicalEscortAction(bool bEnabled = true)
	{
		FConvaiActionParam Character(
			TEXT("character"),
			TEXT("The character who should follow the guide."),
			EConvaiActionParamType::Reference);
		FConvaiActionParam Destination(
			TEXT("destination"),
			TEXT("The world object or named destination to escort them to."),
			EConvaiActionParamType::Reference);
		Destination.Connector = TEXT("to");
		FConvaiAction Escort(
			EscortActionName,
			TEXT("Escort a character to a destination while keeping them nearby. ")
			TEXT("Use current surroundings when speaking: if already at the destination, ")
			TEXT("address it directly without follow-me or future-travel wording; if close by, ")
			TEXT("use only a brief transition; otherwise invite them to follow. ")
			TEXT("After travelling, acknowledge the destination naturally."),
			{ Character, Destination });
		Escort.bWaitForBotSpeech = true;
		Escort.bEnabled = bEnabled;
		return Escort;
	}

	int32 CountActionsNamed(const TArray<FConvaiAction>& Actions, const TCHAR* Name)
	{
		int32 Count = 0;
		for (const FConvaiAction& Action : Actions)
		{
			if (Action.Name.Equals(Name, ESearchCase::IgnoreCase))
			{
				++Count;
			}
		}
		return Count;
	}

	FString FreezeActionConfig(UConvaiChatbotComponent& Chatbot)
	{
		// The component's override is intentionally private; connection code calls
		// it through the public interface contract as this test does.
		return static_cast<IConvaiConnectionInterface&>(Chatbot).GetActionConfigJson();
	}

	void ReadFrozenContract(
		UConvaiChatbotComponent& Chatbot,
		TArray<FConvaiAction>& OutActions,
		TArray<FString>& OutBuiltIns)
	{
		static_cast<IConvaiConnectionInterface&>(Chatbot).GetAdvertisedActionContract(
			OutActions, OutBuiltIns);
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

	const FConvaiEnvironmentData DefaultEnvironment;
	const FConvaiAction* DefaultEscort =
		FindActionNamed(DefaultEnvironment.Actions, EscortActionName);
	TestNotNull(TEXT("The stock action list includes Escort for discoverability"),
		DefaultEscort);
	if (DefaultEscort != nullptr)
	{
		TestFalse(TEXT("The stock Escort row is disabled by default"),
			DefaultEscort->bEnabled);
		TestTrue(TEXT("The stock Escort row waits for bot speech by default"),
			DefaultEscort->bWaitForBotSpeech);
	}

	const FArrayProperty* ActionsProperty = FindFProperty<FArrayProperty>(
		FConvaiEnvironmentData::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiEnvironmentData, Actions));
	TestNotNull(TEXT("Actions property is reflected"), ActionsProperty);
#if WITH_EDITORONLY_DATA
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
	FConvaiAction Disabled(TEXT("Hidden Action"), TEXT("A disabled fixture"));
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

		Chatbot->EnvironmentData.bEnableActions = true;
		FreezeActionConfig(*Chatbot);
		TArray<FConvaiAction> FrozenActions;
		TArray<FString> FrozenBuiltIns;
		ReadFrozenContract(*Chatbot, FrozenActions, FrozenBuiltIns);
		TestEqual(TEXT("The frozen parser contract contains only the advertised action"),
			FrozenActions.Num(), 1);
		TestTrue(TEXT("The enabled named action is frozen for parsing"),
			ContainsActionNamed(FrozenActions, TEXT("Inspect")));
		TestFalse(TEXT("A disabled action is absent from the frozen parser contract"),
			ContainsActionNamed(FrozenActions, TEXT("Hidden Action")));

		Chatbot->EnvironmentData.Actions.Add(
			FConvaiAction(TEXT("Added after connect"), TEXT("A live-only fixture")));
		Chatbot->bEnableCancelActionPlanAction = true;
		TArray<FConvaiAction> ParsingActions;
		Chatbot->GetActionsForParsing(ParsingActions);
		TestEqual(TEXT("Live action edits do not expand the frozen parser contract"),
			ParsingActions.Num(), 1);
		TestFalse(TEXT("An action added after connect was not advertised"),
			ContainsActionNamed(ParsingActions, TEXT("Added after connect")));
		TestFalse(TEXT("A built-in enabled after connect was not advertised"),
			ContainsActionNamed(ParsingActions, CancelActionPlanName));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiActionEnablementEffectiveContractTest,
	"Convai.Actions.Enablement.EffectiveContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiActionEnablementEffectiveContractTest::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot =
		NewObject<UConvaiChatbotComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("A transient chatbot can be created"), Chatbot))
	{
		return false;
	}

	FConvaiAction Enabled(TEXT("Inspect"));
	FConvaiAction Disabled(TEXT("Hidden Action"));
	Disabled.bEnabled = false;
	FConvaiAction DisabledBuiltInShadow(TEXT("Remind Self"));
	DisabledBuiltInShadow.bEnabled = false;
	Chatbot->EnvironmentData.Actions = { Enabled, Disabled, DisabledBuiltInShadow };
	Chatbot->bEnableRemindSelfAction = true;

	IConvaiConnectionInterface* Connection = Chatbot;
	const TArray<FString> Advertised = ReadActionStrings(Connection->GetActionConfigJson());
	TestEqual(TEXT("Only the enabled designer action is advertised"), Advertised.Num(), 1);
	if (Advertised.Num() == 1)
	{
		TestTrue(TEXT("The advertised contract contains Inspect"),
			Advertised[0].StartsWith(TEXT("Inspect")));
	}

	TArray<FConvaiAction> Snapshot;
	TArray<FString> BuiltIns;
	Connection->GetAdvertisedActionContract(Snapshot, BuiltIns);
	TestEqual(TEXT("The parsing snapshot contains only one action"), Snapshot.Num(), 1);
	TestEqual(TEXT("A disabled authored row still reserves the same-named built-in"),
		BuiltIns.Num(), 0);

	Chatbot->EnvironmentData.Actions[1].bEnabled = true;
	Connection->GetAdvertisedActionContract(Snapshot, BuiltIns);
	TestEqual(TEXT("Live edits do not mutate the frozen connection contract"), Snapshot.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiActionCaseInsensitiveIdentityTest,
	"Convai.Actions.Enablement.CaseInsensitiveIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiActionCaseInsensitiveIdentityTest::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot =
		NewObject<UConvaiChatbotComponent>(GetTransientPackage());
	TestNotNull(TEXT("An action-mutation chatbot can be created"), Chatbot);
	if (Chatbot == nullptr)
	{
		return false;
	}

	Chatbot->EnvironmentData.Actions = {
		FConvaiAction(TEXT("Inspect Artifact"), TEXT("Original designer action"))
	};
	Chatbot->AddAction(
		FConvaiAction(TEXT("iNsPeCt aRtIfAcT"), TEXT("Replacement designer action")));
	TestEqual(TEXT("Mixed-case replacement keeps a single action identity"),
		Chatbot->EnvironmentData.Actions.Num(), 1);
	if (Chatbot->EnvironmentData.Actions.Num() == 1)
	{
		TestEqual(TEXT("The mixed-case replacement updates the existing action"),
			Chatbot->EnvironmentData.Actions[0].Description,
			FString(TEXT("Replacement designer action")));
	}

	Chatbot->RemoveAction(TEXT("INSPECT ARTIFACT"));
	TestEqual(TEXT("Mixed-case removal resolves the same action identity"),
		Chatbot->EnvironmentData.Actions.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiCancelActionPlanEnablementTest,
	"Convai.Actions.Cancellation.EnablementAndFrozenAdvertisement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiCancelActionPlanEnablementTest::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot = NewObject<UConvaiChatbotComponent>(GetTransientPackage());
	TestNotNull(TEXT("A transient chatbot can be created"), Chatbot);
	if (Chatbot == nullptr)
	{
		return false;
	}

	Chatbot->EnvironmentData.Actions.Reset();
	Chatbot->EnvironmentData.bEnableActions = true;
	TArray<FConvaiAction> Effective;
	Chatbot->GetEffectiveActions(Effective);
	TestFalse(TEXT("Cancel Action Plan is disabled by default"),
		ContainsActionNamed(Effective, CancelActionPlanName));

	Chatbot->bEnableCancelActionPlanAction = true;
	Chatbot->GetEffectiveActions(Effective);
	TestTrue(TEXT("The toggle adds Cancel Action Plan to the effective contract"),
		ContainsActionNamed(Effective, CancelActionPlanName));

	const TArray<FString> Advertised = ReadActionStrings(FreezeActionConfig(*Chatbot));
	TestTrue(TEXT("The enabled control is serialized at connect"),
		Advertised.ContainsByPredicate(
			[](const FString& Action)
			{
				return Action.StartsWith(CancelActionPlanName, ESearchCase::IgnoreCase);
			}));

	Chatbot->bEnableCancelActionPlanAction = false;
	TArray<FConvaiAction> FrozenForParsing;
	Chatbot->GetActionsForParsing(FrozenForParsing);
	TestTrue(TEXT("The connect-time control remains parseable after a live toggle edit"),
		ContainsActionNamed(FrozenForParsing, CancelActionPlanName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiCancelActionPlanShadowingTest,
	"Convai.Actions.Cancellation.DesignerActionShadowsBuiltIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiCancelActionPlanShadowingTest::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot = NewObject<UConvaiChatbotComponent>(GetTransientPackage());
	TestNotNull(TEXT("A transient chatbot can be created"), Chatbot);
	if (Chatbot == nullptr)
	{
		return false;
	}

	Chatbot->EnvironmentData.Actions = {
		FConvaiAction(TEXT("cAnCeL aCtIoN pLaN"),
			TEXT("A designer-owned action with the reserved name"))
	};
	Chatbot->EnvironmentData.bEnableActions = true;
	Chatbot->bEnableCancelActionPlanAction = true;

	TArray<FConvaiAction> Effective;
	Chatbot->GetEffectiveActions(Effective);
	TestEqual(TEXT("Shadowing does not duplicate the designer action"), Effective.Num(), 1);
	if (Effective.Num() == 1)
	{
		TestEqual(TEXT("The designer description is retained"), Effective[0].Description,
			FString(TEXT("A designer-owned action with the reserved name")));
	}

	const TArray<FString> Advertised = ReadActionStrings(FreezeActionConfig(*Chatbot));
	TestEqual(TEXT("Only the designer action is serialized"), Advertised.Num(), 1);
	TArray<FConvaiAction> FrozenActions;
	TArray<FString> FrozenBuiltIns;
	ReadFrozenContract(*Chatbot, FrozenActions, FrozenBuiltIns);
	TestEqual(TEXT("Mixed-case identity still produces one frozen action"),
		CountActionsNamed(FrozenActions, CancelActionPlanName), 1);
	TestFalse(TEXT("A mixed-case designer shadow is not marked as reserved"),
		FrozenBuiltIns.Contains(CancelActionPlanName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiEscortToContractTest,
	"Convai.Actions.EscortTo.ContractAndFrozenAdvertisement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiEscortToContractTest::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot = NewObject<UConvaiChatbotComponent>(GetTransientPackage());
	TestNotNull(TEXT("A transient chatbot can be created"), Chatbot);
	if (Chatbot == nullptr)
	{
		return false;
	}

	Chatbot->EnvironmentData.Actions = { MakeCanonicalEscortAction() };
	Chatbot->EnvironmentData.bEnableActions = true;
	Chatbot->bEnableCancelActionPlanAction = false;

	TArray<FConvaiAction> Effective;
	Chatbot->GetEffectiveActions(Effective);
	TestEqual(TEXT("The enabled Escort row is the only effective action"), Effective.Num(), 1);
	const FConvaiAction* Escort = FindActionNamed(Effective, EscortActionName);
	TestNotNull(TEXT("The authored Escort row is present"), Escort);
	TestFalse(TEXT("Escort does not implicitly advertise Cancel Action Plan"),
		ContainsActionNamed(Effective, CancelActionPlanName));
	if (Escort != nullptr)
	{
		TestTrue(TEXT("Escort waits for bot speech before starting"),
			Escort->bWaitForBotSpeech);
		TestTrue(TEXT("Escort grounds already-arrived dialogue"),
			Escort->Description.Contains(
				TEXT("already at the destination"), ESearchCase::IgnoreCase));
		TestTrue(TEXT("Escort keeps close-by transitions brief"),
			Escort->Description.Contains(
				TEXT("close by"), ESearchCase::IgnoreCase));
		TestTrue(TEXT("Escort reserves follow language for real travel"),
			Escort->Description.Contains(
				TEXT("otherwise invite them to follow"), ESearchCase::IgnoreCase));
		TestEqual(TEXT("Escort has exactly two parameters"), Escort->Parameters.Num(), 2);
		if (Escort->Parameters.Num() == 2)
		{
			TestEqual(TEXT("The escorted character is the first parameter"),
				Escort->Parameters[0].Name, FString(TEXT("character")));
			TestTrue(TEXT("The character parameter is a reference"),
				Escort->Parameters[0].Type == EConvaiActionParamType::Reference);
			TestEqual(TEXT("The destination is the second parameter"),
				Escort->Parameters[1].Name, FString(TEXT("destination")));
			TestTrue(TEXT("The destination parameter is a reference"),
				Escort->Parameters[1].Type == EConvaiActionParamType::Reference);
			TestEqual(TEXT("The destination is joined with 'to'"),
				Escort->Parameters[1].Connector, FString(TEXT("to")));
		}
	}

	const TArray<FString> Serialized = ReadActionStrings(FreezeActionConfig(*Chatbot));
	TestEqual(TEXT("Only the enabled Escort row is serialized"), Serialized.Num(), 1);
	TestTrue(TEXT("Escort uses the exact character-to-destination wire contract"),
		Serialized.ContainsByPredicate(
			[](const FString& Action)
			{
				return Action.StartsWith(
					TEXT("Escort {character: ref} to {destination: ref}"));
			}));
	TArray<FConvaiAction> FrozenActions;
	TArray<FString> FrozenBuiltIns;
	ReadFrozenContract(*Chatbot, FrozenActions, FrozenBuiltIns);
	TestTrue(TEXT("The canonical Escort row is frozen for parsing"),
		ContainsActionNamed(FrozenActions, EscortActionName));
	TestFalse(TEXT("An ordinary Escort row is not recorded as a connected built-in"),
		FrozenBuiltIns.Contains(EscortActionName));
	TestFalse(TEXT("Escort does not implicitly add the cancellation control"),
		FrozenBuiltIns.Contains(CancelActionPlanName));

	Chatbot->EnvironmentData.Actions[0].bEnabled = false;
	TArray<FConvaiAction> ParsingActions;
	Chatbot->GetActionsForParsing(ParsingActions);
	TestTrue(TEXT("Escort remains parseable for the frozen session after a live disable"),
		ContainsActionNamed(ParsingActions, EscortActionName));
	TestFalse(TEXT("A cancellation control was never part of the frozen session"),
		ContainsActionNamed(ParsingActions, CancelActionPlanName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiEscortToSchemaIdentityTest,
	"Convai.Actions.EscortTo.CanonicalSchemaIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiEscortToSchemaIdentityTest::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot =
		NewObject<UConvaiChatbotComponent>(GetTransientPackage());
	TestNotNull(TEXT("A transient chatbot can be created"), Chatbot);
	if (Chatbot == nullptr)
	{
		return false;
	}

	FConvaiAction NonCanonicalEscort(
		TEXT("eScOrT"),
		TEXT("A designer action that deliberately does not use the native schema"),
		{ FConvaiActionParam(TEXT("subject"), TEXT("A custom subject."),
			EConvaiActionParamType::String) });
	Chatbot->EnvironmentData.Actions = { NonCanonicalEscort };
	Chatbot->EnvironmentData.bEnableActions = true;
	FreezeActionConfig(*Chatbot);

	TArray<FConvaiAction> FrozenActions;
	TArray<FString> FrozenBuiltIns;
	ReadFrozenContract(*Chatbot, FrozenActions, FrozenBuiltIns);
	const FConvaiAction* FrozenEscort = FindActionNamed(FrozenActions, EscortActionName);
	TestNotNull(TEXT("The same-named designer action is retained"), FrozenEscort);
	if (FrozenEscort != nullptr)
	{
		TestEqual(TEXT("The designer schema is retained without native rewriting"),
			FrozenEscort->Parameters.Num(), 1);
		if (FrozenEscort->Parameters.Num() == 1)
		{
			TestEqual(TEXT("The custom parameter name is retained"),
				FrozenEscort->Parameters[0].Name, FString(TEXT("subject")));
			TestTrue(TEXT("The custom parameter type is retained"),
				FrozenEscort->Parameters[0].Type == EConvaiActionParamType::String);
		}
	}
	TestFalse(TEXT("No ordinary Escort row is marked as a connected built-in"),
		FrozenBuiltIns.Contains(EscortActionName));
	TestFalse(TEXT("A same-named designer row does not imply cancellation"),
		ContainsActionNamed(FrozenActions, CancelActionPlanName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiDisabledEscortRowTest,
	"Convai.Actions.EscortTo.DisabledRowNotAdvertised",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiDisabledEscortRowTest::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot =
		NewObject<UConvaiChatbotComponent>(GetTransientPackage());
	TestNotNull(TEXT("A disabled-shadow chatbot can be created"), Chatbot);
	if (Chatbot == nullptr)
	{
		return false;
	}

	FConvaiAction DisabledEscort = MakeCanonicalEscortAction(false);
	FConvaiAction DisabledCancel(
		TEXT("cAnCeL aCtIoN pLaN"), TEXT("Disabled designer cancellation fixture"));
	DisabledCancel.bEnabled = false;
	FConvaiAction DisabledUnrelated(
		TEXT("Disabled unrelated"), TEXT("Never advertised"));
	DisabledUnrelated.bEnabled = false;

	Chatbot->EnvironmentData.Actions = {
		DisabledEscort,
		DisabledCancel,
		DisabledUnrelated,
		FConvaiAction()
	};
	Chatbot->EnvironmentData.bEnableActions = true;
	Chatbot->bEnableCancelActionPlanAction = false;

	TArray<FConvaiAction> Effective;
	Chatbot->GetEffectiveActions(Effective);
	TestEqual(TEXT("Disabled authored rows are absent from the effective action list"),
		Effective.Num(), 0);

	const TArray<FString> Serialized = ReadActionStrings(FreezeActionConfig(*Chatbot));
	TestEqual(TEXT("No disabled or suppressed action is serialized"), Serialized.Num(), 0);
	TArray<FConvaiAction> FrozenActions;
	TArray<FString> FrozenBuiltIns;
	ReadFrozenContract(*Chatbot, FrozenActions, FrozenBuiltIns);
	TestEqual(TEXT("The parser snapshot remains empty"), FrozenActions.Num(), 0);
	TestFalse(TEXT("The unrelated disabled action is absent from parsing"),
		ContainsActionNamed(FrozenActions, TEXT("Disabled unrelated")));
	TestEqual(TEXT("No built-in was independently enabled for this contract"),
		FrozenBuiltIns.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
