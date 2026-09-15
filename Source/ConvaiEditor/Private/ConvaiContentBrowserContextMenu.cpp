/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiContentBrowserContextMenu.cpp
 *
 * Implementation of Content Browser context menu extensions for Convai tools.
 */

#include "ConvaiContentBrowserContextMenu.h"
#include "ToolMenus.h"
#include "ContentBrowserDataMenuContexts.h"
#include "ContentBrowserMenuContexts.h"
#include "Engine/TextureRenderTarget2D.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/InheritableComponentHandler.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/NavMovementComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Misc/MessageDialog.h"

#define LOCTEXT_NAMESPACE "FConvaiContentBrowserContextMenu"

FDelegateHandle FConvaiContentBrowserContextMenu::MenuExtensionHandle;
FString FConvaiContentBrowserContextMenu::CurrentPackagePath;

namespace
{
	/**
	 * Notify the editor / archetype machinery that a specific UPROPERTY on this object changed.
	 * Without a property-scoped event the CDO archetype-delta tracking can miss struct edits,
	 * which is what produced the "NavMovementProperties was not set" symptom.
	 */
	void NotifyPropertyChanged(UObject *Object, FName PropertyName)
	{
		if (!Object || PropertyName.IsNone())
		{
			return;
		}

		FProperty *Property = Object->GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			return;
		}

		Object->PreEditChange(Property);
		FPropertyChangedEvent Event(Property);
		Object->PostEditChangeProperty(Event);
	}

	/**
	 * Apply Convai's nav-movement defaults (acceleration-driven path following etc.) to any
	 * UNavMovementComponent subclass — both UCharacterMovementComponent and UFloatingPawnMovement.
	 *
	 * In 5.5+ these moved into FNavMovementProperties (exposed via GetNavMovementProperties()).
	 * On 5.0–5.4 the same fields exist on UNavMovementComponent but are protected, so we reach
	 * them via UPROPERTY reflection (which bypasses C++ access modifiers). FindFProperty returns
	 * null for fields that don't exist in a given engine version, so missing fields (e.g.
	 * bStopMovementAbortPaths pre-5.5) safely no-op.
	 */
	void ApplyNavMovementDefaults(UNavMovementComponent *Comp)
	{
		if (!Comp)
		{
			return;
		}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		if (FNavMovementProperties *NavProps = Comp->GetNavMovementProperties())
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
			if (FBoolProperty *Prop = FindFProperty<FBoolProperty>(Comp->GetClass(), PropertyName))
			{
				Prop->SetPropertyValue_InContainer(Comp, bValue);
			}
		};
		auto SetReflectedFloat = [Comp](FName PropertyName, float Value)
		{
			if (FFloatProperty *Prop = FindFProperty<FFloatProperty>(Comp->GetClass(), PropertyName))
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
}

void FConvaiContentBrowserContextMenu::Register()
{
	UToolMenus *ToolMenus = UToolMenus::Get();
	if (!ToolMenus)
	{
		return;
	}

	UToolMenu *AddNewMenu = ToolMenus->ExtendMenu("ContentBrowser.AddNewContextMenu");
	if (AddNewMenu)
	{
		FToolMenuSection *GetContentSection = AddNewMenu->FindSection("ContentBrowserGetContent");
		if (!GetContentSection)
		{
			GetContentSection = &AddNewMenu->AddSection("ContentBrowserGetContent", LOCTEXT("GetContentMenuHeading", "Get Content"));
		}

		GetContentSection->AddDynamicEntry("ConvaiContent", FNewToolMenuSectionDelegate::CreateStatic(&FConvaiContentBrowserContextMenu::PopulateContextMenu));
	}

	RegisterBlueprintAssetMenu();
}

void FConvaiContentBrowserContextMenu::Unregister()
{
	UToolMenus *ToolMenus = UToolMenus::Get();
	if (ToolMenus)
	{
		ToolMenus->RemoveEntry("ContentBrowser.AddNewContextMenu", "ContentBrowserGetContent", "ConvaiContent");
	}

	UnregisterBlueprintAssetMenu();
}

