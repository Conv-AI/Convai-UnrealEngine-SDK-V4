// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "ConvaiDefinitions.h"
#include "ConvaiObjectComponent.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiTrackedPropertyResponseSettingsTest,
	"Convai.Objects.TrackedProperty.ResponseSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiTrackedPropertyResponseSettingsTest::RunTest(const FString& Parameters)
{
	FConvaiTrackedProperty Silent;
	Silent.ShouldRespond = EC_RunLLMOption::Never;
	Silent.Delivery = EConvaiContextDelivery::WaitUntilConversationIsIdle;
	Silent.bFlushImmediately = true;
	Silent.NormalizeResponseSettings();
	TestEqual(TEXT("Never uses normal delivery"), Silent.Delivery,
		EConvaiContextDelivery::SendNormally);
	TestFalse(TEXT("Never cannot flush immediately"), Silent.bFlushImmediately);

	for (const EC_RunLLMOption Responsive :
		{ EC_RunLLMOption::Auto, EC_RunLLMOption::Always })
	{
		FConvaiTrackedProperty Property;
		Property.ShouldRespond = Responsive;
		Property.Delivery = EConvaiContextDelivery::WaitUntilConversationIsIdle;
		Property.bFlushImmediately = true;
		Property.NormalizeResponseSettings();
		TestEqual(TEXT("Responsive delivery is preserved"), Property.Delivery,
			EConvaiContextDelivery::WaitUntilConversationIsIdle);
		TestTrue(TEXT("Responsive immediate flush is preserved"),
			Property.bFlushImmediately);
	}

#if WITH_EDITORONLY_DATA
	const FProperty* DeliveryProperty = FindFProperty<FProperty>(
		FConvaiTrackedProperty::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiTrackedProperty, Delivery));
	const FProperty* FlushProperty = FindFProperty<FProperty>(
		FConvaiTrackedProperty::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiTrackedProperty, bFlushImmediately));
	TestNotNull(TEXT("Delivery is reflected"), DeliveryProperty);
	TestNotNull(TEXT("Flush Immediately is reflected"), FlushProperty);
	if (DeliveryProperty)
	{
		TestTrue(TEXT("Nested customization owns Delivery enabling"),
			DeliveryProperty->GetMetaData(TEXT("EditCondition")).IsEmpty());
	}
	if (FlushProperty)
	{
		TestTrue(TEXT("Nested customization owns Flush enabling"),
			FlushProperty->GetMetaData(TEXT("EditCondition")).IsEmpty());
	}
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiTrackedPropertyMutatorNormalizationTest,
	"Convai.Objects.TrackedProperty.MutatorNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiTrackedPropertyMutatorNormalizationTest::RunTest(const FString& Parameters)
{
	UConvaiObjectComponent* Component =
		NewObject<UConvaiObjectComponent>(GetTransientPackage());
	TestNotNull(TEXT("Transient Convai Object Component is created"), Component);
	if (!Component)
	{
		return false;
	}

	FConvaiTrackedProperty Input;
	Input.PropertyPath = TEXT("bOpen");
	Input.ShouldRespond = EC_RunLLMOption::Never;
	Input.Delivery = EConvaiContextDelivery::WaitUntilConversationIsIdle;
	Input.bFlushImmediately = true;
	TestTrue(TEXT("A silent property can be added"),
		Component->AddTrackedProperty(Input));

	TArray<FConvaiTrackedProperty> Stored;
	Component->GetTrackedProperties(Stored);
	TestEqual(TEXT("One property is stored"), Stored.Num(), 1);
	if (Stored.Num() != 1)
	{
		return false;
	}
	TestEqual(TEXT("Add normalizes delivery"), Stored[0].Delivery,
		EConvaiContextDelivery::SendNormally);
	TestFalse(TEXT("Add normalizes flushing"), Stored[0].bFlushImmediately);

	Input.ShouldRespond = EC_RunLLMOption::Auto;
	Input.Delivery = EConvaiContextDelivery::WaitUntilConversationIsIdle;
	Input.bFlushImmediately = true;
	TestTrue(TEXT("The property can be updated to responsive"),
		Component->UpdateTrackedProperty(Input.PropertyPath, Input));
	Component->GetTrackedProperties(Stored);
	TestEqual(TEXT("Update preserves responsive delivery"), Stored[0].Delivery,
		EConvaiContextDelivery::WaitUntilConversationIsIdle);
	TestTrue(TEXT("Update preserves responsive flushing"),
		Stored[0].bFlushImmediately);

	Input.ShouldRespond = EC_RunLLMOption::Never;
	TestTrue(TEXT("The property can be returned to Never"),
		Component->UpdateTrackedProperty(Input.PropertyPath, Input));
	Component->GetTrackedProperties(Stored);
	TestEqual(TEXT("Never update restores normal delivery"), Stored[0].Delivery,
		EConvaiContextDelivery::SendNormally);
	TestFalse(TEXT("Never update clears immediate flushing"),
		Stored[0].bFlushImmediately);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMovementAwarenessDetailsMetadataTest,
	"Convai.Objects.MovementAwareness.DetailsMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiMovementAwarenessDetailsMetadataTest::RunTest(const FString& Parameters)
{
#if WITH_EDITORONLY_DATA
	const FProperty* OuterProperty = FindFProperty<FProperty>(
		UConvaiObjectComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UConvaiObjectComponent, MovementAwareness));
	TestNotNull(TEXT("Movement Awareness is reflected"), OuterProperty);
	if (OuterProperty)
	{
		TestEqual(TEXT("Exactly one Movement Awareness category is authored"),
			OuterProperty->GetMetaData(TEXT("Category")),
			FString(TEXT("Convai|Object|Movement Awareness")));
		TestTrue(TEXT("The settings struct is flattened into its category"),
			OuterProperty->HasMetaData(TEXT("ShowOnlyInnerProperties")));
	}

	const FString FlatteningCategory = TEXT("ConvaiObjectMovementSettings");
	for (TFieldIterator<FProperty> It(FConvaiObjectMovementSettings::StaticStruct());
		It; ++It)
	{
		TestEqual(
			*FString::Printf(TEXT("%s stays in the parent Movement Awareness category"),
				*It->GetName()),
			It->GetMetaData(TEXT("Category")), FlatteningCategory);
	}

	const FProperty* EnableProperty = FindFProperty<FProperty>(
		FConvaiObjectMovementSettings::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiObjectMovementSettings,
			bEnableMovementAwareness));
	const FProperty* SensitivityProperty = FindFProperty<FProperty>(
		FConvaiObjectMovementSettings::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiObjectMovementSettings,
			MovementSensitivity));
	const FProperty* StateProperty = FindFProperty<FProperty>(
		FConvaiObjectMovementSettings::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiObjectMovementSettings,
			bExposeMovementState));
	const FProperty* StartProperty = FindFProperty<FProperty>(
		FConvaiObjectMovementSettings::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiObjectMovementSettings,
			StartedMovingResponse));
	const FProperty* StopProperty = FindFProperty<FProperty>(
		FConvaiObjectMovementSettings::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiObjectMovementSettings,
			StoppedMovingResponse));
	const FProperty* DeliveryProperty = FindFProperty<FProperty>(
		FConvaiObjectMovementSettings::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiObjectMovementSettings, Delivery));
	const FProperty* FlushProperty = FindFProperty<FProperty>(
		FConvaiObjectMovementSettings::StaticStruct(),
		GET_MEMBER_NAME_CHECKED(FConvaiObjectMovementSettings,
			bFlushImmediately));

	TestNotNull(TEXT("Master switch is reflected"), EnableProperty);
	TestNotNull(TEXT("Sensitivity is reflected"), SensitivityProperty);
	TestNotNull(TEXT("Compatible movement-state field remains reflected"),
		StateProperty);
	TestNotNull(TEXT("Start response is reflected"), StartProperty);
	TestNotNull(TEXT("Stop response is reflected"), StopProperty);
	TestNotNull(TEXT("Movement delivery is reflected"), DeliveryProperty);
	TestNotNull(TEXT("Movement flush is reflected"), FlushProperty);

	if (EnableProperty)
	{
		TestEqual(TEXT("Master switch has the intended label"),
			EnableProperty->GetMetaData(TEXT("DisplayName")),
			FString(TEXT("Enable Movement Awareness")));
		TestFalse(TEXT("Master switch has a useful tooltip"),
			EnableProperty->GetMetaData(TEXT("ToolTip")).IsEmpty());
	}
	if (SensitivityProperty)
	{
		TestEqual(TEXT("Sensitivity is disabled with the master switch"),
			SensitivityProperty->GetMetaData(TEXT("EditCondition")),
			FString(TEXT("bEnableMovementAwareness")));
		TestFalse(TEXT("Sensitivity has a useful tooltip"),
			SensitivityProperty->GetMetaData(TEXT("ToolTip")).IsEmpty());
	}
	if (StateProperty)
	{
		TestEqual(TEXT("Existing state field gets the clearer UI label"),
			StateProperty->GetMetaData(TEXT("DisplayName")),
			FString(TEXT("Add Movement State")));
		TestEqual(TEXT("Movement state is disabled with the master switch"),
			StateProperty->GetMetaData(TEXT("EditCondition")),
			FString(TEXT("bEnableMovementAwareness")));
	}

	const FString StateCondition =
		TEXT("bEnableMovementAwareness && bExposeMovementState");
	const FString ResponsiveCondition =
		TEXT("bEnableMovementAwareness && bExposeMovementState && (StartedMovingResponse != EC_RunLLMOption::Never || StoppedMovingResponse != EC_RunLLMOption::Never)");
	if (StartProperty)
	{
		TestEqual(TEXT("Start response requires the durable state"),
			StartProperty->GetMetaData(TEXT("EditCondition")), StateCondition);
		TestEqual(TEXT("Start response has a natural label"),
			StartProperty->GetMetaData(TEXT("DisplayName")),
			FString(TEXT("When Movement Starts")));
	}
	if (StopProperty)
	{
		TestEqual(TEXT("Stop response requires the durable state"),
			StopProperty->GetMetaData(TEXT("EditCondition")), StateCondition);
		TestEqual(TEXT("Stop response has a natural label"),
			StopProperty->GetMetaData(TEXT("DisplayName")),
			FString(TEXT("When Movement Stops")));
	}
	if (DeliveryProperty)
	{
		TestEqual(TEXT("Delivery requires a responsive movement edge"),
			DeliveryProperty->GetMetaData(TEXT("EditCondition")),
			ResponsiveCondition);
	}
	if (FlushProperty)
	{
		TestEqual(TEXT("Flush requires a responsive movement edge"),
			FlushProperty->GetMetaData(TEXT("EditCondition")),
			ResponsiveCondition);
	}

	const UEnum* SensitivityEnum = StaticEnum<EConvaiMovementSensitivity>();
	TestNotNull(TEXT("Movement sensitivity enum is reflected"), SensitivityEnum);
	if (SensitivityEnum)
	{
		TestEqual(TEXT("Sensitivity exposes exactly five choices"),
			SensitivityEnum->NumEnums() - 1, 5); // excludes UHT's hidden _MAX
		TestEqual(TEXT("Very Low display name"),
			SensitivityEnum->GetDisplayNameTextByValue(
				static_cast<int64>(EConvaiMovementSensitivity::VeryLow)).ToString(),
			FString(TEXT("Very Low")));
		TestEqual(TEXT("Very High display name"),
			SensitivityEnum->GetDisplayNameTextByValue(
				static_cast<int64>(EConvaiMovementSensitivity::VeryHigh)).ToString(),
			FString(TEXT("Very High")));
	}
