// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "RestAPI/ConvaiKnowledgeBankProxy.h"
#include "ConvaiDefinitions.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiKnowledgeBankUploadResponseTest,
	"Convai.KnowledgeBank.Parsing.UploadResponse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiKnowledgeBankUploadResponseTest::RunTest(const FString& Parameters)
{
	const FString Response = TEXT(R"({
		"id": "0a1b2c3d-4e5f-6789-abcd-ef0123456789",
		"file_name": "photosynthesis.txt",
		"is_available": false,
		"status": "inactive",
		"timestamp": "2024-09-17 20:51:09.216522",
		"file_size": "72374"
	})");

	FConvaiKnowledgeBankDocument Document;
	if (!TestTrue(TEXT("The upload response parses"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocument(Response, Document)))
	{
		return false;
	}

	TestEqual(TEXT("The document ID is read"),
		Document.DocumentID, FString(TEXT("0a1b2c3d-4e5f-6789-abcd-ef0123456789")));
	TestEqual(TEXT("The file name is read"),
		Document.FileName, FString(TEXT("photosynthesis.txt")));
	TestFalse(TEXT("A freshly uploaded document is not yet available"), Document.bIsAvailable);
	TestFalse(TEXT("An inactive document is not connected"), Document.bConnected);
	TestEqual(TEXT("The timestamp keeps its microseconds"),
		Document.Timestamp, FString(TEXT("2024-09-17 20:51:09.216522")));
	TestEqual(TEXT("A quoted file size becomes an integer"),
		Document.FileSizeBytes, static_cast<int64>(72374));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiKnowledgeBankNumericFileSizeTest,
	"Convai.KnowledgeBank.Parsing.NumericFileSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiKnowledgeBankNumericFileSizeTest::RunTest(const FString& Parameters)
{
	const FString Response = TEXT(R"({"id": "abc", "file_size": 72374, "status": "active"})");

	FConvaiKnowledgeBankDocument Document;
	if (!TestTrue(TEXT("A response with an unquoted file size parses"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocument(Response, Document)))
	{
		return false;
	}

	TestEqual(TEXT("An unquoted file size becomes the same integer"),
		Document.FileSizeBytes, static_cast<int64>(72374));
	TestTrue(TEXT("An active document is connected"), Document.bConnected);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiKnowledgeBankRejectsErrorBodyTest,
	"Convai.KnowledgeBank.Parsing.RejectsErrorBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiKnowledgeBankRejectsErrorBodyTest::RunTest(const FString& Parameters)
{
	FConvaiKnowledgeBankDocument Document;

	TestFalse(TEXT("An error body carrying no ID is not a document"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocument(
			TEXT(R"({"API_ERROR": "Invalid API key provided."})"), Document));

	TestFalse(TEXT("Malformed JSON is not a document"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocument(
			TEXT("{\"Successfully deleted document\"}"), Document));

	TestFalse(TEXT("An empty body is not a document"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocument(FString(), Document));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiKnowledgeBankListOfObjectsTest,
	"Convai.KnowledgeBank.Parsing.ListOfObjects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiKnowledgeBankListOfObjectsTest::RunTest(const FString& Parameters)
{
	const FString Response = TEXT(R"({
		"docs": [
			{
				"id": "first-uuid",
				"file_name": "photosynthesis.txt",
				"is_available": true,
				"status": "inactive",
				"timestamp": "2024-09-17 21:44:53.201385",
				"file_size": "1404"
			},
			{
				"id": "second-uuid",
				"file_name": "FAQ.txt",
				"is_available": true,
				"status": "active",
				"timestamp": "2024-09-17 21:21:35.566677",
				"file_size": "736"
			}
		]
	})");

	TArray<FConvaiKnowledgeBankDocument> Documents;
	if (!TestTrue(TEXT("A docs array of objects parses"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocumentArray(Response, Documents)))
	{
		return false;
	}

	if (!TestEqual(TEXT("Both documents are recovered"), Documents.Num(), 2))
	{
		return false;
	}

	TestEqual(TEXT("The first document ID is read"),
		Documents[0].DocumentID, FString(TEXT("first-uuid")));
	TestTrue(TEXT("A processed document is available"), Documents[0].bIsAvailable);
	TestFalse(TEXT("An inactive document is not connected"), Documents[0].bConnected);
	TestEqual(TEXT("The first file size is read"),
		Documents[0].FileSizeBytes, static_cast<int64>(1404));

	TestEqual(TEXT("The second document ID is read"),
		Documents[1].DocumentID, FString(TEXT("second-uuid")));
	TestTrue(TEXT("An active document is connected"), Documents[1].bConnected);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiKnowledgeBankListOfStringsTest,
	"Convai.KnowledgeBank.Parsing.ListOfJsonStrings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiKnowledgeBankListOfStringsTest::RunTest(const FString& Parameters)
{
	// The published response shows each entry as a string holding the object's
	// JSON rather than the object itself. Both shapes have to land the same way.
	const FString Response = TEXT(R"({
		"docs": [
			"{\"id\": \"first-uuid\", \"file_name\": \"photosynthesis.txt\", \"is_available\": true, \"status\": \"inactive\", \"file_size\": \"1404\"}",
			"{\"id\": \"second-uuid\", \"file_name\": \"FAQ.txt\", \"is_available\": true, \"status\": \"active\", \"file_size\": \"736\"}"
		]
	})");

	TArray<FConvaiKnowledgeBankDocument> Documents;
	if (!TestTrue(TEXT("A docs array of JSON strings parses"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocumentArray(Response, Documents)))
	{
		return false;
	}

	if (!TestEqual(TEXT("Both documents are recovered"), Documents.Num(), 2))
	{
		return false;
	}

	TestEqual(TEXT("The first document ID is read"),
		Documents[0].DocumentID, FString(TEXT("first-uuid")));
	TestEqual(TEXT("The first file name is read"),
		Documents[0].FileName, FString(TEXT("photosynthesis.txt")));
	TestEqual(TEXT("The first file size is read"),
		Documents[0].FileSizeBytes, static_cast<int64>(1404));
	TestTrue(TEXT("An active document is connected"), Documents[1].bConnected);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiKnowledgeBankListEdgeCasesTest,
	"Convai.KnowledgeBank.Parsing.ListEdgeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiKnowledgeBankListEdgeCasesTest::RunTest(const FString& Parameters)
{
	TArray<FConvaiKnowledgeBankDocument> Documents;

	TestTrue(TEXT("An empty docs array parses"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocumentArray(TEXT(R"({"docs": []})"), Documents));
	TestEqual(TEXT("An empty docs array yields no documents"), Documents.Num(), 0);

	TestFalse(TEXT("An error body has no docs array"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocumentArray(
			TEXT(R"({"API_ERROR": "Invalid API key provided."})"), Documents));

	TestFalse(TEXT("Malformed JSON has no docs array"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocumentArray(TEXT("not json at all"), Documents));

	// One good entry, one unparsable string, one object with no id.
	const FString Mixed = TEXT(R"({
		"docs": [
			{"id": "good-uuid", "file_name": "keep.txt"},
			"{ this is not json }",
			{"file_name": "no-id.txt"}
		]
	})");

	TestTrue(TEXT("A mixed docs array still parses"),
		UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocumentArray(Mixed, Documents));
	if (!TestEqual(TEXT("Only the usable entry survives"), Documents.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("The surviving entry is the one with an ID"),
		Documents[0].DocumentID, FString(TEXT("good-uuid")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
