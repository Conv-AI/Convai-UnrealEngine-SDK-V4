// Copyright Convai Inc. All Rights Reserved.

#include "ConvaiToolset.h"
#include "ConvaiToolsetCommon.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/NavMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "ScopedTransaction.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationSystem.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "Model.h"

#include "ConvaiChatbotComponent.h"
#include "ConvaiFaceSync.h"
#include "ConvaiDefinitions.h"

#define LOCTEXT_NAMESPACE "ConvaiSetupToolset"

namespace
{
	// Content paths for the convenience BP components and MetaHuman anim BPs.
	const TCHAR* ChatbotComponentBPPath = TEXT("/Convai/ConvaiConveniencePack/ConvaiBPComponent/BP_ConvaiChatbotComponent.BP_ConvaiChatbotComponent");
	const TCHAR* PlayerComponentBPPath  = TEXT("/Convai/ConvaiConveniencePack/ConvaiBPComponent/BP_ConvaiPlayerComponent.BP_ConvaiPlayerComponent");
	const TCHAR* FaceAnimBPPath         = TEXT("/Convai/MetaHumans/Animations/Convai_MetaHuman_FaceAnim.Convai_MetaHuman_FaceAnim");
	const TCHAR* BodyAnimBPPath         = TEXT("/Convai/MetaHumans/Animations/Convai_MetaHuman_BodyAnim.Convai_MetaHuman_BodyAnim");

	/** Apply Convai's nav-movement defaults to any UNavMovementComponent subclass.
	 *  Faithful replication of FConvaiContentBrowserContextMenu::ApplyNavMovementDefaults. */
	void ApplyNavMovementDefaults(UNavMovementComponent* Comp)
	{
		if (!Comp)
		{
			return;
		}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		if (FNavMovementProperties* NavProps = Comp->GetNavMovementProperties())
		{
			NavProps->FixedPathBrakingDistance = 0.f;
			NavProps->bUpdateNavAgentWithOwnersCollision = true;
			NavProps->bUseAccelerationForPaths = true;
			NavProps->bUseFixedBrakingDistanceForPaths = false;
			NavProps->bStopMovementAbortPaths = true;
		}
#else
		auto SetReflectedBool = [Comp](FName PropertyName, bool bValue)
		{
			if (FBoolProperty* Prop = FindFProperty<FBoolProperty>(Comp->GetClass(), PropertyName))
			{
				Prop->SetPropertyValue_InContainer(Comp, bValue);
			}
		};
		auto SetReflectedFloat = [Comp](FName PropertyName, float Value)
		{
			if (FFloatProperty* Prop = FindFProperty<FFloatProperty>(Comp->GetClass(), PropertyName))
			{
				Prop->SetPropertyValue_InContainer(Comp, Value);
			}
		};
		SetReflectedFloat(TEXT("FixedPathBrakingDistance"), 0.f);
		SetReflectedBool(TEXT("bUpdateNavAgentWithOwnersCollision"), true);
		SetReflectedBool(TEXT("bUseAccelerationForPaths"), true);
		SetReflectedBool(TEXT("bUseFixedBrakingDistanceForPaths"), false);
		SetReflectedBool(TEXT("bStopMovementAbortPaths"), true);
#endif
	}

	void ApplyFloatingPawnMovementDefaults(UFloatingPawnMovement* Comp)
	{
		if (!Comp)
		{
			return;
		}
		Comp->Modify();
		Comp->MaxSpeed = 375.f;
		Comp->Acceleration = 200.f;
		Comp->Deceleration = 250.f;
		Comp->TurningBoost = 3.f;
		ApplyNavMovementDefaults(Comp);

		ConvaiToolsetCommon::NotifyPropertyChanged(Comp, TEXT("MaxSpeed"));
		ConvaiToolsetCommon::NotifyPropertyChanged(Comp, TEXT("Acceleration"));
		ConvaiToolsetCommon::NotifyPropertyChanged(Comp, TEXT("Deceleration"));
		ConvaiToolsetCommon::NotifyPropertyChanged(Comp, TEXT("TurningBoost"));
		ConvaiToolsetCommon::NotifyPropertyChanged(Comp, TEXT("NavMovementProperties"));
	}

