// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "ConvaiSubsystem.h"

#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiServerErrorSeverityTest,
	"Convai.ServerError.FatalFieldDecidesSeverity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiServerErrorSeverityTest::RunTest(const FString& Parameters)
{
	// Severity is decided at the parse site and cannot be read back without a live session, so
	// the log is the oracle: UConvaiSubsystem::OnError is the only Error-verbosity line on this
	// path, and an unexpected one fails the test. The packet sink is private on the subsystem;
	// the DLL listener it overrides is public. Both directions are pinned — a regression to a
	// hardcoded fatal reds the first packet, over-correcting to non-fatal-by-default reds the
	// second.
	// ClassWithin is UGameInstance, so the outer has to be one; it is never initialized, and
	// nothing on this path reaches for it.
	TStrongObjectPtr<UGameInstance> Owner(NewObject<UGameInstance>(GetTransientPackage()));
	TStrongObjectPtr<UConvaiSubsystem> Subsystem(NewObject<UConvaiSubsystem>(Owner.Get()));
	convai::IConvaiClientListner& Listener = *Subsystem.Get();

	AddExpectedMessagePlain(TEXT("convai-tests-severity-absent"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);

	Listener.OnDataPacketReceived(
		R"({"label":"rtvi-ai","type":"error","data":{"error":"convai-tests-severity-false","fatal":false}})",
		"convai-bot");
	Listener.OnDataPacketReceived(
		R"({"label":"rtvi-ai","type":"error","data":{"error":"convai-tests-severity-absent"}})",
		"convai-bot");

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
