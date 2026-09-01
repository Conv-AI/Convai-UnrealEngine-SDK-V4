// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "ConvaiActionUtils.h"
#include "ConvaiDefinitions.h"
#include "Environment/ConvaiEnvironment.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiSpaceDelimitedActionTest,
	"Convai.Actions.Parsing.SpaceDelimitedAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiSpaceDelimitedActionTest::RunTest(const FString& Parameters)
{
	TArray<FConvaiActionParam> MoveParameters;
	MoveParameters.Emplace(TEXT("destination"), FString(),
		EConvaiActionParamType::Reference);

	TArray<FConvaiActionParam> RoutineParameters;
	RoutineParameters.Emplace(TEXT("routine"), FString(),
		EConvaiActionParamType::String);

	TArray<FConvaiActionParam> EscortParameters;
	EscortParameters.Emplace(TEXT("character"), FString(),
		EConvaiActionParamType::Reference);
	FConvaiActionParam& Destination = EscortParameters.Emplace_GetRef(
		TEXT("destination"), FString(), EConvaiActionParamType::Reference);
	Destination.Connector = TEXT("to");

	const FConvaiAction Move(TEXT("Move To"), FString(), MoveParameters);
	const FConvaiAction ShowRoutine(
		TEXT("Show Routine"), FString(), RoutineParameters);
	const FConvaiAction Escort(TEXT("Escort"), FString(), EscortParameters);
	const TArray<FConvaiAction> Templates = { Move, ShowRoutine, Escort };

	const FString MoveResponse = TEXT("Move To Cube");
	const FConvaiAction* MoveTemplate =
		UConvaiActions::FindActionTemplate(MoveResponse, Templates);
	if (!TestNotNull(TEXT("The standard move response matches"), MoveTemplate))
	{
		return false;
	}
	TestEqual(TEXT("The standard move action is recovered"),
		MoveTemplate->Name, Move.Name);
	const FString MoveBlob =
		UConvaiActions::StripActionPrefix(MoveResponse, MoveTemplate->Name);
	TestEqual(TEXT("The move value follows the action name"),
		MoveBlob, FString(TEXT("Cube")));
	TestEqual(TEXT("The move value remains intact"),
		UConvaiActions::SplitParamValues(MoveBlob, MoveTemplate->Parameters)[0],
		FString(TEXT("Cube")));

	const FString RoutineResponse = TEXT("Show Routine Sensitive Skin");
	const FConvaiAction* RoutineTemplate =
		UConvaiActions::FindActionTemplate(RoutineResponse, Templates);
	if (!TestNotNull(TEXT("The standard routine response matches"), RoutineTemplate))
	{
		return false;
	}
	const FString RoutineBlob =
		UConvaiActions::StripActionPrefix(RoutineResponse, RoutineTemplate->Name);
	TestEqual(TEXT("A multi-word value owns the remaining response"),
		UConvaiActions::SplitParamValues(
			RoutineBlob, RoutineTemplate->Parameters)[0],
		FString(TEXT("Sensitive Skin")));

	const FString EscortResponse = TEXT("Escort User to Gallery");
	const FConvaiAction* EscortTemplate =
		UConvaiActions::FindActionTemplate(EscortResponse, Templates);
	if (!TestNotNull(TEXT("The connector response matches"), EscortTemplate))
	{
		return false;
	}
	const FString EscortBlob =
		UConvaiActions::StripActionPrefix(EscortResponse, EscortTemplate->Name);
	const TArray<FString> EscortValues =
		UConvaiActions::SplitParamValues(EscortBlob, EscortTemplate->Parameters);
	if (!TestEqual(TEXT("The connector response yields two values"),
		EscortValues.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("The first connector value is recovered"),
		EscortValues[0], FString(TEXT("User")));
	TestEqual(TEXT("The second connector value is recovered"),
		EscortValues[1], FString(TEXT("Gallery")));

	const FString SeparateTarget = TEXT("Sensitive Skin");
	TestEqual(TEXT("A separately supplied target uses the same value parser"),
		UConvaiActions::SplitParamValues(
			SeparateTarget, RoutineTemplate->Parameters)[0],
		SeparateTarget);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiInlineParameterizedActionTest,
	"Convai.Actions.Parsing.InlineParameterizedAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiInlineParameterizedActionTest::RunTest(const FString& Parameters)
{
	TArray<FConvaiActionParam> RoutineParameters;
	FConvaiActionParam& Routine = RoutineParameters.AddDefaulted_GetRef();
	Routine.Name = TEXT("Rutina");
	Routine.Type = EConvaiActionParamType::String;
	Routine.Choices = {
		TEXT("Piel Sensible"),
		TEXT("Piel Mixta"),
		TEXT("Piel Normal a Seca")
	};

	const FConvaiAction Basic(TEXT("MostrarRutina"), FString(), RoutineParameters);
	const FConvaiAction Detailed(TEXT("MostrarRutina Detallada"), FString(), RoutineParameters);
	const TArray<FConvaiAction> Templates = { Basic, Detailed };

	const FString Inline = TEXT("MostrarRutina{Rutina:Piel Sensible}");
	const FConvaiAction* Template = UConvaiActions::FindActionTemplate(Inline, Templates);
	if (!TestNotNull(TEXT("The inline response matches its advertised action"), Template))
	{
		return false;
	}
	TestEqual(TEXT("The canonical action name is recovered"), Template->Name, Basic.Name);

	const FString Blob = UConvaiActions::StripActionPrefix(Inline, Template->Name);
	TestEqual(TEXT("The inline parameter blob is retained"), Blob,
		FString(TEXT("{Rutina:Piel Sensible}")));
	const TArray<FString> Values = UConvaiActions::SplitParamValues(Blob, Template->Parameters);
	if (!TestEqual(TEXT("The inline response yields one value"), Values.Num(), 1))
	{
		return false;
	}
	const FString CleanValue = UConvaiActions::StripParamNameMimicry(
		Values[0], Template->Parameters[0].Name);
	TestEqual(TEXT("The Spanish choice is recovered exactly"), CleanValue,
		FString(TEXT("Piel Sensible")));

	const FString LongerInline =
		TEXT("MostrarRutina Detallada{Rutina:Piel Normal a Seca}");
	const FConvaiAction* LongerTemplate =
		UConvaiActions::FindActionTemplate(LongerInline, Templates);
	if (!TestNotNull(TEXT("An overlapping longer action matches"), LongerTemplate))
	{
		return false;
	}
	TestEqual(TEXT("The longest canonical prefix wins"), LongerTemplate->Name, Detailed.Name);

	const FString NonBoundary = TEXT("MostrarRutinaExtendida{Rutina:Piel Mixta}");
	TestNull(TEXT("A partial identifier prefix is not an action match"),
		UConvaiActions::FindActionTemplate(NonBoundary, Templates));
	TestEqual(TEXT("A partial identifier is not stripped"),
		UConvaiActions::StripActionPrefix(NonBoundary, Basic.Name), NonBoundary);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiActionPunctuationReferenceTest,
	"Convai.Actions.Parsing.PunctuationRichReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiActionPunctuationReferenceTest::RunTest(const FString& Parameters)
{
	const FString CanonicalName = TEXT("Artwork: Marcus Panel");

	TestEqual(TEXT("An unlabelled canonical value keeps its colon spacing"),
		UConvaiActions::StripParamNameMimicry(CanonicalName, TEXT("target")), CanonicalName);
	TestEqual(TEXT("A declared parameter label is removed without rewriting the value"),
		UConvaiActions::StripParamNameMimicry(
			TEXT("target: Artwork: Marcus Panel"), TEXT("target")), CanonicalName);
	TestEqual(TEXT("An echoed type label is removed without rewriting the value"),
		UConvaiActions::StripParamNameMimicry(
			TEXT("target: ref: Artwork: Marcus Panel"), TEXT("target")), CanonicalName);
	TestEqual(TEXT("An echoed choice list is removed only from the parameter label"),
		UConvaiActions::StripParamNameMimicry(
			TEXT("target [Artwork|Statue]: Artwork: Marcus Panel"), TEXT("target")),
		CanonicalName);
	TestEqual(TEXT("Brackets authored inside a value are preserved"),
		UConvaiActions::StripParamNameMimicry(
			TEXT("Gallery [North]"), TEXT("target")),
		FString(TEXT("Gallery [North]")));
	TestEqual(TEXT("A time value keeps its internal colon"),
		UConvaiActions::StripParamNameMimicry(TEXT("3:30"), TEXT("time")), FString(TEXT("3:30")));
	TestEqual(TEXT("Repeated unrecognized colons remain intact"),
		UConvaiActions::StripParamNameMimicry(
			TEXT("Gallery:: Artwork: Marcus Panel"), TEXT("target")),
		FString(TEXT("Gallery:: Artwork: Marcus Panel")));
	TestEqual(TEXT("URLs remain intact"),
		UConvaiActions::StripParamNameMimicry(
			TEXT("https://example.com/exhibit:2"), TEXT("target")),
		FString(TEXT("https://example.com/exhibit:2")));

	FConvaiEnvironmentData Environment;
	FConvaiObjectEntry RegisteredObject;
	RegisteredObject.Name = CanonicalName;
	Environment.Objects.Add(RegisteredObject);

	TArray<FConvaiActionParam> MoveToParameters;
	MoveToParameters.Emplace(TEXT("target"), FString(), EConvaiActionParamType::Reference);
	TestEqual(TEXT("Embedded quotes remain part of a single reference value"),
		UConvaiActions::SplitParamValues(TEXT("Marcus \"Aurelius\" Panel"), MoveToParameters)[0],
		FString(TEXT("Marcus \"Aurelius\" Panel")));
	TestEqual(TEXT("Embedded braces remain part of a single reference value"),
		UConvaiActions::SplitParamValues(TEXT("Gallery {North}"), MoveToParameters)[0],
		FString(TEXT("Gallery {North}")));
	TestEqual(TEXT("A whole-value brace wrapper is still removed"),
		UConvaiActions::SplitParamValues(TEXT("{Artwork: Marcus Panel}"), MoveToParameters)[0],
		CanonicalName);

	const FConvaiAction MoveTo(TEXT("Move To"), FString(), MoveToParameters);
	const TArray<FConvaiAction> Templates = { MoveTo };
	const FString FilledAction = TEXT("Move To Artwork: Marcus Panel");
	const FConvaiAction* Template = UConvaiActions::FindActionTemplate(FilledAction, Templates);
	if (!TestNotNull(TEXT("The filled Move To action matches its template"), Template))
	{
		return false;
	}

	const FString Combined = UConvaiActions::StripActionPrefix(FilledAction, Template->Name);
	const TArray<FString> Values = UConvaiActions::SplitParamValues(Combined, Template->Parameters);
	if (!TestEqual(TEXT("The parser returns one target slot"), Values.Num(), 1))
	{
		return false;
	}

	const FString CleanValue = UConvaiActions::StripParamNameMimicry(
		Values[0], Template->Parameters[0].Name, &Environment);
	const FConvaiResultParam Parsed = UConvaiActions::CoerceParam(
		CleanValue, Template->Parameters[0].Type, Environment);
	TestEqual(TEXT("The parsed string retains the canonical object name"), Parsed.StringValue, CanonicalName);
	TestEqual(TEXT("The punctuation-rich object resolves to a reference"), Parsed.RefValue.Name, CanonicalName);

	FConvaiObjectEntry LabelShapedObject;
	LabelShapedObject.Name = TEXT("target: East Hall");
	Environment.Objects.Add(LabelShapedObject);
	TestEqual(TEXT("An exact registered name wins over label-shaped syntax"),
		UConvaiActions::StripParamNameMimicry(
			LabelShapedObject.Name, TEXT("target"), &Environment), LabelShapedObject.Name);

	FConvaiActionParam CharacterParameter(
		TEXT("character"), FString(), EConvaiActionParamType::Reference);
	FConvaiActionParam DestinationParameter(
		TEXT("destination"), FString(), EConvaiActionParamType::Reference);
	DestinationParameter.Connector = TEXT("to");
	const TArray<FConvaiActionParam> EscortParameters = {
		CharacterParameter, DestinationParameter
	};

	const TArray<FString> NamedMapValues = UConvaiActions::SplitParamValues(
		TEXT("{character: \"User\", destination: \"Augustus display\"}"),
		EscortParameters);
	TestEqual(TEXT("A named brace map fills both Escort parameters"),
		NamedMapValues.Num(), 2);
	if (NamedMapValues.Num() == 2)
	{
		const FConvaiResultParam ParsedCharacter = UConvaiActions::CoerceParam(
			UConvaiActions::StripParamNameMimicry(
				NamedMapValues[0], CharacterParameter.Name, &Environment),
			CharacterParameter.Type, Environment);
		const FConvaiResultParam ParsedDestination = UConvaiActions::CoerceParam(
			UConvaiActions::StripParamNameMimicry(
				NamedMapValues[1], DestinationParameter.Name, &Environment),
			DestinationParameter.Type, Environment);
		TestEqual(TEXT("The named brace map isolates its character value"),
			ParsedCharacter.StringValue, FString(TEXT("User")));
		TestEqual(TEXT("The named brace map isolates its destination value"),
			ParsedDestination.StringValue, FString(TEXT("Augustus display")));
	}

	const TArray<FString> PositionalBraceValues = UConvaiActions::SplitParamValues(
		TEXT("{User} {Augustus display}"), EscortParameters);
	TestEqual(TEXT("Two positional brace values remain supported"),
		PositionalBraceValues.Num(), 2);
	if (PositionalBraceValues.Num() == 2)
	{
		TestEqual(TEXT("The first positional brace remains the character"),
			PositionalBraceValues[0], FString(TEXT("User")));
		TestEqual(TEXT("The second positional brace remains the destination"),
			PositionalBraceValues[1], FString(TEXT("Augustus display")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiActionResponseCorpusTest,
	"Convai.Actions.Parsing.ResponseCorpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiActionResponseCorpusTest::RunTest(const FString& Parameters)
{
	TArray<FConvaiActionParam> MoveParameters;
	MoveParameters.Emplace(
		TEXT("destination"), FString(), EConvaiActionParamType::Reference);

	TArray<FConvaiActionParam> NoteParameters;
	NoteParameters.Emplace(
		TEXT("message"), FString(), EConvaiActionParamType::String);

	TArray<FConvaiActionParam> EscortParameters;
	EscortParameters.Emplace(
		TEXT("character"), FString(), EConvaiActionParamType::Reference);
	FConvaiActionParam& EscortDestination = EscortParameters.Emplace_GetRef(
		TEXT("destination"), FString(), EConvaiActionParamType::Reference);
	EscortDestination.Connector = TEXT("to");

	TArray<FConvaiActionParam> MessageParameters;
	MessageParameters.Emplace(
		TEXT("message"), FString(), EConvaiActionParamType::String);
	FConvaiActionParam& MessageRecipient = MessageParameters.Emplace_GetRef(
		TEXT("recipient"), FString(), EConvaiActionParamType::Reference);
	MessageRecipient.Connector = TEXT("to");

	const TArray<FConvaiAction> Templates = {
		FConvaiAction(TEXT("Move To"), FString(), MoveParameters),
		FConvaiAction(TEXT("Record Note"), FString(), NoteParameters),
		FConvaiAction(TEXT("Escort"), FString(), EscortParameters),
		FConvaiAction(TEXT("Send Message"), FString(), MessageParameters)
	};

	struct FResponseCase
	{
		FString Label;
		FString RawResponse;
		FString ExpectedAction;
		TArray<FString> ExpectedValues;
	};

	TArray<FResponseCase> Cases;
	const auto AddCase = [&Cases](
		const TCHAR* Label,
		const TCHAR* RawResponse,
		const TCHAR* ExpectedAction,
		TArray<FString> ExpectedValues)
	{
		FResponseCase Case;
		Case.Label = Label;
		Case.RawResponse = RawResponse;
		Case.ExpectedAction = ExpectedAction;
		Case.ExpectedValues = MoveTemp(ExpectedValues);
		Cases.Add(MoveTemp(Case));
	};

	// Observed contract and close weak-model variations.
	AddCase(TEXT("standard space-delimited reference"),
		TEXT("Move To Cube"), TEXT("Move To"), { TEXT("Cube") });
	AddCase(TEXT("legacy named brace reference"),
		TEXT("Move To{destination:Cube}"), TEXT("Move To"), { TEXT("Cube") });
	AddCase(TEXT("lowercase canonical action"),
		TEXT("move to Cube"), TEXT("Move To"), { TEXT("Cube") });
	AddCase(TEXT("colon-glued action separator"),
		TEXT("Move To: Cube"), TEXT("Move To"), { TEXT("Cube") });
	AddCase(TEXT("comma-glued action separator"),
		TEXT("Move To, Cube"), TEXT("Move To"), { TEXT("Cube") });
	AddCase(TEXT("braced value followed by sentence punctuation"),
		TEXT("Move To{Cube}."), TEXT("Move To"), { TEXT("Cube") });

	// A String without Choices is intentionally unrestricted. These cases catch
	// accidental rewriting that a constrained enum/choice might otherwise hide.
	AddCase(TEXT("free text keeps punctuation quotes braces and time"),
		TEXT("Record Note Visitor said: \"Meet me at 3:30, by Gallery {North}.\""),
		TEXT("Record Note"),
		{ TEXT("Visitor said: \"Meet me at 3:30, by Gallery {North}.\"") });
	AddCase(TEXT("named free text keeps commas semicolons and colon"),
		TEXT("Record Note{message:Use route A, then B; arrive at 3:30.}."),
		TEXT("Record Note"),
		{ TEXT("Use route A, then B; arrive at 3:30.") });
	AddCase(TEXT("separate quoted alternatives remain balanced"),
		TEXT("Record Note \"Sensitive Skin\" or \"Mixed Skin\""),
		TEXT("Record Note"),
		{ TEXT("\"Sensitive Skin\" or \"Mixed Skin\"") });
	AddCase(TEXT("multiple brace phrases are not mistaken for one wrapper"),
		TEXT("Record Note {Line one} {Line two}"),
		TEXT("Record Note"),
		{ TEXT("{Line one} {Line two}") });
	AddCase(TEXT("an interior parameter word remains authored text"),
		TEXT("Record Note The word message: belongs in this note."),
		TEXT("Record Note"),
		{ TEXT("The word message: belongs in this note.") });
	AddCase(TEXT("spaced leading punctuation remains authored text"),
		TEXT("Record Note : keep this leading marker."),
		TEXT("Record Note"),
		{ TEXT(": keep this leading marker.") });

	// Connector actions: normal output, schema-label mimicry, and positional
	// brace/quote fallbacks seen from weaker models.
	AddCase(TEXT("connector with multi-word destination"),
		TEXT("Escort User to Main Gallery"), TEXT("Escort"),
		{ TEXT("User"), TEXT("Main Gallery") });
	AddCase(TEXT("single outer brace with named connector values"),
		TEXT("Escort{character:User, destination:Main Gallery}"),
		TEXT("Escort"), { TEXT("User"), TEXT("Main Gallery") });
	AddCase(TEXT("named values retain connector wording"),
		TEXT("Escort{character:Museum Guide to destination:Gallery: North Wing}"),
		TEXT("Escort"), { TEXT("Museum Guide"), TEXT("Gallery: North Wing") });
	AddCase(TEXT("positional brace values support multi-word names"),
		TEXT("Escort {Museum Guide} {Gallery: North Wing}"),
		TEXT("Escort"), { TEXT("Museum Guide"), TEXT("Gallery: North Wing") });
	AddCase(TEXT("positional quoted values support punctuation"),
		TEXT("Escort \"Museum Guide\" \"Gallery, North Wing\""),
		TEXT("Escort"), { TEXT("Museum Guide"), TEXT("Gallery, North Wing") });
	AddCase(TEXT("a connector may separate positional quoted values"),
		TEXT("Escort \"Museum Guide\" to \"Gallery, North Wing\""),
		TEXT("Escort"), { TEXT("Museum Guide"), TEXT("Gallery, North Wing") });
	AddCase(TEXT("quoted positional values allow sentence punctuation"),
		TEXT("Escort \"User\" \"Gallery\"."), TEXT("Escort"),
		{ TEXT("User"), TEXT("Gallery") });
	AddCase(TEXT("mixed quoted positional values remain compatible"),
		TEXT("Escort \"User\" Gallery"), TEXT("Escort"),
		{ TEXT("User"), TEXT("Gallery") });
	AddCase(TEXT("mixed quoted positional values allow sentence punctuation"),
		TEXT("Escort User \"Gallery\"."), TEXT("Escort"),
		{ TEXT("User"), TEXT("Gallery") });
	AddCase(TEXT("a quoted connector word remains an explicit value"),
		TEXT("Escort \"To\" Gallery"), TEXT("Escort"),
		{ TEXT("To"), TEXT("Gallery") });
	AddCase(TEXT("a repeated connector remains part of the second value"),
		TEXT("Escort User to Path to Main Gallery"),
		TEXT("Escort"), { TEXT("User"), TEXT("Path to Main Gallery") });
	AddCase(TEXT("simple missing-connector fallback remains compatible"),
		TEXT("Escort User Gallery"), TEXT("Escort"),
		{ TEXT("User"), TEXT("Gallery") });
	AddCase(TEXT("ambiguous multi-word missing connector is not guessed"),
		TEXT("Escort Museum Guide Main Gallery"), TEXT("Escort"),
		{ FString(), FString() });
	AddCase(TEXT("connector with unrestricted free text"),
		TEXT("Send Message Welcome aboard to User"), TEXT("Send Message"),
		{ TEXT("Welcome aboard"), TEXT("User") });
	AddCase(TEXT("quoted free text may contain the connector word"),
		TEXT("Send Message \"Go to the gallery\" \"User\""), TEXT("Send Message"),
		{ TEXT("Go to the gallery"), TEXT("User") });
	AddCase(TEXT("quoted free text may precede an explicit connector"),
		TEXT("Send Message \"Go to the gallery\" to User"), TEXT("Send Message"),
		{ TEXT("Go to the gallery"), TEXT("User") });
	AddCase(TEXT("quotes inside free text do not become parameter slots"),
		TEXT("Send Message Tell them \"yes\" or \"no\" to Guide"),
		TEXT("Send Message"),
		{ TEXT("Tell them \"yes\" or \"no\""), TEXT("Guide") });
	AddCase(TEXT("a connector inside a quoted recipient is authored text"),
		TEXT("Send Message Hi to \"Guide to Gallery\""), TEXT("Send Message"),
		{ TEXT("Hi"), TEXT("Guide to Gallery") });
	AddCase(TEXT("named free text may contain the connector word"),
		TEXT("Send Message{message:Go to the gallery, recipient:User}"),
		TEXT("Send Message"), { TEXT("Go to the gallery"), TEXT("User") });
	AddCase(TEXT("ambiguous repeated connector in free text is not guessed"),
		TEXT("Send Message Go to Gallery to User"), TEXT("Send Message"),
		{ FString(), FString() });
	AddCase(TEXT("quoted alternatives without a recipient are not two values"),
		TEXT("Send Message \"yes\" or \"no\""), TEXT("Send Message"),
		{ FString(), FString() });
	AddCase(TEXT("one quoted value is not split into two slots"),
		TEXT("Send Message \"Hello there\""), TEXT("Send Message"),
		{ FString(), FString() });
	AddCase(TEXT("quoted missing value with punctuation is not fabricated"),
		TEXT("Send Message \"Hello\"."), TEXT("Send Message"),
		{ FString(), FString() });
	AddCase(TEXT("a trailing connector does not become a value"),
		TEXT("Send Message Hello to"), TEXT("Send Message"),
		{ FString(), FString() });
	AddCase(TEXT("a leading connector does not become a value"),
		TEXT("Send Message to Guide"), TEXT("Send Message"),
		{ FString(), FString() });
	AddCase(TEXT("a sentence-final connector does not become a value"),
		TEXT("Send Message Hello to."), TEXT("Send Message"),
		{ FString(), FString() });

	FConvaiEnvironmentData EmptyEnvironment;
	for (const FResponseCase& Case : Cases)
	{
		const FString Context = FString::Printf(
			TEXT("%s [%s]"), *Case.Label, *Case.RawResponse);
		const FConvaiAction* Template =
			UConvaiActions::FindActionTemplate(Case.RawResponse, Templates);
		if (!TestNotNull(*Context, Template))
		{
			continue;
		}

		TestEqual(
			*(Context + TEXT(" action")),
			Template->Name,
			Case.ExpectedAction);

		const FString Blob =
			UConvaiActions::StripActionPrefix(Case.RawResponse, Template->Name);
		const TArray<FString> RawValues =
			UConvaiActions::SplitParamValues(Blob, Template->Parameters);
		TestEqual(
			*(Context + TEXT(" value count")),
			RawValues.Num(),
			Template->Parameters.Num());

		for (int32 Index = 0;
			Index < FMath::Min(RawValues.Num(), Case.ExpectedValues.Num());
			++Index)
		{
			const FConvaiActionParam& Decl = Template->Parameters[Index];
			const FString CleanValue = UConvaiActions::StripParamNameMimicry(
				RawValues[Index], Decl.Name);
			const FConvaiResultParam Result = UConvaiActions::CoerceParam(
				CleanValue,
				Decl.Type,
				EmptyEnvironment,
				Decl.EnumType,
				&Decl.Choices);
			TestEqual(
				*FString::Printf(TEXT("%s value %d"), *Context, Index),
				Result.StringValue,
				Case.ExpectedValues[Index]);
		}
	}

	// Ambiguous or unrelated forms must not be silently reinterpreted.
	TestNull(TEXT("Parenthesized action syntax is not guessed"),
		UConvaiActions::FindActionTemplate(TEXT("Move To(Cube)"), Templates));
	TestNull(TEXT("Leading prose is not mistaken for an action"),
		UConvaiActions::FindActionTemplate(
			TEXT("I think we should Escort User to Gallery"), Templates));

	const FConvaiAction* MissingValueTemplate =
		UConvaiActions::FindActionTemplate(TEXT("Move To"), Templates);
	if (TestNotNull(TEXT("An action with a missing value still identifies the action"),
		MissingValueTemplate))
	{
		const TArray<FString> MissingValues = UConvaiActions::SplitParamValues(
			UConvaiActions::StripActionPrefix(
				TEXT("Move To"), MissingValueTemplate->Name),
			MissingValueTemplate->Parameters);
		TestEqual(TEXT("The missing parameter slot is empty"),
			MissingValues.Num(), 1);
		if (!MissingValues.IsEmpty())
		{
			TestTrue(TEXT("No parameter is fabricated"), MissingValues[0].IsEmpty());
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
