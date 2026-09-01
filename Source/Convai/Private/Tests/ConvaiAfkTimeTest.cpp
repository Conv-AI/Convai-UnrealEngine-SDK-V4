// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "ConvaiSubsystem.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// What the server actually does, observed on the wire: it closes an idle
	// session after ServerBudget, warning every WarnInterval on the way down with
	// the seconds remaining (450, 300, 150), and restarts the whole countdown
	// whenever it is sent a reset.
	constexpr double ServerBudget = 600.0;
	constexpr double WarnInterval = 150.0;

	/** Plays a session out against that server and returns the second at which it
	 *  is finally closed, with the player going quiet at t=0 and never returning. */
	double SimulateDisconnectSecond(float AfkTimeSeconds)
	{
		double Deadline = ServerBudget;
		double NextWarning = WarnInterval;

		for (double T = 1.0; T < 100000.0; T += 1.0)
		{
			if (T >= Deadline)
			{
				return T;
			}

			if (T >= NextWarning)
			{
				const int32 Remaining = FMath::RoundToInt(Deadline - T);
				if (UConvaiSubsystem::ShouldRenewIdleTimer(T, Remaining, AfkTimeSeconds))
				{
					Deadline = T + ServerBudget;
				}
				NextWarning = T + WarnInterval;
			}
		}

		return -1.0;
	}
}

// ---------------------------------------------------------------------------
// Test: the shipped default changes nothing
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAfkTimeTest_DefaultSendsNothing,
	"Convai.Connection.AfkTime.DefaultSendsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAfkTimeTest_DefaultSendsNothing::RunTest(const FString& Parameters)
{
	// Every warning the server sends at the default budget already reaches the
	// deadline, so none of them is ever answered and an upgrading project sees
	// exactly the behaviour it had before the setting existed.
	TestFalse(TEXT("First warning is not renewed at the default"),
		UConvaiSubsystem::ShouldRenewIdleTimer(150.0, 450, 600.0f));
	TestFalse(TEXT("Second warning is not renewed at the default"),
		UConvaiSubsystem::ShouldRenewIdleTimer(300.0, 300, 600.0f));
	TestFalse(TEXT("Final warning is not renewed at the default"),
		UConvaiSubsystem::ShouldRenewIdleTimer(450.0, 150, 600.0f));

	TestEqual(TEXT("Session still closes on the server's own schedule"),
		SimulateDisconnectSecond(600.0f), 600.0);
	return true;
}

// ---------------------------------------------------------------------------
// Test: a raised budget is honoured, and honoured tightly
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAfkTimeTest_RaisedBudgetIsHonoured,
	"Convai.Connection.AfkTime.RaisedBudgetIsHonoured",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAfkTimeTest_RaisedBudgetIsHonoured::RunTest(const FString& Parameters)
{
	// Budgets that fall on a warning boundary land exactly.
	TestEqual(TEXT("20 minutes closes at 20 minutes"),
		SimulateDisconnectSecond(1200.0f), 1200.0);
	TestEqual(TEXT("15 minutes closes at 15 minutes"),
		SimulateDisconnectSecond(900.0f), 900.0);

	// Budgets that do not still land inside one warning interval, and always
	// late rather than early - cutting a player off before their own setting
	// said to would be the worse failure.
	for (float Afk = 620.0f; Afk <= 3600.0f; Afk += 10.0f)
	{
		const double Closed = SimulateDisconnectSecond(Afk);
		if (!TestTrue(TEXT("Never closes before the budget"), Closed >= (double)Afk))
		{
			return false;
		}
		if (!TestTrue(TEXT("Never overshoots by a whole warning interval"),
			Closed - (double)Afk < WarnInterval))
		{
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test: elapsed time alone is not the predicate
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAfkTimeTest_CountsTheDeadlineNotTheClock,
	"Convai.Connection.AfkTime.CountsTheDeadlineNotTheClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAfkTimeTest_CountsTheDeadlineNotTheClock::RunTest(const FString& Parameters)
{
	// The final warning inside a 20 minute budget. The player has been away for
	// less than the budget, so a predicate reading elapsed time alone renews here
	// and pushes the close out to 27.5 minutes. Reading the deadline the server
	// is counting down to stops instead.
	TestFalse(TEXT("Does not renew a warning whose deadline already reaches the budget"),
		UConvaiSubsystem::ShouldRenewIdleTimer(750.0, 450, 1200.0f));
	TestTrue(TEXT("Renews while the deadline is still short of the budget"),
		UConvaiSubsystem::ShouldRenewIdleTimer(600.0, 450, 1200.0f));
	return true;
}

// ---------------------------------------------------------------------------
// Test: a malformed warning cannot buy extra life
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAfkTimeTest_MalformedWarningIsNotCredited,
	"Convai.Connection.AfkTime.MalformedWarningIsNotCredited",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAfkTimeTest_MalformedWarningIsNotCredited::RunTest(const FString& Parameters)
{
	// A warning with no readable remaining_seconds is read as none left, which
	// degrades to comparing elapsed time alone: still bounded, still never early.
	TestTrue(TEXT("Unknown remaining still renews inside the budget"),
		UConvaiSubsystem::ShouldRenewIdleTimer(500.0, 0, 1200.0f));
	TestFalse(TEXT("Unknown remaining stops at the budget"),
		UConvaiSubsystem::ShouldRenewIdleTimer(1200.0, 0, 1200.0f));

	// A negative remaining is nonsense; it must not read as time in hand.
	TestFalse(TEXT("Negative remaining is clamped, not trusted"),
		UConvaiSubsystem::ShouldRenewIdleTimer(1200.0, -600, 1200.0f));
	return true;
}

// ---------------------------------------------------------------------------
// Test: a character mid-line gets one reprieve, and only one
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAfkTimeTest_MidLineGetsOneReprieve,
	"Convai.Connection.AfkTime.MidLineGetsOneReprieve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAfkTimeTest_MidLineGetsOneReprieve::RunTest(const FString& Parameters)
{
	// Inside the budget the character makes no difference either way - the
	// reprieve is not spent on a renewal that was happening regardless.
	TestTrue(TEXT("Inside the budget, renews without touching the reprieve"),
		UConvaiSubsystem::DecideIdleWarningAction(600.0, 450, 1200.0f, true, false)
			== EConvaiIdleWarningAction::Renew);

	// Out of budget with the character mid-line: buy one more window.
	TestTrue(TEXT("Out of budget mid-line, renews for speech"),
		UConvaiSubsystem::DecideIdleWarningAction(1200.0, 450, 1200.0f, true, false)
			== EConvaiIdleWarningAction::RenewForSpeech);

	// The same warning once the reprieve is gone: the character does not get to
	// hold the session open by continuing to talk.
	TestTrue(TEXT("Reprieve is not renewable"),
		UConvaiSubsystem::DecideIdleWarningAction(1200.0, 450, 1200.0f, true, true)
			== EConvaiIdleWarningAction::LetClose);

	// Silence out of budget closes immediately, spent or not.
	TestTrue(TEXT("Silent and out of budget closes"),
		UConvaiSubsystem::DecideIdleWarningAction(1200.0, 450, 1200.0f, false, false)
			== EConvaiIdleWarningAction::LetClose);

	// An empty chair is still an empty chair at the default budget.
	TestTrue(TEXT("Default budget still closes on the first warning when silent"),
		UConvaiSubsystem::DecideIdleWarningAction(150.0, 450, 600.0f, false, false)
			== EConvaiIdleWarningAction::LetClose);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
