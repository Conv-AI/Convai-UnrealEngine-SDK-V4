// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "ConvaiMergedObjectNavigation.h"

#include "ConvaiChatbotComponent.h"
#include "ConvaiObjectComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FScopedMergedNavigationWorld
	{
		FScopedMergedNavigationWorld()
		{
			const FName WorldName = MakeUniqueObjectName(
				GetTransientPackage(),
				UWorld::StaticClass(),
				TEXT("ConvaiMergedNavigationTestWorld"));
			World = UWorld::CreateWorld(
				EWorldType::Editor,
				false,
				WorldName,
				GetTransientPackage(),
				true);
		}

		~FScopedMergedNavigationWorld()
		{
			if (World)
			{
				World->DestroyWorld(false);
				World->MarkObjectsPendingKill();
			}
		}

		AActor* SpawnActorAt(const FVector& Location) const
		{
			if (!World)
			{
				return nullptr;
			}

			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags = RF_Transient;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AActor* Actor = World->SpawnActor<AActor>(SpawnParameters);
			if (!Actor)
			{
				return nullptr;
			}

			USceneComponent* Root = NewObject<USceneComponent>(Actor, NAME_None, RF_Transient);
			Actor->SetRootComponent(Root);
			Root->RegisterComponent();
			Root->SetWorldLocation(Location);
			return Actor;
		}

		UConvaiObjectComponent* AddMergedObject(AActor* Actor, const FString& Name) const
		{
			if (!Actor)
			{
				return nullptr;
			}

			UConvaiObjectComponent* Component =
				NewObject<UConvaiObjectComponent>(Actor, NAME_None, RF_Transient);
			Actor->AddInstanceComponent(Component);
			Component->ObjectEntry.Name = Name;
			Component->ObjectEntry.Ref = Actor;
			Component->bMergeWithSameNamedObjects = true;
			Component->RegisterComponent();
			return Component;
		}

		UConvaiChatbotComponent* AddChatbot(AActor* Actor) const
		{
			if (!Actor)
			{
				return nullptr;
			}

			UConvaiChatbotComponent* Component =
				NewObject<UConvaiChatbotComponent>(Actor, NAME_None, RF_Transient);
			Actor->AddInstanceComponent(Component);
			Component->RegisterComponent();
			return Component;
		}

		UWorld* World = nullptr;
	};

	FConvaiMovementPoint MakeNamedPoint(const FString& Name, const FVector& RelativeLocation)
	{
		FConvaiMovementPoint Point;
		Point.Name = Name;
		Point.bCreatesSeparateDestination = true;
		Point.bEnabled = true;
		Point.Attachment = EConvaiMovementPointAttachment::RelativeToObject;
		Point.Transform.SetLocation(RelativeLocation);
		return Point;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMergedObjectActionNearestMemberTest,
	"Convai.Actions.MergedObjects.ActionTimeNearestMember",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FConvaiMergedObjectActionNearestMemberTest::RunTest(const FString& Parameters)
{
	FScopedMergedNavigationWorld Scene;
	if (!TestNotNull(TEXT("A disposable world is available"), Scene.World))
	{
		return false;
	}

	AActor* Source = Scene.SpawnActorAt(FVector::ZeroVector);
	AActor* FirstActor = Scene.SpawnActorAt(FVector(1000.0f, 0.0f, 0.0f));
	AActor* NearestActor = Scene.SpawnActorAt(FVector::ZeroVector);
	UConvaiObjectComponent* First = Scene.AddMergedObject(FirstActor, TEXT("Museum plinth"));
	UConvaiObjectComponent* Nearest = Scene.AddMergedObject(NearestActor, TEXT("Museum plinth"));
	UConvaiChatbotComponent* Chatbot = Scene.AddChatbot(Source);
	if (!TestNotNull(TEXT("The source actor exists"), Source)
		|| !TestNotNull(TEXT("The first merged member exists"), First)
		|| !TestNotNull(TEXT("The nearest merged member exists"), Nearest)
		|| !TestNotNull(TEXT("The chatbot exists"), Chatbot))
	{
		return false;
	}

	FConvaiObjectEntry LogicalEntry = First->ObjectEntry;
	LogicalEntry.Description = TEXT("A logical group of matching museum plinths.");
	const TArray<UConvaiObjectComponent*> Registry = { First, Nearest };

	FConvaiObjectEntry ResolvedEntry;
	TestTrue(
		TEXT("A concrete member already at the source resolves without spatial polling or nav data"),
		ConvaiMergedObjectNavigation::ResolveNearestReachableMember(
			Source, LogicalEntry, Registry, ResolvedEntry));
	TestEqual(TEXT("The nearest concrete actor wins over the first registered actor"),
		ResolvedEntry.Ref.Get(), NearestActor);
	TestEqual(TEXT("The logical group name is preserved"),
		ResolvedEntry.Name, LogicalEntry.Name);
	TestEqual(TEXT("The logical group description is preserved"),
		ResolvedEntry.Description, LogicalEntry.Description);

	FirstActor->GetRootComponent()->SetWorldLocation(FVector::ZeroVector);
	TestTrue(TEXT("An exact arrival tie still resolves"),
		ConvaiMergedObjectNavigation::ResolveNearestReachableMember(
			Source, LogicalEntry, Registry, ResolvedEntry));
	TestEqual(TEXT("Exact ties preserve registry order"),
		ResolvedEntry.Ref.Get(), FirstActor);
	FirstActor->GetRootComponent()->SetWorldLocation(FVector(1000.0f, 0.0f, 0.0f));

	Chatbot->EnvironmentData.Objects.Add(LogicalEntry);
	FConvaiResultParam TargetParam;
	TargetParam.Type = EConvaiActionParamType::Reference;
	TargetParam.RefValue = LogicalEntry;
	FConvaiResultAction Action;
	Action.Action = TEXT("Move To");
	Action.Parameters.Add(TEXT("target"), TargetParam);
	Action.RelatedObjectOrCharacter = LogicalEntry;

	ConvaiMergedObjectNavigation::ResolveActionObjectReferences(*Chatbot, Action, Registry);
	const FConvaiResultParam* ResolvedParam = Action.Parameters.Find(TEXT("target"));
	if (TestNotNull(TEXT("The target parameter remains available"), ResolvedParam))
	{
		TestEqual(TEXT("The dispatched action parameter uses the nearest member"),
			ResolvedParam->RefValue.Ref.Get(), NearestActor);
		TestEqual(TEXT("The deprecated mirror follows the resolved reference"),
			Action.RelatedObjectOrCharacter.Ref.Get(), NearestActor);
		TestEqual(TEXT("The deprecated mirror retains the logical name"),
			Action.RelatedObjectOrCharacter.Name, LogicalEntry.Name);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMergedObjectActionSubObjectTest,
	"Convai.Actions.MergedObjects.NamedDestinationSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FConvaiMergedObjectActionSubObjectTest::RunTest(const FString& Parameters)
{
	FScopedMergedNavigationWorld Scene;
	if (!TestNotNull(TEXT("A disposable world is available"), Scene.World))
	{
		return false;
	}

	AActor* Source = Scene.SpawnActorAt(FVector(500.0f, 0.0f, 0.0f));
	AActor* FirstActor = Scene.SpawnActorAt(FVector(2000.0f, 0.0f, 0.0f));
	AActor* NearestActor = Scene.SpawnActorAt(FVector::ZeroVector);
	AActor* MissingPointActor = Scene.SpawnActorAt(FVector(500.0f, 0.0f, 0.0f));
	UConvaiObjectComponent* First = Scene.AddMergedObject(FirstActor, TEXT("Gallery door"));
	UConvaiObjectComponent* Nearest = Scene.AddMergedObject(NearestActor, TEXT("Gallery door"));
	UConvaiObjectComponent* MissingPoint =
		Scene.AddMergedObject(MissingPointActor, TEXT("Gallery door"));
	if (!TestNotNull(TEXT("The source actor exists"), Source)
		|| !TestNotNull(TEXT("The first merged member exists"), First)
		|| !TestNotNull(TEXT("The nearest merged member exists"), Nearest)
		|| !TestNotNull(TEXT("The member without the named point exists"), MissingPoint))
	{
		return false;
	}

	First->ObjectEntry.MovementPoints.Add(MakeNamedPoint(TEXT("Other Side"), FVector(500.0f, 0.0f, 0.0f)));
	Nearest->ObjectEntry.MovementPoints.Add(MakeNamedPoint(TEXT("Other Side"), FVector(500.0f, 0.0f, 0.0f)));
	const TArray<UConvaiObjectComponent*> Registry = { First, MissingPoint, Nearest };

	FConvaiObjectEntry LogicalDestination =
		First->ObjectEntry.MakeMovementPointSubEntry(TEXT("Other Side"));
	LogicalDestination.Description = TEXT("The other side of the logical gallery door.");
	FConvaiObjectEntry ResolvedDestination;
	TestTrue(TEXT("A named destination resolves across the merged members"),
		ConvaiMergedObjectNavigation::ResolveNearestReachableMember(
			Source, LogicalDestination, Registry, ResolvedDestination));
	TestEqual(TEXT("The member owning the arrived named point wins"),
		ResolvedDestination.Ref.Get(), NearestActor);
	TestEqual(TEXT("The generated destination name remains logical"),
		ResolvedDestination.Name, LogicalDestination.Name);
	TestTrue(TEXT("The generated destination marker is preserved"),
		ResolvedDestination.bIsMovementPointSubObject);
	TestEqual(TEXT("Only the requested destination points remain"),
		ResolvedDestination.MovementPoints.Num(), 1);

	// The base entry of an expanded group must ignore separate-destination
	// points. Move the source onto the concrete actor body: the second member
	// should win by body arrival, not by its named point 500 units away.
	Source->GetRootComponent()->SetWorldLocation(FVector::ZeroVector);
	FConvaiObjectEntry LogicalBase = First->ObjectEntry;
	LogicalBase.FilterMovementPointsToObjectItself();
	FConvaiObjectEntry ResolvedBase;
	TestTrue(TEXT("The expanded base object resolves against concrete object bodies"),
		ConvaiMergedObjectNavigation::ResolveNearestReachableMember(
			Source, LogicalBase, Registry, ResolvedBase));
	TestEqual(TEXT("The nearest body wins for the base logical object"),
		ResolvedBase.Ref.Get(), NearestActor);
	TestEqual(TEXT("Named destination points do not leak back into the base entry"),
		ResolvedBase.MovementPoints.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMergedObjectActionNoReachableFallbackTest,
	"Convai.Actions.MergedObjects.NoReachablePreservesFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FConvaiMergedObjectActionNoReachableFallbackTest::RunTest(const FString& Parameters)
{
	FScopedMergedNavigationWorld Scene;
	if (!TestNotNull(TEXT("A disposable world is available"), Scene.World))
	{
		return false;
	}

	AActor* Source = Scene.SpawnActorAt(FVector::ZeroVector);
	AActor* FirstActor = Scene.SpawnActorAt(FVector(1000.0f, 0.0f, 0.0f));
	AActor* SecondActor = Scene.SpawnActorAt(FVector(2000.0f, 0.0f, 0.0f));
	UConvaiObjectComponent* First = Scene.AddMergedObject(FirstActor, TEXT("Remote exhibit"));
	UConvaiObjectComponent* Second = Scene.AddMergedObject(SecondActor, TEXT("Remote exhibit"));
	if (!TestNotNull(TEXT("The source actor exists"), Source)
		|| !TestNotNull(TEXT("The first merged member exists"), First)
		|| !TestNotNull(TEXT("The second merged member exists"), Second))
	{
		return false;
	}

	FConvaiObjectEntry LogicalEntry = First->ObjectEntry;
	LogicalEntry.Description = TEXT("A logical remote exhibit.");
	const TArray<UConvaiObjectComponent*> Registry = { First, Second };
	FConvaiObjectEntry ResolvedEntry;
	TestFalse(TEXT("No member is invented as reachable when nav data is absent"),
		ConvaiMergedObjectNavigation::ResolveNearestReachableMember(
			Source, LogicalEntry, Registry, ResolvedEntry));
	TestEqual(TEXT("The established fallback actor remains unchanged"),
		ResolvedEntry.Ref.Get(), FirstActor);
	TestEqual(TEXT("The fallback logical name remains unchanged"),
		ResolvedEntry.Name, LogicalEntry.Name);
	TestEqual(TEXT("The fallback logical description remains unchanged"),
		ResolvedEntry.Description, LogicalEntry.Description);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