void FConvaiContentBrowserContextMenu::PopulateContextMenu(FToolMenuSection &InSection)
{
	UContentBrowserDataMenuContext_AddNewMenu *AddNewMenuContext = InSection.FindContext<UContentBrowserDataMenuContext_AddNewMenu>();
	if (AddNewMenuContext && AddNewMenuContext->bCanBeModified && AddNewMenuContext->bContainsValidPackagePath)
	{
		if (AddNewMenuContext->SelectedPaths.Num() > 0)
		{
			CurrentPackagePath = AddNewMenuContext->SelectedPaths[0].ToString();
		}

		InSection.AddSubMenu(
			"ConvaiSubMenu",
			LOCTEXT("ConvaiSubMenuLabel", "Convai"),
			LOCTEXT("ConvaiSubMenuTooltip", "Convai tools and options"),
			FNewToolMenuDelegate::CreateStatic(&FConvaiContentBrowserContextMenu::MakeConvaiSubMenu),
			FUIAction(),
			EUserInterfaceActionType::Button,
			false,
			FSlateIcon());
	}
}

void FConvaiContentBrowserContextMenu::MakeConvaiSubMenu(UToolMenu *Menu)
{
	if (!Menu)
	{
		return;
	}

	FToolMenuSection &ConvaiSection = Menu->AddSection("ConvaiActions", LOCTEXT("ConvaiActionsHeading", "Convai Actions"));

	ConvaiSection.AddMenuEntry(
		"ConvaiButton",
		LOCTEXT("ConvaiButtonLabel", "Vision  Render Target"),
		LOCTEXT("ConvaiButtonTooltip", "Vision  Render Target (placeholder)"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&FConvaiContentBrowserContextMenu::ExecuteConvaiAction)));
}

void FConvaiContentBrowserContextMenu::ExecuteConvaiAction()
{
	CreateAndSaveRenderTarget(CurrentPackagePath);
}

void FConvaiContentBrowserContextMenu::CreateAndSaveRenderTarget(const FString &PackagePath)
{
	if (PackagePath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("ConvaiContentBrowserContextMenu: Package path is empty"));
		return;
	}

	FString CleanPath = PackagePath;
	if (CleanPath.StartsWith(TEXT("/All/")))
	{
		CleanPath = CleanPath.Mid(4);
	}

	if (!CleanPath.EndsWith(TEXT("/")))
	{
		CleanPath += TEXT("/");
	}

	FString AssetName = TEXT("VisionRenderTarget");
	FString FullPackagePath = CleanPath + AssetName;

	FString PackagePathOnly = FPackageName::GetLongPackagePath(*FullPackagePath);
	FString AssetNameOnly = FPackageName::GetShortName(*FullPackagePath);

	UTextureRenderTarget2D *NewRenderTarget = NewObject<UTextureRenderTarget2D>(
		CreatePackage(*FullPackagePath),
		*AssetNameOnly,
		RF_Public | RF_Standalone | RF_Transactional);

	if (!NewRenderTarget)
	{
		UE_LOG(LogTemp, Error, TEXT("ConvaiContentBrowserContextMenu: Failed to create render target"));
		return;
	}

	NewRenderTarget->ResizeTarget(512, 512);
	NewRenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
	NewRenderTarget->ClearColor = FLinearColor::Black;
	NewRenderTarget->UpdateResourceImmediate(true);

	NewRenderTarget->MarkPackageDirty();

	UPackage *Package = NewRenderTarget->GetOutermost();
	if (Package)
	{
		FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		if (UPackage::SavePackage(Package, NewRenderTarget, *PackageFileName, SaveArgs))
		{
			UE_LOG(LogTemp, Log, TEXT("ConvaiContentBrowserContextMenu: Successfully created and saved render target at %s"), *FullPackagePath);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("ConvaiContentBrowserContextMenu: Failed to save render target package"));
		}
	}
}

