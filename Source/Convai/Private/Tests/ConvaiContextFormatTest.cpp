// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "Utility/ConvaiContextFormat.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Test: SanitizeKey — whitespace Pascal-casing, per dot-segment
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCtxFormatTest_SanitizeKey,
	"Convai.DynamicContext.Format.SanitizeKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCtxFormatTest_SanitizeKey::RunTest(const FString& Parameters)
{
	using namespace ConvaiContextFormat;

	TestEqual(TEXT("No whitespace is untouched"), SanitizeKey(TEXT("FrontDoor.bLocked")), TEXT("FrontDoor.bLocked"));
	TestEqual(TEXT("Every word capitalized, first included"), SanitizeKey(TEXT("door state")), TEXT("DoorState"));
	TestEqual(TEXT("Already-capitalized words kept"), SanitizeKey(TEXT("Front Door")), TEXT("FrontDoor"));
	TestEqual(TEXT("Per-segment"), SanitizeKey(TEXT("Front Door.door state")), TEXT("FrontDoor.DoorState"));
	TestEqual(TEXT("Whitespace-free segment untouched next to spaced one"), SanitizeKey(TEXT("bLocked.door state")), TEXT("bLocked.DoorState"));
	TestEqual(TEXT("Multiple spaces collapse"), SanitizeKey(TEXT("a   b  c")), TEXT("ABC"));
	TestEqual(TEXT("Leading whitespace dropped"), SanitizeKey(TEXT("  door state")), TEXT("DoorState"));
	TestEqual(TEXT("Trailing whitespace dropped (segment had whitespace, so Pascal-cased)"), SanitizeKey(TEXT("door ")), TEXT("Door"));
	TestEqual(TEXT("Tabs count as whitespace"), SanitizeKey(TEXT("door\tstate")), TEXT("DoorState"));
	TestEqual(TEXT("Empty stays empty"), SanitizeKey(TEXT("")), TEXT(""));
	return true;
}

// ---------------------------------------------------------------------------
// Test: FormatValueForPrompt — quoting and escaping
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCtxFormatTest_FormatValue,
	"Convai.DynamicContext.Format.FormatValueForPrompt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCtxFormatTest_FormatValue::RunTest(const FString& Parameters)
{
	using namespace ConvaiContextFormat;

	TestEqual(TEXT("Single word stays bare"), FormatValueForPrompt(TEXT("true")), TEXT("true"));
	TestEqual(TEXT("Number stays bare"), FormatValueForPrompt(TEXT("80")), TEXT("80"));
	TestEqual(TEXT("Multi-word quoted"), FormatValueForPrompt(TEXT("3:30 PM")), TEXT("\"3:30 PM\""));
	TestEqual(TEXT("Empty becomes empty quotes"), FormatValueForPrompt(TEXT("")), TEXT("\"\""));
	TestEqual(TEXT("Embedded quote escaped"), FormatValueForPrompt(TEXT("he said \"hi\" twice")), TEXT("\"he said \\\"hi\\\" twice\""));
	TestEqual(TEXT("Newline escaped"), FormatValueForPrompt(TEXT("line one\nline two")), TEXT("\"line one\\nline two\""));
	TestEqual(TEXT("CRLF folds to \\n"), FormatValueForPrompt(TEXT("a\r\nb c")), TEXT("\"a\\nb c\""));
	TestEqual(TEXT("Backslash escaped when quoting"), FormatValueForPrompt(TEXT("path \\ two")), TEXT("\"path \\\\ two\""));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
