// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "DynamicContext/ConvaiPendingContextBatch.h"
#include "DynamicContext/ConvaiDynamicContextTracker.h"
#include "Utility/ConvaiContextFormat.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Test: Held state — last value wins, strongest ShouldRespond wins
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeldLaneTest_StateLastValueMaxRespond,
	"Convai.DynamicContext.HeldLane.StateLastValueMaxRespond",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeldLaneTest_StateLastValueMaxRespond::RunTest(const FString& Parameters)
{
	FConvaiHeldContextLane Lane;
	Lane.HoldState(TEXT("Health"), TEXT("10"), EC_RunLLMOption::Always);
	Lane.HoldState(TEXT("Health"), TEXT("20"), EC_RunLLMOption::Auto);

	TestEqual(TEXT("One ordered key"), Lane.StateOrder.Num(), 1);
	TestEqual(TEXT("Last value wins"), Lane.StateValues[TEXT("Health")], TEXT("20"));
	TestEqual(TEXT("Strongest respond survives a weaker overwrite"),
		Lane.StateRespond[TEXT("Health")], EC_RunLLMOption::Always);

	// And the symmetric direction: weak first, strong second.
	Lane.HoldState(TEXT("Mood"), TEXT("calm"), EC_RunLLMOption::Auto);
	Lane.HoldState(TEXT("Mood"), TEXT("angry"), EC_RunLLMOption::Always);
	TestEqual(TEXT("Upgrade to the stronger respond"),
		Lane.StateRespond[TEXT("Mood")], EC_RunLLMOption::Always);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Held facts and attention — same last-value / max-rank rules
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeldLaneTest_FactAndAttentionMerge,
	"Convai.DynamicContext.HeldLane.FactAndAttentionMerge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeldLaneTest_FactAndAttentionMerge::RunTest(const FString& Parameters)
{
	FConvaiHeldContextLane Lane;
	Lane.HoldDeclarative(TEXT("chest"), TEXT("The chest is locked"), EC_RunLLMOption::Always);
	Lane.HoldDeclarative(TEXT("chest"), TEXT("The chest is open"), EC_RunLLMOption::Never);
	TestEqual(TEXT("Fact: last sentence wins"),
		Lane.DeclarativeSentences[TEXT("chest")], TEXT("The chest is open"));
	TestEqual(TEXT("Fact: strongest respond survives"),
		Lane.DeclarativeRespond[TEXT("chest")], EC_RunLLMOption::Always);

	FConvaiObjectEntry Vase;  Vase.Name = TEXT("Vase");
	FConvaiObjectEntry Door;  Door.Name = TEXT("Door");
	Lane.HoldAttention(Vase, TEXT("shiny"), EC_RunLLMOption::Always, /*bAddAttentionEvent*/ true, /*bFromGaze*/ true);
	Lane.HoldAttention(Door, TEXT(""), EC_RunLLMOption::Auto, /*bAddAttentionEvent*/ false, /*bFromGaze*/ false);
	TestEqual(TEXT("Attention: last object wins"), Lane.AttentionObject.Name, TEXT("Door"));
	TestEqual(TEXT("Attention: strongest respond survives"),
		Lane.AttentionRespond, EC_RunLLMOption::Always);
	TestFalse(TEXT("Attention: latest call's gaze flag wins"), Lane.bAttentionFromGaze);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Duplicate held event rank-merges instead of dropping the upgrade
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeldLaneTest_DuplicateEventUpgrades,
	"Convai.DynamicContext.HeldLane.DuplicateEventUpgrades",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeldLaneTest_DuplicateEventUpgrades::RunTest(const FString& Parameters)
{
	FConvaiHeldContextLane Lane;
	Lane.HoldEvent(TEXT("The alarm rang"), EC_RunLLMOption::Auto, /*bEphemeral*/ false);
	Lane.HoldEvent(TEXT("The alarm rang"), EC_RunLLMOption::Always, /*bEphemeral*/ false);

	TestEqual(TEXT("Deduped to one event"), Lane.Events.Num(), 1);
	TestEqual(TEXT("Duplicate upgraded Auto to Always"),
		Lane.Events[0].ShouldRespond, EC_RunLLMOption::Always);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Persistent and ephemeral events with identical text stay distinct
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeldLaneTest_EphemeralIdentityDistinct,
	"Convai.DynamicContext.HeldLane.EphemeralIdentityDistinct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeldLaneTest_EphemeralIdentityDistinct::RunTest(const FString& Parameters)
{
	FConvaiHeldContextLane Lane;
	Lane.HoldEvent(TEXT("X"), EC_RunLLMOption::Auto, /*bEphemeral*/ false);
	Lane.HoldEvent(TEXT("X"), EC_RunLLMOption::Auto, /*bEphemeral*/ true);
	TestEqual(TEXT("Same text, different ephemerality = two events"), Lane.Events.Num(), 2);

	// DropEvent removes only the matching identity.
	Lane.DropEvent(TEXT("X"), /*bEphemeral*/ false);
	TestEqual(TEXT("Only the persistent one dropped"), Lane.Events.Num(), 1);
	TestTrue(TEXT("Survivor is the ephemeral one"), Lane.Events[0].bEphemeral);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Dropping the final payload resets the release controls
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeldLaneTest_DropLastResetsControls,
	"Convai.DynamicContext.HeldLane.DropLastResetsControls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeldLaneTest_DropLastResetsControls::RunTest(const FString& Parameters)
{
	FConvaiHeldContextLane Lane;
	FConvaiObjectEntry Vase;  Vase.Name = TEXT("Vase");
	Lane.HoldAttention(Vase, TEXT(""), EC_RunLLMOption::Auto, true, /*bFromGaze*/ true);
	Lane.IdleSinceTime = 123.0;
	Lane.bReleaseAsSoonAsIdle = true;

	// Look-away cancels the only payload — a stale idle clock and a stale
	// urgency flag must not leak onto the next unrelated held item.
	Lane.DropAttention();
	TestFalse(TEXT("Lane is empty"), Lane.HasWork());
	TestEqual(TEXT("Idle clock reset"), Lane.IdleSinceTime, -1.0);
	TestFalse(TEXT("Urgency flag reset"), Lane.bReleaseAsSoonAsIdle);

	// Same via state / fact / event drops.
	Lane.HoldState(TEXT("K"), TEXT("V"), EC_RunLLMOption::Auto);
	Lane.IdleSinceTime = 55.0;
	Lane.DropStateKey(TEXT("K"));
	TestEqual(TEXT("State drop reset the idle clock"), Lane.IdleSinceTime, -1.0);

	Lane.HoldEvent(TEXT("E"), EC_RunLLMOption::Auto, false);
	Lane.bReleaseAsSoonAsIdle = true;
	Lane.DropEvent(TEXT("E"), false);
	TestFalse(TEXT("Event drop reset the urgency flag"), Lane.bReleaseAsSoonAsIdle);

	Lane.HoldDeclarative(TEXT("chest"), TEXT("The chest is locked"), EC_RunLLMOption::Auto);
	Lane.IdleSinceTime = 99.0;
	Lane.DropDeclarativeKey(TEXT("chest"));
	TestEqual(TEXT("Fact drop reset the idle clock"), Lane.IdleSinceTime, -1.0);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Quiet-period release gate (UpdateIdleAndCheckRelease)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeldLaneTest_QuietPeriodGate,
	"Convai.DynamicContext.HeldLane.QuietPeriodGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeldLaneTest_QuietPeriodGate::RunTest(const FString& Parameters)
{
	FConvaiHeldContextLane Lane;
	Lane.HoldEvent(TEXT("You have arrived"), EC_RunLLMOption::Always, /*bEphemeral*/ true);

	// Continuous quiet reaches the threshold; the clock starts at first idle.
	TestFalse(TEXT("Idle t=100: quiet just started"), Lane.UpdateIdleAndCheckRelease(true, true, 100.0, 2.0));
	TestFalse(TEXT("Idle t=101: still inside quiet period"), Lane.UpdateIdleAndCheckRelease(true, true, 101.0, 2.0));
	TestTrue(TEXT("Idle t=102: quiet period reached"), Lane.UpdateIdleAndCheckRelease(true, true, 102.0, 2.0));

	// Activity resets the clock — the wait restarts on the next idle.
	TestFalse(TEXT("Active t=103 resets"), Lane.UpdateIdleAndCheckRelease(false, true, 103.0, 2.0));
	TestFalse(TEXT("Idle t=104: fresh quiet stretch"), Lane.UpdateIdleAndCheckRelease(true, true, 104.0, 2.0));
	TestFalse(TEXT("Idle t=105.9: not yet"), Lane.UpdateIdleAndCheckRelease(true, true, 105.9, 2.0));
	TestTrue(TEXT("Idle t=106: released"), Lane.UpdateIdleAndCheckRelease(true, true, 106.0, 2.0));

	// A disconnect must not count as observed quiet time.
	TestFalse(TEXT("Disconnected t=107 resets"), Lane.UpdateIdleAndCheckRelease(true, false, 107.0, 2.0));
	TestFalse(TEXT("Reconnected idle t=108: fresh stretch"), Lane.UpdateIdleAndCheckRelease(true, true, 108.0, 2.0));
	TestTrue(TEXT("Idle t=110: released"), Lane.UpdateIdleAndCheckRelease(true, true, 110.0, 2.0));

	// Urgent lanes release at the FIRST idle instant, but never while active.
	Lane.bReleaseAsSoonAsIdle = true;
	TestFalse(TEXT("Urgent but active: no release"), Lane.UpdateIdleAndCheckRelease(false, true, 111.0, 2.0));
	TestTrue(TEXT("Urgent first idle: releases"), Lane.UpdateIdleAndCheckRelease(true, true, 112.0, 2.0));
	Lane.bReleaseAsSoonAsIdle = false;

	// Zero quiet time releases at the first idle instant.
	TestFalse(TEXT("Active resets again"), Lane.UpdateIdleAndCheckRelease(false, true, 113.0, 0.0));
	TestTrue(TEXT("Zero delay: first idle releases"), Lane.UpdateIdleAndCheckRelease(true, true, 114.0, 0.0));

	// An empty lane never releases and keeps the clock reset.
	Lane.Clear();
	TestFalse(TEXT("Empty lane: no release"), Lane.UpdateIdleAndCheckRelease(true, true, 115.0, 0.0));
	TestEqual(TEXT("Empty lane: clock reset"), Lane.IdleSinceTime, -1.0);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Dropping one item while other work remains preserves the controls
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeldLaneTest_PartialDropKeepsControls,
	"Convai.DynamicContext.HeldLane.PartialDropKeepsControls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeldLaneTest_PartialDropKeepsControls::RunTest(const FString& Parameters)
{
	FConvaiHeldContextLane Lane;
	Lane.HoldState(TEXT("A"), TEXT("1"), EC_RunLLMOption::Auto);
	Lane.HoldEvent(TEXT("E"), EC_RunLLMOption::Auto, false);
	Lane.IdleSinceTime = 77.0;
	Lane.bReleaseAsSoonAsIdle = true;

	Lane.DropStateKey(TEXT("A"));
	TestTrue(TEXT("Event still held"), Lane.HasWork());
	TestEqual(TEXT("Idle clock preserved while work remains"), Lane.IdleSinceTime, 77.0);
	TestTrue(TEXT("Urgency flag preserved while work remains"), Lane.bReleaseAsSoonAsIdle);

	// Clear() always resets everything.
	Lane.Clear();
	TestFalse(TEXT("Cleared"), Lane.HasWork());
	TestEqual(TEXT("Clear resets the idle clock"), Lane.IdleSinceTime, -1.0);
	TestFalse(TEXT("Clear resets the urgency flag"), Lane.bReleaseAsSoonAsIdle);
	return true;
}

// ---------------------------------------------------------------------------
// Test: A newer held state withdraws older pending work and restores baseline
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPendingBatchTest_WithdrawStateForHold,
	"Convai.DynamicContext.PendingBatch.WithdrawStateForHold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPendingBatchTest_WithdrawStateForHold::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Platform.Movement"), TEXT("Stopped"));

	FConvaiPendingContextBatch Batch;
	Batch.StageState(Tracker, TEXT("Platform.Movement"), TEXT("Downward"),
		EC_RunLLMOption::Always, /*bOmitPreviousValue*/ true,
		/*PreviousValueOverride*/ nullptr, /*bPreserveTransition*/ true);
	Batch.WatchPromotedStateKeys.Add(TEXT("Platform.Movement"));

	TestTrue(TEXT("Pending state itself requests a response"),
		Batch.StateRequestsResponse(TEXT("Platform.Movement")));
	bool bInheritedWatchPromotion = false;
	TestTrue(TEXT("Existing key is withdrawn"),
		Batch.WithdrawStateForHold(Tracker, TEXT("Platform.Movement"),
			&bInheritedWatchPromotion));
	TestTrue(TEXT("Consumed one-shot watch is handed to the replacement"),
		bInheritedWatchPromotion);

	FString Restored;
	TestTrue(TEXT("Pre-batch state exists after withdrawal"),
		Tracker.GetStateValue(TEXT("Platform.Movement"), Restored));
	TestEqual(TEXT("Pre-batch state value is restored"), Restored, TEXT("Stopped"));
	TestFalse(TEXT("State key is no longer staged"),
		Batch.StagedStateOrder.Contains(TEXT("Platform.Movement")));
	TestFalse(TEXT("Old-value metadata is gone"),
		Batch.OldValues.Contains(TEXT("Platform.Movement")));
	TestFalse(TEXT("Initial-value metadata is gone"),
		Batch.InitialValues.Contains(TEXT("Platform.Movement")));
	TestFalse(TEXT("Transition-preservation metadata is gone"),
		Batch.PreserveTransitionKeys.Contains(TEXT("Platform.Movement")));
	TestFalse(TEXT("Omit-previous metadata is gone"),
		Batch.OmitPreviousValueKeys.Contains(TEXT("Platform.Movement")));
	TestFalse(TEXT("Response rank is gone"),
		Batch.StateRespond.Contains(TEXT("Platform.Movement")));
	TestFalse(TEXT("Watch-promotion metadata is gone"),
		Batch.WatchPromotedStateKeys.Contains(TEXT("Platform.Movement")));
	TestEqual(TEXT("Aggregate response rank is recomputed"),
		Batch.AggregateRunLLM, EC_RunLLMOption::Never);

	FConvaiHeldContextLane Lane;
	Lane.HoldState(TEXT("Platform.Movement"), TEXT("Stopped"),
		bInheritedWatchPromotion
			? EC_RunLLMOption::Always : EC_RunLLMOption::Auto);
	TestEqual(TEXT("Newer value is now owned by the idle lane"),
		Lane.StateValues[TEXT("Platform.Movement")], TEXT("Stopped"));
	TestEqual(TEXT("Idle lane fulfils the consumed watch with its current value"),
		Lane.StateRespond[TEXT("Platform.Movement")], EC_RunLLMOption::Always);
	bInheritedWatchPromotion = true;
	TestFalse(TEXT("Withdrawing a key twice is a no-op"),
		Batch.WithdrawStateForHold(Tracker, TEXT("Platform.Movement"),
			&bInheritedWatchPromotion));
	TestFalse(TEXT("No-op withdrawal resets its watch result"),
		bInheritedWatchPromotion);

	Batch.StageState(Tracker, TEXT("FirstObservation"), TEXT("Visible"),
		EC_RunLLMOption::Auto);
	TestTrue(TEXT("First-appearance key is withdrawn"),
		Batch.WithdrawStateForHold(Tracker, TEXT("FirstObservation")));
	TestFalse(TEXT("First-appearance key is removed from canonical"),
		Tracker.GetStateValue(TEXT("FirstObservation"), Restored));

	FConvaiDynamicContextTracker MixedTracker;
	MixedTracker.SetState(TEXT("Door.State"), TEXT("Closed"));
	FConvaiPendingContextBatch MixedBatch;
	MixedBatch.StageState(MixedTracker, TEXT("Door.State"), TEXT("Open"),
		EC_RunLLMOption::Always);
	MixedBatch.StageEvent(TEXT("The bell rang"), EC_RunLLMOption::Auto);
	MixedBatch.WithdrawStateForHold(MixedTracker, TEXT("Door.State"));
	TestEqual(TEXT("Unrelated event survives keyed state withdrawal"),
		MixedBatch.EventsOrdered.Num(), 1);
	TestEqual(TEXT("Aggregate rank retains the unrelated event"),
		MixedBatch.AggregateRunLLM, EC_RunLLMOption::Auto);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Declarative withdrawal has the same baseline/freshness semantics
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPendingBatchTest_WithdrawDeclarativeForHold,
	"Convai.DynamicContext.PendingBatch.WithdrawDeclarativeForHold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPendingBatchTest_WithdrawDeclarativeForHold::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetDeclarative(TEXT("platform"), TEXT("The platform is stopped."));

	FConvaiPendingContextBatch Batch;
	Batch.StageDeclarative(Tracker, TEXT("platform"),
		TEXT("The platform is moving downward."), EC_RunLLMOption::Always);
	// A second write must not replace the immutable pre-batch snapshot.
	Batch.StageDeclarative(Tracker, TEXT("platform"),
		TEXT("The platform is moving toward you."), EC_RunLLMOption::Never);

	TestEqual(TEXT("Initial declarative snapshot is immutable"),
		Batch.InitialDeclarativeValues[TEXT("platform")],
		TEXT("The platform is stopped."));
	TestTrue(TEXT("Existing declarative is withdrawn"),
		Batch.WithdrawDeclarativeForHold(Tracker, TEXT("platform")));

	FString Restored;
	TestTrue(TEXT("Pre-batch declarative exists after withdrawal"),
		Tracker.GetDeclarativeValue(TEXT("platform"), Restored));
	TestEqual(TEXT("Pre-batch declarative sentence is restored"), Restored,
		TEXT("The platform is stopped."));
	TestFalse(TEXT("Declarative key is no longer staged"),
		Batch.StagedDeclarativeOrder.Contains(TEXT("platform")));
	TestFalse(TEXT("Declarative snapshot metadata is gone"),
		Batch.InitialDeclarativeValues.Contains(TEXT("platform")));
	TestFalse(TEXT("Declarative response rank is gone"),
		Batch.DeclarativeRespond.Contains(TEXT("platform")));
	TestEqual(TEXT("Declarative withdrawal recomputes aggregate rank"),
		Batch.AggregateRunLLM, EC_RunLLMOption::Never);

	FConvaiHeldContextLane Lane;
	Lane.HoldDeclarative(TEXT("platform"), TEXT("The platform is stopped."),
		EC_RunLLMOption::Auto);
	TestEqual(TEXT("Newer fact is now owned by the idle lane"),
		Lane.DeclarativeSentences[TEXT("platform")],
		TEXT("The platform is stopped."));
	TestEqual(TEXT("Idle fact uses only the newer response policy"),
		Lane.DeclarativeRespond[TEXT("platform")], EC_RunLLMOption::Auto);

	Batch.StageDeclarative(Tracker, TEXT("new-fact"),
		TEXT("A new fact."), EC_RunLLMOption::Auto);
	TestTrue(TEXT("First-appearance declarative is withdrawn"),
		Batch.WithdrawDeclarativeForHold(Tracker, TEXT("new-fact")));
	TestFalse(TEXT("First-appearance declarative is removed from canonical"),
		Tracker.GetDeclarativeValue(TEXT("new-fact"), Restored));
	return true;
}

// ---------------------------------------------------------------------------
// Test: A held Movement edge keeps later spatial refreshes in the same lane
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeldContextLaneTest_MovementFactPairing,
	"Convai.DynamicContext.HeldLane.MovementFactPairing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeldContextLaneTest_MovementFactPairing::RunTest(
	const FString& Parameters)
{
	const FString StateKey = TEXT("Platform.Movement");
	const FString FactKey = TEXT("Object:platform");
	const FString Downward = TEXT("Downward");
	const FString Stopped = TEXT("Stopped");

	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(StateKey, Downward);
	Tracker.SetDeclarative(
		FactKey, TEXT("The platform is moving downward."));

	FConvaiHeldContextLane Lane;
	Lane.HoldState(StateKey, Stopped, EC_RunLLMOption::Auto,
		/*bOmitPreviousValue*/ false, &Downward,
		/*bPreserveTransition*/ true);
	Lane.HoldDeclarative(
		FactKey, TEXT("The platform is stopped."), EC_RunLLMOption::Auto);

	// A later nav/relation refresh follows the response rank of the held state.
	Lane.HoldDeclarative(FactKey,
		TEXT("The platform is stopped beside the player."),
		Lane.StateRespond[StateKey]);
	TestEqual(TEXT("Latest stopped sentence remains in the idle lane"),
		Lane.DeclarativeSentences[FactKey],
		TEXT("The platform is stopped beside the player."));
	TestEqual(TEXT("Refreshed fact retains the responsive stop rank"),
		Lane.DeclarativeRespond[FactKey], EC_RunLLMOption::Auto);

	FString CanonicalState;
	FString CanonicalFact;
	TestTrue(TEXT("Canonical moving state remains present while the pair waits"),
		Tracker.GetStateValue(StateKey, CanonicalState));
	TestEqual(TEXT("Held stop does not publish the state early"),
		CanonicalState, Downward);
	TestTrue(TEXT("Canonical moving fact remains present while the pair waits"),
		Tracker.GetDeclarativeValue(FactKey, CanonicalFact));
	TestEqual(TEXT("Held stop does not publish the fact early"), CanonicalFact,
		TEXT("The platform is moving downward."));

	FConvaiHeldContextLane WatchedLane;
	WatchedLane.HoldState(StateKey, Stopped, EC_RunLLMOption::Never,
		/*bOmitPreviousValue*/ false, &Downward,
		/*bPreserveTransition*/ true);
	WatchedLane.HoldDeclarative(
		FactKey, TEXT("The platform is stopped."), EC_RunLLMOption::Always);
	WatchedLane.HoldDeclarative(FactKey,
		TEXT("The platform is stopped beside the player."),
		WatchedLane.DeclarativeRespond[FactKey]);
	TestEqual(TEXT("A watch-promoted fact refresh retains Always"),
		WatchedLane.DeclarativeRespond[FactKey], EC_RunLLMOption::Always);
	TestEqual(TEXT("The watched state's original Never policy stays untouched"),
		WatchedLane.StateRespond[StateKey], EC_RunLLMOption::Never);

	// Canonical is still Stopped while a start is held. A bare watch armed during
	// that wait must evaluate the authoritative reverse against the held Downward
	// value, not dismiss it as canonical Stopped -> Stopped. The spatial fact owns
	// the promoted Always rank while the state retains its authored Never policy,
	// and both remain in the same idle lane.
	TestTrue(TEXT("A bare watch matches a reverse from the held value"),
		ConvaiContextFormat::WouldWatchMatch(
			/*generic target*/ TEXT(""), Downward, Stopped,
			/*target departure observed*/ false));
	FConvaiHeldContextLane ReverseWatchLane;
	ReverseWatchLane.HoldState(StateKey, Stopped, EC_RunLLMOption::Never,
		/*bOmitPreviousValue*/ false, &Downward,
		/*bPreserveTransition*/ true);
	ReverseWatchLane.HoldDeclarative(
		FactKey, TEXT("The platform is stopped."), EC_RunLLMOption::Always);
	TestTrue(TEXT("Reverse state and fact are paired in one held lane"),
		ReverseWatchLane.StateValues.Contains(StateKey)
			&& ReverseWatchLane.DeclarativeSentences.Contains(FactKey));
	TestEqual(TEXT("Reverse watch response rank stays on the paired fact"),
		ReverseWatchLane.DeclarativeRespond[FactKey],
		EC_RunLLMOption::Always);

	FConvaiHeldContextLane FirstVisibleLane;
	FirstVisibleLane.HoldState(StateKey, Stopped, EC_RunLLMOption::Never,
		/*bOmitPreviousValue*/ false, &Downward,
		/*bPreserveTransition*/ true);
	// The spatial fact did not exist at the watched edge (for example, no LOS).
	// When it first appears, the subsystem force-holds this Never sibling rather
	// than letting ordinary SetContextFact publish it beside canonical Downward.
	FirstVisibleLane.HoldDeclarative(
		FactKey, TEXT("The platform is stopped."), EC_RunLLMOption::Never);
	TestTrue(TEXT("A first-visible Never fact can follow the held Movement lane"),
		FirstVisibleLane.DeclarativeSentences.Contains(FactKey));
	TestEqual(TEXT("Forced pairing does not invent a response rank"),
		FirstVisibleLane.DeclarativeRespond[FactKey], EC_RunLLMOption::Never);

	// A newer confirmed restart is authoritative and withdraws both held views.
	Lane.DropStateKey(StateKey);
	Lane.DropDeclarativeKey(FactKey);
	FConvaiPendingContextBatch Restart;
	Restart.StageDeclarative(Tracker, FactKey,
		TEXT("The platform is moving upward."), EC_RunLLMOption::Never);
	Restart.StageState(Tracker, StateKey, TEXT("Upward"),
		EC_RunLLMOption::Never, /*bOmitPreviousValue*/ false, &Stopped,
		/*bPreserveTransition*/ true);
	TestFalse(TEXT("Confirmed restart leaves no older held pair"),
		Lane.HasWork());
	TestEqual(TEXT("Silent restart cannot inherit the stop response"),
		Restart.AggregateRunLLM, EC_RunLLMOption::Never);
	TestTrue(TEXT("Restart updates canonical state"),
		Tracker.GetStateValue(StateKey, CanonicalState));
	TestEqual(TEXT("Restart state is current"), CanonicalState, TEXT("Upward"));
	TestTrue(TEXT("Restart updates canonical fact"),
		Tracker.GetDeclarativeValue(FactKey, CanonicalFact));
	TestEqual(TEXT("Restart fact is current"), CanonicalFact,
		TEXT("The platform is moving upward."));
	return true;
}

// ---------------------------------------------------------------------------
// Test: Per-state response eligibility keeps silent states out of delta logic
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPendingBatchTest_StateResponseEligibility,
	"Convai.DynamicContext.PendingBatch.StateResponseEligibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPendingBatchTest_StateResponseEligibility::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	FConvaiPendingContextBatch Batch;
	Batch.StageState(Tracker, TEXT("Silent"), TEXT("Current"),
		EC_RunLLMOption::Never);
	Batch.StageState(Tracker, TEXT("Responsive"), TEXT("Changed"),
		EC_RunLLMOption::Auto);

	TestFalse(TEXT("Never key remains canonical-only"),
		Batch.StateRequestsResponse(TEXT("Silent")));
	TestTrue(TEXT("Auto key is eligible for its own delta/deferral"),
		Batch.StateRequestsResponse(TEXT("Responsive")));
	TestEqual(TEXT("Responsive peer still raises aggregate"),
		Batch.AggregateRunLLM, EC_RunLLMOption::Auto);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Bulk pending-to-held handoff preserves only explicit watch promotion
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPendingBatchTest_BulkWatchPromotionHandoff,
	"Convai.DynamicContext.PendingBatch.BulkWatchPromotionHandoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPendingBatchTest_BulkWatchPromotionHandoff::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Door.State"), TEXT("Closed"));
	Tracker.SetState(TEXT("Lamp.State"), TEXT("Off"));

	FConvaiPendingContextBatch Batch;
	Batch.StageState(Tracker, TEXT("Door.State"), TEXT("Open"),
		EC_RunLLMOption::Always);
	Batch.WatchPromotedStateKeys.Add(TEXT("Door.State"));
	Batch.StageState(Tracker, TEXT("Lamp.State"), TEXT("On"),
		EC_RunLLMOption::Never);

	FConvaiHeldContextLane Lane;
	bool bDoorWatch = false;
	Batch.WithdrawStateForHold(Tracker, TEXT("Door.State"), &bDoorWatch);
	Lane.HoldState(TEXT("Door.State"), TEXT("Locked"),
		bDoorWatch ? EC_RunLLMOption::Always : EC_RunLLMOption::Auto,
		false, nullptr, bDoorWatch, bDoorWatch);

	bool bLampWatch = true;
	Batch.WithdrawStateForHold(Tracker, TEXT("Lamp.State"), &bLampWatch);
	Lane.HoldState(TEXT("Lamp.State"), TEXT("Dim"),
		bLampWatch ? EC_RunLLMOption::Always : EC_RunLLMOption::Auto,
		false, nullptr, bLampWatch, bLampWatch);

	TestTrue(TEXT("Watched bulk key reports inherited promotion"), bDoorWatch);
	TestEqual(TEXT("Watched bulk replacement keeps Always"),
		Lane.StateRespond[TEXT("Door.State")], EC_RunLLMOption::Always);
	TestFalse(TEXT("Ordinary bulk key does not inherit watch promotion"), bLampWatch);
	TestEqual(TEXT("Ordinary bulk replacement keeps the new Auto policy"),
		Lane.StateRespond[TEXT("Lamp.State")], EC_RunLLMOption::Auto);

	const FString SupersededHeldValue = Lane.StateValues[TEXT("Door.State")];
	bool bBulkNormalWatch = false;
	Lane.DropStateKey(TEXT("Door.State"), &bBulkNormalWatch);
	const FString Closed = TEXT("Closed");
	TestTrue(TEXT("Bulk normal replacement receives held watch ownership"),
		bBulkNormalWatch);
	TestTrue(TEXT("Watched round trip remains staged"),
		Batch.StageState(Tracker, TEXT("Door.State"), Closed,
			bBulkNormalWatch ? EC_RunLLMOption::Always : EC_RunLLMOption::Auto,
			false, &SupersededHeldValue, bBulkNormalWatch));
	if (bBulkNormalWatch)
	{
		Batch.WatchPromotedStateKeys.Add(TEXT("Door.State"));
	}
	TestTrue(TEXT("Bulk normal replacement re-tags pending watch ownership"),
		Batch.WatchPromotedStateKeys.Contains(TEXT("Door.State")));
	TestEqual(TEXT("Bulk normal replacement retains Always"),
		Batch.StateRespond[TEXT("Door.State")], EC_RunLLMOption::Always);
	return true;
}