	void ApplyCharacterMovementDefaults(UCharacterMovementComponent* Comp)
	{
		if (!Comp)
		{
			return;
		}
		Comp->Modify();
		Comp->MaxWalkSpeed = 375.f;
		Comp->MaxAcceleration = 200.f;
		Comp->BrakingDecelerationWalking = 250.f;
		ApplyNavMovementDefaults(Comp);

		Comp->NavAgentProps.AgentRadius = 100.f;
		Comp->NavAgentProps.AgentHeight = 300.f;
		Comp->NavAgentProps.AgentStepHeight = -1.f;
		Comp->NavAgentProps.NavWalkingSearchHeightScale = 500.f;
		Comp->NavAgentProps.bCanCrouch = false;
		Comp->NavAgentProps.bCanJump = false;
		Comp->NavAgentProps.bCanWalk = true;
		Comp->NavAgentProps.bCanSwim = false;
		Comp->NavAgentProps.bCanFly = false;
		Comp->NavAgentProps.PreferredNavData.Reset();

		ConvaiToolsetCommon::NotifyPropertyChanged(Comp, TEXT("MaxWalkSpeed"));
		ConvaiToolsetCommon::NotifyPropertyChanged(Comp, TEXT("MaxAcceleration"));
		ConvaiToolsetCommon::NotifyPropertyChanged(Comp, TEXT("BrakingDecelerationWalking"));
		ConvaiToolsetCommon::NotifyPropertyChanged(Comp, TEXT("NavMovementProperties"));
		ConvaiToolsetCommon::NotifyPropertyChanged(Comp, TEXT("NavAgentProps"));
	}

	UCharacterMovementComponent* GetBlueprintCharacterMovement(UBlueprint* Blueprint)
	{
		if (!Blueprint || !Blueprint->GeneratedClass)
		{
			return nullptr;
		}
		if (AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject()))
		{
			return CDO->FindComponentByClass<UCharacterMovementComponent>();
		}
		return nullptr;
	}

	/** Loads the BlueprintGeneratedClass for a content-asset Blueprint component
	 *  (e.g. BP_ConvaiChatbotComponent) so it can be used as an SCS ComponentClass. */
	UClass* LoadComponentBlueprintClass(const TCHAR* AssetPath)
	{
		if (UBlueprint* BP = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, AssetPath)))
		{
			return BP->GeneratedClass;
		}
		// Fall back to loading the generated class directly (path may already carry "_C").
		return LoadObject<UClass>(nullptr, AssetPath);
	}

	/** Finds an SCS skeletal-mesh node on the Blueprint by its variable / node name (e.g. "Face", "Body"). */
	USCS_Node* FindSkeletalMeshNodeByName(UBlueprint* Blueprint, const FName& Name)
	{
		if (!Blueprint || !Blueprint->SimpleConstructionScript)
		{
			return nullptr;
		}
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentClass
				&& Node->ComponentClass->IsChildOf(USkeletalMeshComponent::StaticClass())
				&& Node->GetVariableName() == Name)
			{
				return Node;
			}
		}
		return nullptr;
	}
}