void FConvaiContentBrowserContextMenu::RegisterBlueprintAssetMenu()
{
	UToolMenus *ToolMenus = UToolMenus::Get();
	if (!ToolMenus)
	{
		return;
	}

	UToolMenu *BPAssetMenu = ToolMenus->ExtendMenu("ContentBrowser.AssetContextMenu.Blueprint");
	if (!BPAssetMenu)
	{
		return;
	}

	FToolMenuSection &Section = BPAssetMenu->FindOrAddSection("ConvaiActorActions");
	Section.AddDynamicEntry(
		"ConvaiSetupPawnMovement",
		FNewToolMenuSectionDelegate::CreateStatic(&FConvaiContentBrowserContextMenu::PopulateBlueprintAssetMenu));
}

void FConvaiContentBrowserContextMenu::UnregisterBlueprintAssetMenu()
{
	UToolMenus *ToolMenus = UToolMenus::Get();
	if (!ToolMenus)
	{
		return;
	}

	ToolMenus->RemoveEntry("ContentBrowser.AssetContextMenu.Blueprint", "ConvaiActorActions", "ConvaiSetupPawnMovement");
}

bool FConvaiContentBrowserContextMenu::IsActorBlueprintAsset(const FAssetData &Asset)
{
	if (!Asset.IsValid())
	{
		return false;
	}

	FString NativeParentClassPath;
	if (!Asset.GetTagValue(FBlueprintTags::NativeParentClassPath, NativeParentClassPath) || NativeParentClassPath.IsEmpty())
	{
		// Fall back to ParentClass tag for non-native parents
		Asset.GetTagValue(FBlueprintTags::ParentClassPath, NativeParentClassPath);
	}

	if (NativeParentClassPath.IsEmpty())
	{
		return false;
	}

	const FString ClassObjectPath = FPackageName::ExportTextPathToObjectPath(NativeParentClassPath);

	UClass *ResolvedClass = nullptr;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	ResolvedClass = UClass::TryFindTypeSlow<UClass>(ClassObjectPath);
#endif
	if (!ResolvedClass)
	{
		ResolvedClass = FindObject<UClass>(nullptr, *ClassObjectPath);
	}

	return ResolvedClass && ResolvedClass->IsChildOf(AActor::StaticClass());
}

void FConvaiContentBrowserContextMenu::PopulateBlueprintAssetMenu(FToolMenuSection &InSection)
{
	UContentBrowserAssetContextMenuContext *Context = InSection.FindContext<UContentBrowserAssetContextMenuContext>();
	if (!Context)
	{
		return;
	}

	TArray<FAssetData> ActorBlueprints;
	// UContentBrowserAssetContextMenuContext::SelectedAssets (TArray<FAssetData>)
	// was introduced in UE 5.1. UE 5.0 only exposes SelectedObjects
	// (TArray<TWeakObjectPtr<UObject>>) — too cumbersome to translate for the
	// build-only 5.0 path, so this menu is skipped there.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (const FAssetData &Asset : Context->SelectedAssets)
	{
		if (IsActorBlueprintAsset(Asset))
		{
			ActorBlueprints.Add(Asset);
		}
	}
#endif

	if (ActorBlueprints.Num() == 0)
	{
		return;
	}

	InSection.AddSubMenu(
		"ConvaiAssetSubMenu",
		LOCTEXT("ConvaiAssetSubMenuLabel", "Convai"),
		LOCTEXT("ConvaiAssetSubMenuTooltip", "Convai actor tools"),
		FNewToolMenuDelegate::CreateLambda([ActorBlueprints](UToolMenu *Menu)
		{
			if (!Menu)
			{
				return;
			}

			FToolMenuSection &Sub = Menu->AddSection("ConvaiBPActions", LOCTEXT("ConvaiBPActionsHeading", "Convai Actions"));
			Sub.AddMenuEntry(
				"SetupConvaiPawnMovement",
				LOCTEXT("SetupConvaiPawnMovementLabel", "Setup Convai Pawn Movement"),
				LOCTEXT("SetupConvaiPawnMovementTooltip",
					"Reparent to Pawn (if pure Actor), add a FloatingPawnMovement component if missing, "
					"and apply Convai default movement properties to the Pawn or Character movement component."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([ActorBlueprints]()
				{
					FScopedTransaction Transaction(LOCTEXT("ConvaiSetupPawnMovementTx", "Setup Convai Pawn Movement"));

					for (const FAssetData &Asset : ActorBlueprints)
					{
						if (UBlueprint *Blueprint = Cast<UBlueprint>(Asset.GetAsset()))
						{
							FConvaiContentBrowserContextMenu::ApplyConvaiMovementToBlueprint(Blueprint);
						}
					}
				})));
		}));
}