#endif

	FConvaiObjectMovementSettings Defaults;
	TestTrue(TEXT("Existing objects retain movement awareness by default"),
		Defaults.bEnableMovementAwareness);
	TestEqual(TEXT("Default sensitivity preserves prior behavior"),
		Defaults.MovementSensitivity, EConvaiMovementSensitivity::Medium);
	TestFalse(TEXT("Durable movement state remains opt-in"),
		Defaults.bExposeMovementState);
	TestEqual(TEXT("Movement starts silently by default"),
		Defaults.StartedMovingResponse, EC_RunLLMOption::Never);
	TestEqual(TEXT("Movement stops silently by default"),
		Defaults.StoppedMovingResponse, EC_RunLLMOption::Never);

	FConvaiObjectMovementSettings Silent;
	Silent.Delivery = EConvaiContextDelivery::WaitUntilConversationIsIdle;
	Silent.bFlushImmediately = true;
	Silent.NormalizeResponseSettings();
	TestEqual(TEXT("Two Never policies restore normal delivery"), Silent.Delivery,
		EConvaiContextDelivery::SendNormally);
	TestFalse(TEXT("Two Never policies clear immediate flushing"),
		Silent.bFlushImmediately);

	FConvaiObjectMovementSettings Responsive;
	Responsive.bEnableMovementAwareness = false;
	Responsive.bExposeMovementState = false;
	Responsive.StartedMovingResponse = EC_RunLLMOption::Auto;
	Responsive.Delivery = EConvaiContextDelivery::WaitUntilConversationIsIdle;
	Responsive.bFlushImmediately = true;
	Responsive.NormalizeResponseSettings();
	TestEqual(TEXT("Temporarily disabling awareness preserves authored delivery"),
		Responsive.Delivery,
		EConvaiContextDelivery::WaitUntilConversationIsIdle);
	TestTrue(TEXT("Temporarily disabling awareness preserves authored flushing"),
		Responsive.bFlushImmediately);
	TestEqual(TEXT("Disabled state has normal effective delivery"),
		Responsive.GetEffectiveDelivery(),
		EConvaiContextDelivery::SendNormally);
	TestFalse(TEXT("Disabled state cannot effectively flush"),
		Responsive.GetEffectiveFlushImmediately());
	Responsive.bEnableMovementAwareness = true;
	Responsive.bExposeMovementState = true;
	TestEqual(TEXT("Re-enabling restores authored effective delivery"),
		Responsive.GetEffectiveDelivery(),
		EConvaiContextDelivery::WaitUntilConversationIsIdle);
	TestTrue(TEXT("Re-enabling restores authored effective flushing"),
		Responsive.GetEffectiveFlushImmediately());

	UConvaiObjectComponent* Component =
		NewObject<UConvaiObjectComponent>(GetTransientPackage());
	TestNotNull(TEXT("Transient movement-aware object is created"), Component);
	if (Component)
	{
		Component->ObjectEntry.Name = TEXT("Platform");
		Component->MovementAwareness.bExposeMovementState = true;
		TArray<FString> ContextKeys;
		Component->AppendTrackedPropertyContextKeys(ContextKeys);
		TestTrue(TEXT("Add Movement State exposes the watchable key"),
			ContextKeys.Contains(TEXT("Platform.Movement")));

		Component->MovementAwareness.bEnableMovementAwareness = false;
		ContextKeys.Reset();
		Component->AppendTrackedPropertyContextKeys(ContextKeys);
		TestFalse(TEXT("Master switch removes the watchable movement key"),
			ContextKeys.Contains(TEXT("Platform.Movement")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