FString UConvaiSetupToolset::SetupConvaiPawnMovement(const FString& CharacterBlueprintPath)
{
	FString Error;
	UBlueprint* Blueprint = ConvaiToolsetCommon::LoadBlueprintByPath(CharacterBlueprintPath, Error);
	if (!Blueprint)
	{
		return FString::Printf(TEXT("Error: %s"), *Error);
	}

	UClass* ParentClass = Blueprint->ParentClass;
	if (!ParentClass || !ParentClass->IsChildOf(AActor::StaticClass()))
	{
		return FString::Printf(TEXT("Error: '%s' parent is not an Actor; cannot set up pawn movement."), *Blueprint->GetName());
	}

	FScopedTransaction Transaction(LOCTEXT("SetupPawnMovementTx", "Setup Convai Pawn Movement"));
	Blueprint->Modify();

	const bool bIsCharacter = ParentClass->IsChildOf(ACharacter::StaticClass());
	const bool bIsPawn = ParentClass->IsChildOf(APawn::StaticClass());
	const bool bIsPureActor = (ParentClass == AActor::StaticClass());

	TArray<FString> Changes;

	bool bReparentedToPawn = false;
	if (bIsPureActor)
	{
		Blueprint->ParentClass = APawn::StaticClass();
		FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bReparentedToPawn = true;
		Changes.Add(TEXT("reparented Actor -> Pawn"));
	}

	const bool bUseFloatingMovement = bReparentedToPawn || (bIsPawn && !bIsCharacter);

	bool bNeedsStructuralCompile = false;
	bool bAnyDefaultsModified = false;

	if (bIsCharacter)
	{
		if (UCharacterMovementComponent* CharMove = GetBlueprintCharacterMovement(Blueprint))
		{
			ApplyCharacterMovementDefaults(CharMove);
			bAnyDefaultsModified = true;
			Changes.Add(TEXT("tuned CharacterMovementComponent"));
		}
	}
	else if (bUseFloatingMovement)
	{
		// Find an existing FloatingPawnMovement template in THIS BP's SCS, else add one.
		UFloatingPawnMovement* FPM = nullptr;
		bool bIsSCSOwned = false;
		if (USCS_Node* Existing = ConvaiToolsetCommon::FindSCSNodeOfClass(Blueprint, UFloatingPawnMovement::StaticClass()))
		{
			FPM = Cast<UFloatingPawnMovement>(Existing->ComponentTemplate);
			bIsSCSOwned = true;
		}
		else if (UClass* GeneratedClass = Blueprint->GeneratedClass)
		{
			// Inherited / native FPM: modify on this BP's CDO so changes save as a per-class delta.
			if (AActor* CDO = Cast<AActor>(GeneratedClass->GetDefaultObject()))
			{
				FPM = CDO->FindComponentByClass<UFloatingPawnMovement>();
			}
		}

		if (!FPM)
		{
			if (USCS_Node* NewNode = ConvaiToolsetCommon::AddSCSComponentNode(Blueprint, UFloatingPawnMovement::StaticClass(), TEXT("FloatingPawnMovement")))
			{
				FPM = Cast<UFloatingPawnMovement>(NewNode->ComponentTemplate);
				bIsSCSOwned = true;
				Changes.Add(TEXT("added FloatingPawnMovement"));
			}
		}

		if (FPM)
		{
			ApplyFloatingPawnMovementDefaults(FPM);
			bAnyDefaultsModified = true;
			Changes.Add(TEXT("applied FloatingPawnMovement defaults"));
			// Only compile when changes were applied to compile-safe storage (an SCS template
			// owned by this BP). For inherited components the CDO delta would be discarded by a recompile.
			if (bIsSCSOwned)
			{
				bNeedsStructuralCompile = true;
			}
		}
	}
	else
	{
		return FString::Printf(TEXT("Error: '%s' parent '%s' is an Actor subclass that is neither Pawn nor Character; "
			"reparent it to a Pawn-derived class and retry."), *Blueprint->GetName(), *ParentClass->GetName());
	}

	if (!bAnyDefaultsModified)
	{
		return FString::Printf(TEXT("No movement component could be set up on '%s'."), *Blueprint->GetName());
	}

	FString SaveError;
	ConvaiToolsetCommon::CompileAndSaveBlueprint(Blueprint, bNeedsStructuralCompile, SaveError);

	FString Result = FString::Printf(TEXT("Setup Convai pawn movement on '%s': %s."),
		*Blueprint->GetName(), *FString::Join(Changes, TEXT(", ")));
	if (!SaveError.IsEmpty())
	{
		Result += FString::Printf(TEXT(" WARNING: %s"), *SaveError);
	}
	return Result;
}

