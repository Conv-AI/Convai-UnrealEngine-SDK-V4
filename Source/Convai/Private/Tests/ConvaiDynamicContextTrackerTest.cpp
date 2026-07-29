// Copyright 2022 Convai Inc. All Rights Reserved.

#include "DynamicContext/ConvaiDynamicContextTracker.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Convenience: split canonical context into lines for element-wise comparison.
static TArray<FString> SplitLines(const FString& Text)
{
	TArray<FString> Lines;
	Text.ParseIntoArray(Lines, TEXT("\n"), /* bCullEmpty */ false);
	return Lines;
}

// ---------------------------------------------------------------------------
// Test: Empty tracker produces empty context
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_EmptyContext,
	"Convai.DynamicContext.Tracker.EmptyContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_EmptyContext::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	TestTrue(TEXT("Empty tracker builds empty string"), Tracker.BuildCanonicalContext().IsEmpty());
	return true;
}

// ---------------------------------------------------------------------------
// Test: Single state property
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_SingleState,
	"Convai.DynamicContext.Tracker.SingleState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_SingleState::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	const bool bExisted = Tracker.SetState(TEXT("Health"), TEXT("80"));

	TestFalse(TEXT("First set should report new"), bExisted);
	TestEqual(TEXT("Canonical context"), Tracker.BuildCanonicalContext(), TEXT("Health is 80"));
	return true;
}

// ---------------------------------------------------------------------------
// Test: Multiple state properties preserve insertion order
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_MultipleStatesOrder,
	"Convai.DynamicContext.Tracker.MultipleStatesOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_MultipleStatesOrder::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Time"), TEXT("3:30 PM"));
	Tracker.SetState(TEXT("Weather"), TEXT("Sunny"));
	Tracker.SetState(TEXT("Health"), TEXT("80"));
	Tracker.SetState(TEXT("Ammo"), TEXT("12"));

	// Multi-word values are quoted at render time; single words stay bare.
	const FString Expected =
		TEXT("Time is \"3:30 PM\"\n")
		TEXT("Weather is Sunny\n")
		TEXT("Health is 80\n")
		TEXT("Ammo is 12");

	TestEqual(TEXT("Canonical context matches expected"), Tracker.BuildCanonicalContext(), Expected);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Updating an existing state replaces value, not duplicates
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_UpdateExistingState,
	"Convai.DynamicContext.Tracker.UpdateExistingState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_UpdateExistingState::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Health"), TEXT("100"));
	Tracker.SetState(TEXT("Ammo"), TEXT("30"));

	FString OldValue;
	const bool bExisted = Tracker.SetState(TEXT("Health"), TEXT("50"), &OldValue);

	TestTrue(TEXT("Second set should report existing"), bExisted);
	TestEqual(TEXT("Old value captured"), OldValue, TEXT("100"));

	const TArray<FString> Lines = SplitLines(Tracker.BuildCanonicalContext());
	TestEqual(TEXT("Still only two lines"), Lines.Num(), 2);
	TestEqual(TEXT("Health updated"), Lines[0], TEXT("Health is 50"));
	TestEqual(TEXT("Ammo unchanged"), Lines[1], TEXT("Ammo is 30"));
	return true;
}

