// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "RestAPI/ConvaiURL.h"
#include "../Convai.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ConvaiURLTestHelpers
{
	/**
	 * Swaps CustomProdURL for the duration of a scope and puts the user's value back.
	 * Outside Shipping, InitializeURLConfig re-reads the setting on every lookup, so
	 * assigning here is enough to redirect the very next GetEndpoint call.
	 */
	struct FScopedCustomProdURL
	{
		explicit FScopedCustomProdURL(const FString& NewValue)
			: Settings(Convai::Get().GetConvaiSettings())
			, Saved(Settings->CustomProdURL)
		{
			Settings->CustomProdURL = NewValue;
		}

		~FScopedCustomProdURL()
		{
			Settings->CustomProdURL = Saved;
		}

		UConvaiSettings* Settings;
		FString Saved;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiURLEndpointHonorsCustomProdURLTest,
	"Convai.URL.EndpointHonorsCustomProdURL",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiURLEndpointHonorsCustomProdURLTest::RunTest(const FString& Parameters)
{
	{
		const ConvaiURLTestHelpers::FScopedCustomProdURL Override(TEXT("https://api.staging.example.com"));

		TestEqual(TEXT("An enum endpoint follows the custom prod URL"),
			UConvaiURL::GetEndpoint(EConvaiEndpoint::KnowledgeBankUpload),
			FString(TEXT("https://api.staging.example.com/character/knowledge-bank/upload")));

		TestEqual(TEXT("A speaker endpoint follows the custom prod URL"),
			UConvaiURL::GetEndpoint(EConvaiEndpoint::NewSpeaker),
			FString(TEXT("https://api.staging.example.com/user/speaker/new")));
	}

	// Restoring the (empty) setting has to drop the override, not keep serving it.
	TestEqual(TEXT("Clearing the custom prod URL restores the default host"),
		UConvaiURL::GetEndpoint(EConvaiEndpoint::KnowledgeBankUpload),
		FString(TEXT("https://api.convai.com/character/knowledge-bank/upload")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiURLEndpointSlashHandlingTest,
	"Convai.URL.EndpointSlashHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiURLEndpointSlashHandlingTest::RunTest(const FString& Parameters)
{
	{
		const ConvaiURLTestHelpers::FScopedCustomProdURL Override(TEXT("https://api.staging.example.com/"));
		TestEqual(TEXT("A trailing slash on the custom URL does not double up"),
			UConvaiURL::GetEndpoint(EConvaiEndpoint::CharacterUpdate),
			FString(TEXT("https://api.staging.example.com/character/update")));
	}

	{
		const ConvaiURLTestHelpers::FScopedCustomProdURL Override(TEXT("https://gateway.example.com/convai"));
		TestEqual(TEXT("A path prefix on the custom URL is kept"),
			UConvaiURL::GetEndpoint(EConvaiEndpoint::CharacterUpdate),
			FString(TEXT("https://gateway.example.com/convai/character/update")));
	}

	{
		const ConvaiURLTestHelpers::FScopedCustomProdURL Override(TEXT("  https://api.staging.example.com  "));
		TestEqual(TEXT("Surrounding whitespace on the custom URL is trimmed"),
			UConvaiURL::GetEndpoint(EConvaiEndpoint::CharacterUpdate),
			FString(TEXT("https://api.staging.example.com/character/update")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiURLEndpointAgreesWithFullURLTest,
	"Convai.URL.EndpointAgreesWithGetFullURL",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiURLEndpointAgreesWithFullURLTest::RunTest(const FString& Parameters)
{
	// UConvaiSetLTMStatus and UConvaiSetKnowledgeBankDocumentStatus reach character/update
	// through the enum, while ConvaiChatBotProxy builds the same path as a literal. The two
	// have to resolve to one host, custom URL or not.
	const ConvaiURLTestHelpers::FScopedCustomProdURL Override(TEXT("https://api.staging.example.com"));

	TestEqual(TEXT("The enum and literal paths to character/update agree"),
		UConvaiURL::GetEndpoint(EConvaiEndpoint::CharacterUpdate),
		UConvaiURL::GetFullURL(TEXT("character/update"), false));

	TestEqual(TEXT("The enum and literal paths to character/get agree"),
		UConvaiURL::GetEndpoint(EConvaiEndpoint::CharacterGet),
		UConvaiURL::GetFullURL(TEXT("character/get"), false));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