FString UConvaiSetupToolset::SetupConvaiCharacter(const FString& CharacterBlueprintPath, const FString& CharacterId)
{
	FString Error;
	UBlueprint* Blueprint = ConvaiToolsetCommon::LoadBlueprintByPath(CharacterBlueprintPath, Error);
	if (!Blueprint)
	{
		return FString::Printf(TEXT("Error: %s"), *Error);
	}

	FScopedTransaction Transaction(LOCTEXT("SetupCharacterTx", "Setup Convai Character"));
	Blueprint->Modify();

	TArray<FString> Changes;

	// Each template-property edit is routed through SetTemplatePropertyAndPropagate so the value
	// reaches already-placed level instances (those that still hold the template's old value),
	// preserving per-instance overrides. All edits happen BEFORE the single compile at the end.
	int32 TotalPropagated = 0;

	// 1. Chatbot component (BP_ConvaiChatbotComponent convenience class).
	UClass* ChatbotBPClass = LoadComponentBlueprintClass(ChatbotComponentBPPath);
	USCS_Node* ChatbotNode = nullptr;
	if (ChatbotBPClass)
	{
		ChatbotNode = ConvaiToolsetCommon::FindSCSNodeOfClass(Blueprint, UConvaiChatbotComponent::StaticClass());
		if (!ChatbotNode)
		{
			ChatbotNode = ConvaiToolsetCommon::AddSCSComponentNode(Blueprint, ChatbotBPClass, TEXT("ConvaiChatbot"));
			if (ChatbotNode)
			{
				Changes.Add(TEXT("added BP_ConvaiChatbotComponent"));
			}
		}
	}
	else
	{
		Changes.Add(TEXT("WARNING: could not load BP_ConvaiChatbotComponent"));
	}

	if (ChatbotNode)
	{
		if (UConvaiChatbotComponent* ChatbotTemplate = Cast<UConvaiChatbotComponent>(ChatbotNode->ComponentTemplate))
		{
			TotalPropagated += ConvaiToolsetCommon::SetTemplatePropertyAndPropagate(Blueprint, ChatbotNode, TEXT("CharacterID"),
				[&]() { ChatbotTemplate->CharacterID = CharacterId; });
			TotalPropagated += ConvaiToolsetCommon::SetTemplatePropertyAndPropagate(Blueprint, ChatbotNode, TEXT("bAutoFillConversationPartnerFromPlayer"),
				[&]() { ChatbotTemplate->bAutoFillConversationPartnerFromPlayer = true; });
			Changes.Add(FString::Printf(TEXT("set CharacterID='%s'"), *CharacterId));
		}
	}

	// 2. Native ConvaiFaceSyncComponent.
	USCS_Node* FaceSyncNode = ConvaiToolsetCommon::FindSCSNodeOfClass(Blueprint, UConvaiFaceSyncComponent::StaticClass());
	if (!FaceSyncNode)
	{
		FaceSyncNode = ConvaiToolsetCommon::AddSCSComponentNode(Blueprint, UConvaiFaceSyncComponent::StaticClass(), TEXT("ConvaiFaceSync"));
		if (FaceSyncNode)
		{
			Changes.Add(TEXT("added ConvaiFaceSyncComponent"));
		}
	}

	if (FaceSyncNode)
	{
		if (UConvaiFaceSyncComponent* FaceSyncTemplate = Cast<UConvaiFaceSyncComponent>(FaceSyncNode->ComponentTemplate))
		{
			TotalPropagated += ConvaiToolsetCommon::SetTemplatePropertyAndPropagate(Blueprint, FaceSyncNode, TEXT("LipSyncMode"),
				[&]() { FaceSyncTemplate->LipSyncMode = EC_LipSyncMode::BS_MHA; });
			TotalPropagated += ConvaiToolsetCommon::SetTemplatePropertyAndPropagate(Blueprint, FaceSyncNode, TEXT("bEnableInterpolation"),
				[&]() { FaceSyncTemplate->bEnableInterpolation = true; });
			Changes.Add(TEXT("set LipSyncMode=BS_MHA + interpolation"));
		}
	}

	// 3. Assign Convai MetaHuman anim BPs to the Face and Body skeletal mesh components.
	auto AssignAnim = [&](const FName& MeshNodeName, const TCHAR* AnimBPPath, const TCHAR* Label)
	{
		USCS_Node* MeshNode = FindSkeletalMeshNodeByName(Blueprint, MeshNodeName);
		if (!MeshNode)
		{
			return; // No such mesh on this BP — skip silently (not all characters have both).
		}
		USkeletalMeshComponent* MeshTemplate = Cast<USkeletalMeshComponent>(MeshNode->ComponentTemplate);
		if (!MeshTemplate)
		{
			return;
		}

		UClass* AnimClass = nullptr;
		if (UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, AnimBPPath)))
		{
			AnimClass = AnimBP->GeneratedClass;
		}
		if (!AnimClass)
		{
			Changes.Add(FString::Printf(TEXT("WARNING: could not load %s anim BP"), Label));
			return;
		}

		// SetAnimInstanceClass writes BOTH AnimClass AND AnimationMode. The general helper
		// propagates ONE named property per call, so route BOTH through it (AnimationMode first,
		// then AnimClass) — otherwise placed instances could get the new class but keep their old
		// AnimationMode and never actually run the AnimBP.
		TotalPropagated += ConvaiToolsetCommon::SetTemplatePropertyAndPropagate(Blueprint, MeshNode, TEXT("AnimationMode"),
			[&]()
			{
				MeshTemplate->SetAnimationMode(EAnimationMode::AnimationBlueprint);
			});
		TotalPropagated += ConvaiToolsetCommon::SetTemplatePropertyAndPropagate(Blueprint, MeshNode, TEXT("AnimClass"),
			[&]()
			{
				MeshTemplate->SetAnimInstanceClass(AnimClass);
			});
		Changes.Add(FString::Printf(TEXT("assigned %s anim BP to '%s'"), Label, *MeshNodeName.ToString()));
	};

	AssignAnim(TEXT("Face"), FaceAnimBPPath, TEXT("Face"));
	AssignAnim(TEXT("Body"), BodyAnimBPPath, TEXT("Body"));

	if (TotalPropagated > 0)
	{
		Changes.Add(FString::Printf(TEXT("propagated to %d placed instance prop(s)"), TotalPropagated));
	}

	FString SaveError;
	ConvaiToolsetCommon::CompileAndSaveBlueprint(Blueprint, /*bStructural*/ true, SaveError);

	FString Result = FString::Printf(TEXT("Setup Convai character on '%s': %s."),
		*Blueprint->GetName(), *FString::Join(Changes, TEXT(", ")));
	if (!SaveError.IsEmpty())
	{
		Result += FString::Printf(TEXT(" WARNING: %s"), *SaveError);
	}
	return Result;
}