// ---------------------------------------------------------------------------
// Test: Held watch ownership survives replacement and transfers exactly once
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeldLaneTest_WatchPromotionOwnership,
	"Convai.DynamicContext.HeldLane.WatchPromotionOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeldLaneTest_WatchPromotionOwnership::RunTest(const FString& Parameters)
{
	FConvaiHeldContextLane Lane;
	Lane.HoldState(TEXT("Door.State"), TEXT("Open"),
		EC_RunLLMOption::Always, false, nullptr,
		/*bPreserveTransition*/ true, /*bWatchPromoted*/ true);
	Lane.HoldState(TEXT("Door.State"), TEXT("Locked"),
		EC_RunLLMOption::Never);

	TestTrue(TEXT("Watch ownership is sticky across held replacement"),
		Lane.WatchPromotedStateKeys.Contains(TEXT("Door.State")));
	TestTrue(TEXT("Watch ownership also preserves transition history"),
		Lane.StatePreserveTransitions.Contains(TEXT("Door.State")));

	bool bTransferredWatch = false;
	Lane.DropStateKey(TEXT("Door.State"), &bTransferredWatch);
	TestTrue(TEXT("Authoritative drop transfers consumed watch ownership"),
		bTransferredWatch);
	TestFalse(TEXT("Transferred watch metadata is removed from held lane"),
		Lane.WatchPromotedStateKeys.Contains(TEXT("Door.State")));

	bTransferredWatch = true;
	Lane.DropStateKey(TEXT("Door.State"), &bTransferredWatch);
	TestFalse(TEXT("A second drop cannot transfer the watch twice"),
		bTransferredWatch);

	Lane.HoldState(TEXT("Lamp.State"), TEXT("On"),
		EC_RunLLMOption::Always, false, nullptr,
		/*bPreserveTransition*/ true, /*bWatchPromoted*/ true);
	Lane.Clear();
	TestTrue(TEXT("Clear removes all watch-promotion ownership"),
		Lane.WatchPromotedStateKeys.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