UFloatingPawnMovement *FConvaiContentBrowserContextMenu::FindOrAddFloatingPawnMovement(UBlueprint *Blueprint, bool &bOutAddedNewNode, bool &bOutIsSCSOwned)
{
	bOutAddedNewNode = false;
	bOutIsSCSOwned = false;

	if (!Blueprint)
	{
		return nullptr;
	}

	// Case 1: FPM already declared in THIS Blueprint's SCS -> modify its template directly.
	if (USimpleConstructionScript *SCS = Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node *Node : SCS->GetAllNodes())
		{
			if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf(UFloatingPawnMovement::StaticClass()))
			{
				if (UFloatingPawnMovement *Existing = Cast<UFloatingPawnMovement>(Node->ComponentTemplate))
				{
					bOutIsSCSOwned = true;
					return Existing;
				}
			}
		}
	}

	// Case 2: FPM is inherited (parent BP's SCS, or native default subobject).
	// Modify on THIS BP's CDO instance so changes are saved as a per-class delta override
	// rather than mutating the ancestor's template. Caller must NOT recompile after this.
	if (UClass *ParentClass = Blueprint->ParentClass)
	{
		if (AActor *ParentCDO = Cast<AActor>(ParentClass->GetDefaultObject()))
		{
			if (ParentCDO->FindComponentByClass<UFloatingPawnMovement>())
			{
				if (UClass *GeneratedClass = Blueprint->GeneratedClass)
				{
					if (AActor *CDO = Cast<AActor>(GeneratedClass->GetDefaultObject()))
					{
						if (UFloatingPawnMovement *Inst = CDO->FindComponentByClass<UFloatingPawnMovement>())
						{
							return Inst;
						}
					}
				}
			}
		}
	}

	// Case 3: None found anywhere -> add a new SCS node on this Blueprint.
	USimpleConstructionScript *SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return nullptr;
	}

	SCS->Modify();
	USCS_Node *NewNode = SCS->CreateNode(UFloatingPawnMovement::StaticClass(), TEXT("FloatingPawnMovement"));
	if (!NewNode)
	{
		return nullptr;
	}
	SCS->AddNode(NewNode);

	bOutAddedNewNode = true;
	bOutIsSCSOwned = true;
	return Cast<UFloatingPawnMovement>(NewNode->ComponentTemplate);
}

void FConvaiContentBrowserContextMenu::ApplyFloatingPawnMovementDefaults(UFloatingPawnMovement *Comp)
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

	NotifyPropertyChanged(Comp, TEXT("MaxSpeed"));
	NotifyPropertyChanged(Comp, TEXT("Acceleration"));
	NotifyPropertyChanged(Comp, TEXT("Deceleration"));
	NotifyPropertyChanged(Comp, TEXT("TurningBoost"));
	NotifyPropertyChanged(Comp, TEXT("NavMovementProperties"));
}

UCharacterMovementComponent *FConvaiContentBrowserContextMenu::GetBlueprintCharacterMovement(UBlueprint *Blueprint)
{
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return nullptr;
	}

	if (AActor *CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject()))
	{
		return CDO->FindComponentByClass<UCharacterMovementComponent>();
	}

	return nullptr;
}