FString UConvaiSetupToolset::SetupConvaiPlayer(const FString& PlayerBlueprintPath)
{
	FString Error;
	UBlueprint* Blueprint = ConvaiToolsetCommon::LoadBlueprintByPath(PlayerBlueprintPath, Error);
	if (!Blueprint)
	{
		return FString::Printf(TEXT("Error: %s"), *Error);
	}

	UClass* PlayerBPClass = LoadComponentBlueprintClass(PlayerComponentBPPath);
	if (!PlayerBPClass)
	{
		return TEXT("Error: could not load BP_ConvaiPlayerComponent.");
	}

	FScopedTransaction Transaction(LOCTEXT("SetupPlayerTx", "Setup Convai Player"));
	Blueprint->Modify();

	// Idempotent: only add if no UConvaiPlayerComponent (native or BP) already present.
	USCS_Node* Existing = ConvaiToolsetCommon::FindSCSNodeOfClass(Blueprint, PlayerBPClass);
	if (!Existing)
	{
		// Also accept any node already derived from the same generated class' native parent.
		Existing = ConvaiToolsetCommon::FindSCSNodeOfClass(Blueprint, PlayerBPClass->GetSuperClass());
	}

	if (Existing)
	{
		return FString::Printf(TEXT("'%s' already has a Convai player component; no change."), *Blueprint->GetName());
	}

	USCS_Node* NewNode = ConvaiToolsetCommon::AddSCSComponentNode(Blueprint, PlayerBPClass, TEXT("ConvaiPlayer"));
	if (!NewNode)
	{
		return FString::Printf(TEXT("Error: failed to add player component to '%s'."), *Blueprint->GetName());
	}

	FString SaveError;
	ConvaiToolsetCommon::CompileAndSaveBlueprint(Blueprint, /*bStructural*/ true, SaveError);

	FString Result = FString::Printf(TEXT("Added BP_ConvaiPlayerComponent to '%s'."), *Blueprint->GetName());
	if (!SaveError.IsEmpty())
	{
		Result += FString::Printf(TEXT(" WARNING: %s"), *SaveError);
	}
	return Result;
}

