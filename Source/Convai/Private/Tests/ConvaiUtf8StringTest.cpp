// Copyright Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "Utility/ConvaiUtf8String.h"
#include "Environment/ConvaiEnvironment.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiUtf8LargePayloadTest,
	"Convai.Connection.UTF8.LargePayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FConvaiUtf8LargePayloadTest::RunTest(const FString& Parameters)
{
	const FString LargePayload = FString::ChrN(32 * 1024, TEXT('x'));
	const TArray<ANSICHAR> Bytes = ConvaiUtf8::ConvertToNullTerminatedBytes(LargePayload);

	TestEqual(
		TEXT("Conversion preserves every byte beyond the former 4 KiB limit"),
		Bytes.Num(),
		LargePayload.Len() + 1);
	TestEqual(TEXT("The converted payload is explicitly null terminated"), Bytes.Last(), '\0');
	TestEqual(
		TEXT("The converted payload is not truncated"),
		FCStringAnsi::Strlen(Bytes.GetData()),
		LargePayload.Len());
	const FUTF8ToTCHAR LargeRoundTripConverter(Bytes.GetData(), Bytes.Num() - 1);
	const FString LargeRoundTrip(
		LargeRoundTripConverter.Length(),
		LargeRoundTripConverter.Get());
	TestEqual(TEXT("Every large-payload byte round-trips"), LargeRoundTrip, LargePayload);

	const TArray<ANSICHAR> EmptyBytes =
		ConvaiUtf8::ConvertToNullTerminatedBytes(FString());
	TestEqual(TEXT("An empty string still owns one terminator"), EmptyBytes.Num(), 1);
	TestEqual(TEXT("The empty representation is null terminated"), EmptyBytes[0], '\0');

	const FString MultibyteText = TEXT("\u0645\u062a\u062d\u0641 \u0631\u0648\u0645\u0627\u0646\u064a \u2014 gallery");
	const TArray<ANSICHAR> MultibyteBytes =
		ConvaiUtf8::ConvertToNullTerminatedBytes(MultibyteText);
	TestTrue(
		TEXT("Multibyte UTF-8 uses more bytes than TCHAR code units"),
		MultibyteBytes.Num() - 1 > MultibyteText.Len());
	const FUTF8ToTCHAR RoundTripConverter(
		MultibyteBytes.GetData(),
		MultibyteBytes.Num() - 1);
	const FString RoundTripText(RoundTripConverter.Length(), RoundTripConverter.Get());
	TestEqual(TEXT("Multibyte content round-trips exactly"), RoundTripText, MultibyteText);

	FConvaiEnvironmentData Environment;
	Environment.Objects.Reset();
	for (int32 Index = 0; Index < 64; ++Index)
	{
		FConvaiObjectEntry& Entry = Environment.Objects.AddDefaulted_GetRef();
		Entry.Name = FString::Printf(TEXT("Museum object %d"), Index + 1);
		Entry.Description = FString::Printf(
			TEXT("\u0642\u0637\u0639\u0629 \u0645\u0639\u0631\u0648\u0636\u0629 %d \u2014 a deliberately detailed museum object description used to verify large scene payload transport."),
			Index + 1);
	}
	const FString ActionConfigJson = Environment.ToActionConfigJson();
	const TArray<ANSICHAR> ActionConfigBytes =
		ConvaiUtf8::ConvertToNullTerminatedBytes(ActionConfigJson);
	TestTrue(
		TEXT("A realistic 64-object action config exceeds the former 4 KiB cap"),
		ActionConfigBytes.Num() - 1 > 4096);
	const FUTF8ToTCHAR ActionConfigRoundTripConverter(
		ActionConfigBytes.GetData(),
		ActionConfigBytes.Num() - 1);
	const FString ActionConfigRoundTrip(
		ActionConfigRoundTripConverter.Length(),
		ActionConfigRoundTripConverter.Get());
	TestEqual(
		TEXT("The serialized action config survives the production UTF-8 representation"),
		ActionConfigRoundTrip,
		ActionConfigJson);

	TSharedPtr<FJsonObject> ParsedActionConfig;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ActionConfigRoundTrip);
	TestTrue(
		TEXT("The large action config remains valid JSON"),
		FJsonSerializer::Deserialize(Reader, ParsedActionConfig) && ParsedActionConfig.IsValid());
	if (ParsedActionConfig.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
		TestTrue(
			TEXT("The parsed action config retains its objects array"),
			ParsedActionConfig->TryGetArrayField(TEXT("objects"), Objects) && Objects != nullptr);
		if (Objects)
		{
			TestEqual(TEXT("All scene objects survive serialization"), Objects->Num(), 64);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
