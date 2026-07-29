// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiDefinitions.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FConvaiMovementPoint MakePoint(const FString& Name, bool bSeparateDestination = false,
		bool bEnabled = true,
		EConvaiMovementPointAttachment Attachment = EConvaiMovementPointAttachment::RelativeToObject)
	{
		FConvaiMovementPoint P;
		P.Name = Name;
		P.bCreatesSeparateDestination = bSeparateDestination;
		P.bEnabled = bEnabled;
		P.Attachment = Attachment;
		return P;
	}

	FConvaiObjectEntry MakeEntry(const FString& Name, std::initializer_list<FConvaiMovementPoint> Points)
	{
		FConvaiObjectEntry E;
		E.Name = Name;
		for (const FConvaiMovementPoint& P : Points)
		{
			E.MovementPoints.Add(P);
		}
		return E;
	}
}

// ---------------------------------------------------------------------------
// Test: NormalizeMovementPointName — trim + collapse, casing preserved
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMovementPointNamesTest_Normalize,
	"Convai.Objects.MovementPoints.NormalizeName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMovementPointNamesTest_Normalize::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Plain name untouched"),
		FConvaiObjectEntry::NormalizeMovementPointName(TEXT("Other Side")), TEXT("Other Side"));
	TestEqual(TEXT("Leading/trailing whitespace trimmed"),
		FConvaiObjectEntry::NormalizeMovementPointName(TEXT("  Other Side  ")), TEXT("Other Side"));
	TestEqual(TEXT("Inner whitespace collapsed"),
		FConvaiObjectEntry::NormalizeMovementPointName(TEXT("Other   Side")), TEXT("Other Side"));
	TestEqual(TEXT("Tabs count as whitespace"),
		FConvaiObjectEntry::NormalizeMovementPointName(TEXT("Other\tSide")), TEXT("Other Side"));
	TestEqual(TEXT("Casing preserved for display"),
		FConvaiObjectEntry::NormalizeMovementPointName(TEXT("other side")), TEXT("other side"));
	TestEqual(TEXT("Whitespace-only is empty"),
		FConvaiObjectEntry::NormalizeMovementPointName(TEXT("   ")), TEXT(""));
	TestEqual(TEXT("Empty stays empty"),
		FConvaiObjectEntry::NormalizeMovementPointName(TEXT("")), TEXT(""));
	return true;
}