FString UConvaiSetupToolset::AddNavMeshVolumeForCurrentLevel(TOptional<FVector> Location, TOptional<FVector> Extent)
{
	if (!GEditor)
	{
		return TEXT("Error: GEditor is unavailable.");
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("Error: no current editor world.");
	}

	// Determine center + extent. If the caller didn't pass them, derive from the
	// combined bounds of all level actors.
	FVector Center = Location.Get(FVector::ZeroVector);
	FVector BoxExtent = Extent.Get(FVector(2048.f, 2048.f, 2048.f));

	if (!Location.IsSet() || !Extent.IsSet())
	{
		FBox LevelBounds(ForceInit);
		bool bHasBounds = false;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsA(ANavMeshBoundsVolume::StaticClass()))
			{
				continue;
			}
			FVector Origin, ActorExtent;
			Actor->GetActorBounds(/*bOnlyCollidingComponents*/ false, Origin, ActorExtent);
			if (!ActorExtent.IsNearlyZero())
			{
				LevelBounds += FBox(Origin - ActorExtent, Origin + ActorExtent);
				bHasBounds = true;
			}
		}

		if (bHasBounds)
		{
			if (!Location.IsSet())
			{
				Center = LevelBounds.GetCenter();
			}
			if (!Extent.IsSet())
			{
				// Add a margin so the navmesh comfortably covers the playable area.
				BoxExtent = LevelBounds.GetExtent() * 1.25f;
			}
		}
	}

	// Generous minimum half-extent so navigation (and "follow") works even when the level bounds
	// could not be measured (e.g. World Partition actors not loaded during iteration). ~100m x 100m x 40m.
	if (!Extent.IsSet())
	{
		BoxExtent = BoxExtent.ComponentMax(FVector(5000.f, 5000.f, 2000.f));
	}

	FActorSpawnParameters SpawnParams;
	ANavMeshBoundsVolume* Volume = World->SpawnActor<ANavMeshBoundsVolume>(
		ANavMeshBoundsVolume::StaticClass(), FTransform(Center), SpawnParams);
	if (!Volume)
	{
		return TEXT("Error: failed to spawn NavMeshBoundsVolume.");
	}

	// Build REAL box brush geometry at the target size. A NavMeshBoundsVolume derives both its nav
	// bounds AND its editor wireframe from the brush; a bare SpawnActor leaves the brush unbuilt, so
	// just scaling the actor renders nothing (invisible boundaries) and gives unreliable coverage.
	// UCubeBuilder builds an exact box (full dimensions = 2 x half-extent), keeping scale at 1.
	UCubeBuilder* CubeBuilder = NewObject<UCubeBuilder>(Volume);
	CubeBuilder->X = BoxExtent.X * 2.0f;
	CubeBuilder->Y = BoxExtent.Y * 2.0f;
	CubeBuilder->Z = BoxExtent.Z * 2.0f;

	Volume->PreEditChange(nullptr);
	Volume->PolyFlags = 0;
	Volume->Brush = NewObject<UModel>(Volume, NAME_None, RF_Transactional);
	Volume->Brush->Initialize(Volume, true);
	Volume->Brush->Polys = NewObject<UPolys>(Volume->Brush, NAME_None, RF_Transactional);
	if (UBrushComponent* BrushComp = Volume->GetBrushComponent())
	{
		BrushComp->Brush = Volume->Brush;
	}
	Volume->BrushBuilder = DuplicateObject<UBrushBuilder>(CubeBuilder, Volume);
	CubeBuilder->Build(World, Volume);
	Volume->SetActorLocation(Center);
	Volume->PostEditChange();

	// Rebuild navigation so the volume takes effect immediately.
	if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
	{
		NavSys->OnNavigationBoundsUpdated(Volume);
		NavSys->Build();
	}

	return FString::Printf(TEXT("Spawned NavMeshBoundsVolume at (%s) with half-extent (%s) and rebuilt navigation."),
		*Center.ToCompactString(), *BoxExtent.ToCompactString());
}