void FConvaiContentBrowserContextMenu::ApplyCharacterMovementDefaults(UCharacterMovementComponent *Comp)
{
	if (!Comp)
	{
		return;
	}

	Comp->Modify();

	// Speed / acceleration / deceleration analogues for the character's default walking mode.
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

	// Notify per top-level property so archetype-delta tracking captures the struct edits.
	NotifyPropertyChanged(Comp, TEXT("MaxWalkSpeed"));
	NotifyPropertyChanged(Comp, TEXT("MaxAcceleration"));
	NotifyPropertyChanged(Comp, TEXT("BrakingDecelerationWalking"));
	NotifyPropertyChanged(Comp, TEXT("NavMovementProperties"));
	NotifyPropertyChanged(Comp, TEXT("NavAgentProps"));
}

void FConvaiContentBrowserContextMenu::ApplyConvaiMovementToBlueprint(UBlueprint *Blueprint)
{
	if (!Blueprint || !Blueprint->ParentClass)
	{
		return;
	}

	UClass *ParentClass = Blueprint->ParentClass;
	if (!ParentClass->IsChildOf(AActor::StaticClass()))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ConvaiContentBrowserContextMenu: Skipping %s - parent %s is not an Actor."),
			*Blueprint->GetName(), *ParentClass->GetName());
		return;
	}

	const bool bIsCharacter = ParentClass->IsChildOf(ACharacter::StaticClass());
	const bool bIsPawn = ParentClass->IsChildOf(APawn::StaticClass());
	const bool bIsPureActor = (ParentClass == AActor::StaticClass());

	Blueprint->Modify();

	// Reparent + compile up front so subsequent SCS work happens on the final structure.
	bool bReparentedToPawn = false;
	if (bIsPureActor)
	{
		Blueprint->ParentClass = APawn::StaticClass();
		FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bReparentedToPawn = true;
	}

	const bool bUseFloatingMovement = bReparentedToPawn || (bIsPawn && !bIsCharacter);

	bool bNeedsStructuralCompile = false;
	bool bAnyDefaultsModified = false;

	if (bIsCharacter)
	{
		if (UCharacterMovementComponent *CharMove = GetBlueprintCharacterMovement(Blueprint))
		{
			ApplyCharacterMovementDefaults(CharMove);
			bAnyDefaultsModified = true;
		}
	}
	else if (bUseFloatingMovement)
	{
		bool bAddedNewNode = false;
		bool bIsSCSOwned = false;
		if (UFloatingPawnMovement *FPM = FindOrAddFloatingPawnMovement(Blueprint, bAddedNewNode, bIsSCSOwned))
		{
			ApplyFloatingPawnMovementDefaults(FPM);
			bAnyDefaultsModified = true;
			// Compile only when the changes were applied to compile-safe storage (an SCS template
			// owned by this BP). For inherited components we hold the change on the CDO and rely
			// on archetype delta serialization on the next save, so a recompile would discard it.
			if (bIsSCSOwned)
			{
				bNeedsStructuralCompile = true;
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ConvaiContentBrowserContextMenu: Could not set up Convai pawn movement on '%s'. "
				 "Parent class '%s' is an Actor subclass that does not eventually inherit from APawn or ACharacter, "
				 "so the parent class was left untouched (we don't reparent custom Actor hierarchies). "
				 "Please reparent the Blueprint to something that eventually inherits from APawn (recommended, "
				 "since ACharacter requires a capsule collision setup) and run this option again."),
			*Blueprint->GetName(), *ParentClass->GetName());
		return;
	}

	if (bAnyDefaultsModified)
	{
		if (bNeedsStructuralCompile)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}
	}

	Blueprint->MarkPackageDirty();

	// Persist immediately so the CDO archetype delta survives the next BP recompile.
	// Without this, opening the BP editor (which compiles on open) regenerates the CDO
	// from the parent class and discards inherited-component overrides we just applied.
	if (UPackage *Package = Blueprint->GetOutermost())
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Blueprint, *FileName, SaveArgs))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("ConvaiContentBrowserContextMenu: Movement setup applied to '%s' but the package save failed. "
					 "Please save the asset manually so the changes persist past the next compile."),
				*Blueprint->GetName());
		}
	}
}

#undef LOCTEXT_NAMESPACE