// ---------------------------------------------------------------------------
// Test: CollectMovementPointSubNames — the opt-in expansion rules
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMovementPointNamesTest_Collect,
	"Convai.Objects.MovementPoints.CollectSubNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMovementPointNamesTest_Collect::RunTest(const FString& Parameters)
{
	TArray<FString> Subs;

	// No point opted in -> no sub-objects, whatever the names say. (The Name
	// field is disabled in the editor while the checkbox is off; stale text
	// in the data must stay inert.)
	{
		const FConvaiObjectEntry Door = MakeEntry(TEXT("Door"),
			{ MakePoint(TEXT("")), MakePoint(TEXT("Stale Text")) });
		FConvaiObjectEntry::CollectMovementPointSubNames({ &Door }, Subs);
		TestEqual(TEXT("Nothing opted in: no sub-objects"), Subs.Num(), 0);
	}

	// Create Separate Destination is an explicit opt-in: a single ticked,
	// named point expands — no dormancy heuristics.
	{
		const FConvaiObjectEntry Door = MakeEntry(TEXT("Door"),
			{ MakePoint(TEXT("")), MakePoint(TEXT("Other Side"), /*bSeparateDestination*/ true) });
		FConvaiObjectEntry::CollectMovementPointSubNames({ &Door }, Subs);
		TestEqual(TEXT("One opted-in point: one sub-object"), Subs.Num(), 1);
		if (Subs.Num() == 1)
		{
			TestEqual(TEXT("Display name preserved"), Subs[0], TEXT("Other Side"));
		}
	}

	// A ticked point with an EMPTY name is inert (nothing to address).
	{
		const FConvaiObjectEntry Door = MakeEntry(TEXT("Door"),
			{ MakePoint(TEXT(""), /*bSeparateDestination*/ true) });
		FConvaiObjectEntry::CollectMovementPointSubNames({ &Door }, Subs);
		TestEqual(TEXT("Opted-in but unnamed: no sub-objects"), Subs.Num(), 0);
	}

	// Multiple distinct destinations; case/whitespace variants collapse with
	// first occurrence supplying the display casing.
	{
		const FConvaiObjectEntry Door = MakeEntry(TEXT("Door"),
			{ MakePoint(TEXT("Kitchen Side"), true), MakePoint(TEXT("Hallway Side"), true),
			  MakePoint(TEXT("kitchen   side"), true) });
		FConvaiObjectEntry::CollectMovementPointSubNames({ &Door }, Subs);
		TestEqual(TEXT("Two distinct destinations: two sub-objects"), Subs.Num(), 2);
		if (Subs.Num() == 2)
		{
			TestEqual(TEXT("Insertion order kept"), Subs[0], TEXT("Kitchen Side"));
			TestEqual(TEXT("Second name kept"), Subs[1], TEXT("Hallway Side"));
		}
	}

	// Disabled points never create identities, opted-in or not.
	{
		const FConvaiObjectEntry Door = MakeEntry(TEXT("Door"),
			{ MakePoint(TEXT("")), MakePoint(TEXT("Other Side"), true, /*bEnabled*/ false) });
		FConvaiObjectEntry::CollectMovementPointSubNames({ &Door }, Subs);
		TestEqual(TEXT("Disabled destination point: no sub-objects"), Subs.Num(), 0);
	}

	// Merged set: destinations group case-insensitively ACROSS members.
	{
		const FConvaiObjectEntry MemberA = MakeEntry(TEXT("Door"),
			{ MakePoint(TEXT("Other Side"), true), MakePoint(TEXT("")) });
		const FConvaiObjectEntry MemberB = MakeEntry(TEXT("Door"),
			{ MakePoint(TEXT("OTHER SIDE"), true), MakePoint(TEXT("Balcony"), true) });
		FConvaiObjectEntry::CollectMovementPointSubNames({ &MemberA, &MemberB }, Subs);
		TestEqual(TEXT("Merged: two distinct destinations"), Subs.Num(), 2);
		if (Subs.Num() == 2)
		{
			TestEqual(TEXT("First occurrence supplies casing"), Subs[0], TEXT("Other Side"));
			TestEqual(TEXT("Member B's unique destination included"), Subs[1], TEXT("Balcony"));
		}
	}

	// The elevator scenario: the platform's own point stays with the object
	// (checkbox off), the landings become destinations (checkbox on).
	{
		const FConvaiObjectEntry Platform = MakeEntry(TEXT("Platform"),
			{ MakePoint(TEXT("")),
			  MakePoint(TEXT("Lower Landing"), true),
			  MakePoint(TEXT("Upper Landing"), true) });
		FConvaiObjectEntry::CollectMovementPointSubNames({ &Platform }, Subs);
		TestEqual(TEXT("Elevator: two landing destinations"), Subs.Num(), 2);

		FConvaiObjectEntry Base = Platform;
		Base.FilterMovementPointsToObjectItself();
		TestEqual(TEXT("Base object keeps only its own point"), Base.MovementPoints.Num(), 1);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test: filters + MakeMovementPointSubEntry
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMovementPointNamesTest_SubEntry,
	"Convai.Objects.MovementPoints.MakeSubEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMovementPointNamesTest_SubEntry::RunTest(const FString& Parameters)
{
	FConvaiObjectEntry Door = MakeEntry(TEXT("Door"),
		{ MakePoint(TEXT("")), MakePoint(TEXT("Other Side"), true), MakePoint(TEXT("other side"), true),
		  MakePoint(TEXT("Balcony"), true) });
	Door.bFallbackToObjectWhenPointsUnreachable = true;

	const FConvaiObjectEntry Sub = Door.MakeMovementPointSubEntry(TEXT("Other Side"));
	TestEqual(TEXT("Sub name is space-joined"), Sub.Name, TEXT("Door Other Side"));
	TestEqual(TEXT("Sub keeps only its points (case-insensitive)"), Sub.MovementPoints.Num(), 2);
	TestFalse(TEXT("Sub never falls back to the object body"), Sub.bFallbackToObjectWhenPointsUnreachable);
	TestTrue(TEXT("Sub is flagged"), Sub.bIsMovementPointSubObject);
	TestEqual(TEXT("Sub bookkeeping: base name"), Sub.MovementPointSubObjectBaseName, TEXT("Door"));
	TestEqual(TEXT("Sub bookkeeping: point name"), Sub.MovementPointSubObjectPointName, TEXT("Other Side"));
	TestEqual(TEXT("Relative destination is distinguished from the object"),
		Sub.Description,
		TEXT("A standing location that moves with Door; it is not Door itself."));

	const FConvaiObjectEntry Platform = MakeEntry(TEXT("Moving Platform"),
		{ MakePoint(TEXT("Lower Landing"), true, true,
			EConvaiMovementPointAttachment::KeepWorldPosition) });
	const FConvaiObjectEntry LowerLanding =
		Platform.MakeMovementPointSubEntry(TEXT("Lower Landing"));
	TestEqual(TEXT("World-fixed landing is distinguished from the platform"),
		LowerLanding.Description,
		TEXT("A fixed standing location for accessing Moving Platform; it is not Moving Platform itself."));

	const FConvaiObjectEntry Mixed = MakeEntry(TEXT("Lift"),
		{ MakePoint(TEXT("Exit"), true, true,
			EConvaiMovementPointAttachment::KeepWorldPosition),
		  MakePoint(TEXT("Exit"), true, true,
			EConvaiMovementPointAttachment::RelativeToObject) });
	TestEqual(TEXT("Mixed attachment destinations stay truthful and generic"),
		Mixed.MakeMovementPointSubEntry(TEXT("Exit")).Description,
		TEXT("A separate standing location for Lift; it is not Lift itself."));

	const FConvaiObjectEntry FixedMember = MakeEntry(TEXT("Lift"),
		{ MakePoint(TEXT("Exit"), true, true,
			EConvaiMovementPointAttachment::KeepWorldPosition) });
	const FConvaiObjectEntry RelativeMember = MakeEntry(TEXT("Lift"),
		{ MakePoint(TEXT("Exit"), true, true,
			EConvaiMovementPointAttachment::RelativeToObject) });
	const TArray<const FConvaiObjectEntry*> MergedMembers{
		&FixedMember, &RelativeMember };
	TestEqual(TEXT("Merged members aggregate attachment semantics"),
		FConvaiObjectEntry::MovementPointDestinationDescription(
			TEXT("Lift"), MergedMembers, TEXT("Exit")),
		TEXT("A separate standing location for Lift; it is not Lift itself."));

	const FConvaiObjectEntry DisabledMismatch = MakeEntry(TEXT("Lift"),
		{ MakePoint(TEXT("Deck"), true, true,
			EConvaiMovementPointAttachment::RelativeToObject),
		  MakePoint(TEXT("Deck"), true, false,
			EConvaiMovementPointAttachment::KeepWorldPosition) });
	TestEqual(TEXT("Disabled points do not change attachment semantics"),
		DisabledMismatch.MakeMovementPointSubEntry(TEXT("Deck")).Description,
		TEXT("A standing location that moves with Lift; it is not Lift itself."));

	FConvaiObjectEntry Base = Door;
	Base.FilterMovementPointsToObjectItself();
	TestEqual(TEXT("Base keeps only the object-itself point"), Base.MovementPoints.Num(), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