FString UConvaiSetupToolset::SetBlueprintPropertyAndPropagate(const FString& BlueprintPath, const FString& ComponentName,
	const FString& PropertyName, const FString& ValueAsString)
{
	FString Error;
	UBlueprint* Blueprint = ConvaiToolsetCommon::LoadBlueprintByPath(BlueprintPath, Error);
	if (!Blueprint)
	{
		return FString::Printf(TEXT("Error: %s"), *Error);
	}

	const FName PropFName(*PropertyName);
	const bool bActorLevel = ComponentName.IsEmpty();

	// Resolve the template object whose property we edit (actor CDO, or a component template).
	UObject* Template = nullptr;
	USCS_Node* CompNode = nullptr;
	if (bActorLevel)
	{
		Template = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
		if (!Template)
		{
			return TEXT("Error: Blueprint has no generated-class default object (compile it first).");
		}
	}
	else
	{
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			return TEXT("Error: Blueprint has no Simple Construction Script (no editable components).");
		}
		// Match the SCS node by its Blueprint variable name (case-insensitive). Only components added
		// on THIS Blueprint are editable here (inherited components live on a parent Blueprint).
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				CompNode = Node;
				break;
			}
		}
		if (!CompNode || !CompNode->ComponentTemplate)
		{
			return FString::Printf(TEXT("Error: no component named '%s' found on '%s' (only components added on this Blueprint are editable)."),
				*ComponentName, *Blueprint->GetName());
		}
		Template = CompNode->ComponentTemplate;
	}

	FProperty* Property = Template->GetClass()->FindPropertyByName(PropFName);
	if (!Property)
	{
		return FString::Printf(TEXT("Error: %s (%s) has no property named '%s'."),
			bActorLevel ? TEXT("actor") : *ComponentName, *Template->GetClass()->GetName(), *PropertyName);
	}

	// Route through the general propagation helper: capture OLD value, run our mutate (import the new
	// value from text), then copy the NEW value onto every placed instance still holding the OLD value
	// (overrides preserved). For actor-level, the CDO is both template and archetype of placed actors;
	// for a component, the helper derives the actual component archetype from the SCS node.
	bool bParseOk = true;
	UObject* const TemplateForLambda = Template;
	auto Mutate = [&]()
	{
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(TemplateForLambda);
		const TCHAR* Parsed = Property->ImportText_Direct(*ValueAsString, ValuePtr, TemplateForLambda, PPF_None);
		bParseOk = (Parsed != nullptr);
	};

	const int32 Affected = bActorLevel
		? ConvaiToolsetCommon::SetTemplatePropertyAndPropagate(Template, Template, PropFName, Mutate)
		: ConvaiToolsetCommon::SetTemplatePropertyAndPropagate(Blueprint, CompNode, PropFName, Mutate);

	if (!bParseOk)
	{
		return FString::Printf(TEXT("Error: could not parse value '%s' for property '%s' (%s)."),
			*ValueAsString, *PropertyName, *Property->GetClass()->GetName());
	}

	const FString TargetLabel = bActorLevel
		? FString::Printf(TEXT("actor property '%s'"), *PropertyName)
		: FString::Printf(TEXT("'%s.%s'"), *ComponentName, *PropertyName);

	if (!ConvaiToolsetCommon::CompileAndSaveBlueprint(Blueprint, /*bStructural=*/false, Error))
	{
		return FString::Printf(TEXT("Set %s = '%s' and propagated to %d placed instance(s), but save failed: %s"),
			*TargetLabel, *ValueAsString, Affected, *Error);
	}

	return FString::Printf(TEXT("Set %s = '%s' on '%s' and propagated to %d already-placed instance(s) (instances that had overridden the value were left unchanged)."),
		*TargetLabel, *ValueAsString, *Blueprint->GetName(), Affected);
}

#undef LOCTEXT_NAMESPACE
