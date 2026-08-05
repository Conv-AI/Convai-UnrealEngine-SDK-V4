// Copyright 2022 Convai Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ConvaiContextPrivate
{
	float ComputeNearestDistance(
		const FVector& ObserverLocation,
		TConstArrayView<FVector> CandidateLocations,
		const FVector& FallbackLocation);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMergedObjectNearestProximityTest,
	"Convai.Spatial.MergedObjects.NearestProximityDistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FConvaiMergedObjectNearestProximityTest::RunTest(const FString& Parameters)
{
	const FVector Observer = FVector::ZeroVector;
	const FVector Centroid(1000.0f, 0.0f, 0.0f);
	const TArray<FVector> Members = {
		FVector(100.0f, 0.0f, 0.0f),
		FVector(1900.0f, 0.0f, 0.0f)
	};

	TestEqual(
		TEXT("A merged object's scalar distance uses its nearest concrete member"),
		ConvaiContextPrivate::ComputeNearestDistance(Observer, Members, Centroid),
		100.0f);

	TestEqual(
		TEXT("A subject without concrete member points retains its aggregate distance"),
		ConvaiContextPrivate::ComputeNearestDistance(
			Observer,
			TConstArrayView<FVector>(),
			Centroid),
		1000.0f);

	const TArray<FVector> SingleMember = { Centroid };
	TestEqual(
		TEXT("A normal one-member object preserves the previous distance"),
		ConvaiContextPrivate::ComputeNearestDistance(Observer, SingleMember, Centroid),
		1000.0f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