// ---------------------------------------------------------------------------
// Test: Events accumulate in chronological order
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_EventsChronological,
	"Convai.DynamicContext.Tracker.EventsChronological",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_EventsChronological::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.AddEvent(TEXT("Player approached the merchant"));
	Tracker.AddEvent(TEXT("Player looked at the sword"));
	Tracker.AddEvent(TEXT("Alarm went off"));

	const FString Expected =
		TEXT("Player approached the merchant\n")
		TEXT("Player looked at the sword\n")
		TEXT("Alarm went off");

	TestEqual(TEXT("Events in order"), Tracker.BuildCanonicalContext(), Expected);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Mixed states and events — states first, then events
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_StatesBeforeEvents,
	"Convai.DynamicContext.Tracker.StatesBeforeEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_StatesBeforeEvents::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;

	// Interleave adds — order of calls shouldn't matter for grouping
	Tracker.SetState(TEXT("Time"), TEXT("3:30 PM"));
	Tracker.AddEvent(TEXT("Player approached the merchant"));
	Tracker.SetState(TEXT("Weather"), TEXT("Sunny"));
	Tracker.AddEvent(TEXT("Player looked at the sword"));
	Tracker.SetState(TEXT("Health"), TEXT("80"));
	Tracker.SetState(TEXT("Ammo"), TEXT("12"));
	Tracker.AddEvent(TEXT("Alarm went off"));

	const FString Expected =
		TEXT("Time is \"3:30 PM\"\n")
		TEXT("Weather is Sunny\n")
		TEXT("Health is 80\n")
		TEXT("Ammo is 12\n")
		TEXT("Player approached the merchant\n")
		TEXT("Player looked at the sword\n")
		TEXT("Alarm went off");

	TestEqual(TEXT("States grouped before events"), Tracker.BuildCanonicalContext(), Expected);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Remove state property
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_RemoveState,
	"Convai.DynamicContext.Tracker.RemoveState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_RemoveState::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Health"), TEXT("80"));
	Tracker.SetState(TEXT("Ammo"), TEXT("12"));
	Tracker.SetState(TEXT("Weather"), TEXT("Sunny"));

	const bool bRemoved = Tracker.RemoveState(TEXT("Ammo"));
	TestTrue(TEXT("Remove existing returns true"), bRemoved);

	const bool bRemovedAgain = Tracker.RemoveState(TEXT("Ammo"));
	TestFalse(TEXT("Remove nonexistent returns false"), bRemovedAgain);

	const FString Expected =
		TEXT("Health is 80\n")
		TEXT("Weather is Sunny");
	TestEqual(TEXT("Context without removed state"), Tracker.BuildCanonicalContext(), Expected);
	return true;
}

// ---------------------------------------------------------------------------
// Test: GetStateValue
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_GetStateValue,
	"Convai.DynamicContext.Tracker.GetStateValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_GetStateValue::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Health"), TEXT("80"));

	FString Value;
	TestTrue(TEXT("Get existing returns true"), Tracker.GetStateValue(TEXT("Health"), Value));
	TestEqual(TEXT("Value matches"), Value, TEXT("80"));

	FString Missing;
	TestFalse(TEXT("Get missing returns false"), Tracker.GetStateValue(TEXT("Mana"), Missing));
	return true;
}

// ---------------------------------------------------------------------------
// Test: Reset clears everything
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_Reset,
	"Convai.DynamicContext.Tracker.Reset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_Reset::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Health"), TEXT("80"));
	Tracker.SetState(TEXT("Ammo"), TEXT("12"));
	Tracker.AddEvent(TEXT("Alarm went off"));

	Tracker.Reset();
	TestTrue(TEXT("After reset, context is empty"), Tracker.BuildCanonicalContext().IsEmpty());

	FString Value;
	TestFalse(TEXT("State gone after reset"), Tracker.GetStateValue(TEXT("Health"), Value));
	return true;
}

// ---------------------------------------------------------------------------
// Test: State update preserves insertion position (not re-appended)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_UpdatePreservesPosition,
	"Convai.DynamicContext.Tracker.UpdatePreservesPosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_UpdatePreservesPosition::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Health"), TEXT("100"));
	Tracker.SetState(TEXT("Ammo"), TEXT("30"));
	Tracker.SetState(TEXT("Weather"), TEXT("Sunny"));

	// Update middle key
	Tracker.SetState(TEXT("Ammo"), TEXT("25"));

	const TArray<FString> Lines = SplitLines(Tracker.BuildCanonicalContext());
	TestEqual(TEXT("Three lines"), Lines.Num(), 3);
	TestEqual(TEXT("Health still first"), Lines[0], TEXT("Health is 100"));
	TestEqual(TEXT("Ammo still second (updated)"), Lines[1], TEXT("Ammo is 25"));
	TestEqual(TEXT("Weather still third"), Lines[2], TEXT("Weather is Sunny"));
	return true;
}

// ---------------------------------------------------------------------------
// Test: Full scenario — the spec example
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_FullScenario,
	"Convai.DynamicContext.Tracker.FullScenario",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_FullScenario::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;

	// Initial state
	Tracker.SetState(TEXT("Time"), TEXT("3:30 PM"));
	Tracker.SetState(TEXT("Weather"), TEXT("Sunny"));
	Tracker.SetState(TEXT("Health"), TEXT("80"));
	Tracker.SetState(TEXT("Ammo"), TEXT("12"));

	// Events
	Tracker.AddEvent(TEXT("Player approached the merchant"));
	Tracker.AddEvent(TEXT("Player looked at the sword"));
	Tracker.AddEvent(TEXT("Alarm went off"));

	const FString Expected =
		TEXT("Time is \"3:30 PM\"\n")
		TEXT("Weather is Sunny\n")
		TEXT("Health is 80\n")
		TEXT("Ammo is 12\n")
		TEXT("Player approached the merchant\n")
		TEXT("Player looked at the sword\n")
		TEXT("Alarm went off");

	TestEqual(TEXT("Full scenario matches spec example"), Tracker.BuildCanonicalContext(), Expected);

	// Now update a state — should replace, not duplicate
	Tracker.SetState(TEXT("Health"), TEXT("40"));
	const FString AfterUpdate =
		TEXT("Time is \"3:30 PM\"\n")
		TEXT("Weather is Sunny\n")
		TEXT("Health is 40\n")
		TEXT("Ammo is 12\n")
		TEXT("Player approached the merchant\n")
		TEXT("Player looked at the sword\n")
		TEXT("Alarm went off");

	TestEqual(TEXT("After state update, no duplicate"), Tracker.BuildCanonicalContext(), AfterUpdate);

	// Remove a state
	Tracker.RemoveState(TEXT("Ammo"));
	const FString AfterRemove =
		TEXT("Time is \"3:30 PM\"\n")
		TEXT("Weather is Sunny\n")
		TEXT("Health is 40\n")
		TEXT("Player approached the merchant\n")
		TEXT("Player looked at the sword\n")
		TEXT("Alarm went off");

	TestEqual(TEXT("After state removal"), Tracker.BuildCanonicalContext(), AfterRemove);

	// Reset
	Tracker.Reset();
	TestTrue(TEXT("After reset, empty"), Tracker.BuildCanonicalContext().IsEmpty());
	return true;
}

// ---------------------------------------------------------------------------
// Test: Events persist after state removal (events are independent)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_EventsPersistAfterStateRemoval,
	"Convai.DynamicContext.Tracker.EventsPersistAfterStateRemoval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_EventsPersistAfterStateRemoval::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Health"), TEXT("80"));
	Tracker.AddEvent(TEXT("Door opened"));
	Tracker.RemoveState(TEXT("Health"));

	TestEqual(TEXT("Only event remains"), Tracker.BuildCanonicalContext(), TEXT("Door opened"));
	return true;
}

// ---------------------------------------------------------------------------
// Test: SetState with OldValue output pointer is optional
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDynCtxTest_SetStateNoOldValuePtr,
	"Convai.DynamicContext.Tracker.SetStateNoOldValuePtr",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDynCtxTest_SetStateNoOldValuePtr::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Health"), TEXT("100"));

	// Update without providing OldValue pointer — should not crash
	const bool bExisted = Tracker.SetState(TEXT("Health"), TEXT("50"));
	TestTrue(TEXT("Existed without old value ptr"), bExisted);
	TestEqual(TEXT("Value updated"), Tracker.BuildCanonicalContext(), TEXT("Health is 50"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
