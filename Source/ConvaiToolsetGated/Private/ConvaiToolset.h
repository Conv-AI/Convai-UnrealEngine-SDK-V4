// Copyright Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// This header declares the AI-callable toolset reflected types (USTRUCT/UCLASS/
// UFUNCTION). It is BUILD-gated, not preprocessor-gated: ConvaiToolset.Build.cs drops
// a ".ubtignore" into this folder (Private/Gated) on engines older than 5.8, so on
// 5.0-5.7 UnrealHeaderTool never parses this file and the module links as empty.
// We therefore include the 5.8-only ToolsetRegistry base unconditionally here -- this
// file is only ever compiled on 5.8+. The reflected declarations must stay OUTSIDE any
// "#if CONVAI_TOOLSET_SUPPORTED" block: UHT rejects reflection macros inside any
// preprocessor block except WITH_EDITORONLY_DATA (enforced on 5.0 through 5.8 alike).
#include "ConvaiDefinitions.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "ConvaiToolset.generated.h"


/**
 * One declared parameter for an AddConvaiAction call. Mirrors the agent-facing
 * subset of FConvaiActionParam (Name + Description + Type). We expose our own
 * USTRUCT rather than FConvaiActionParam directly so the AI-callable contract
 * stays small and stable (no Choices / EnumType / Connector noise), while the
 * implementation maps it onto a full FConvaiActionParam.
 */
USTRUCT()
struct FConvaiToolsetActionParam
{
	GENERATED_BODY()

	/** Placeholder name as the LLM sees it, e.g. "text", "destination", "time in seconds". */
	UPROPERTY()
	FString Name;

	/** Optional human-language description of what the parameter means. May be empty. */
	UPROPERTY()
	FString Description;

	/**
	 * How the value is interpreted. Use String for free text, Number for numerics,
	 * Reference to resolve against the chatbot's known Objects/Characters, Bool for
	 * true/false, Enum for a fixed choice set, or Auto (default) to infer at parse time.
	 */
	UPROPERTY()
	EConvaiActionParamType Type = EConvaiActionParamType::Auto;

	/**
	 * For a FIXED choice-set parameter, list the allowed values here and use Type=String (this mirrors
	 * the built-in "Move" action's `direction` param: String + choices). AddConvaiAction cannot supply
	 * a UEnum, so a Type=Enum param is coerced to String — put the options in Choices so the LLM sees
	 * them as "{name [a|b|c]: string}". Leave empty for free-form params.
	 */
	UPROPERTY()
	TArray<FString> Choices;
};

/**
 * Convai setup toolset: high-level, idempotent editor operations that wire a
 * MetaHuman / Pawn Blueprint and the current level up for Convai conversation
 * and navigation. These replicate the plugin's Content-Browser / context-menu
 * setup flows so an AI agent can perform them programmatically instead of
 * hand-editing Blueprint graphs.
 *
 * All Blueprint paths are package object paths, e.g. "/Game/MyChars/BP_Npc".
 */
UCLASS(MinimalAPI)
class UConvaiSetupToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/*
	 * Sets up Convai-tuned movement on a character/pawn Blueprint so it can navigate.
	 *
	 * If the Blueprint's parent is a plain AActor it is reparented to APawn. A
	 * UFloatingPawnMovement component is added if one isn't present (or inherited),
	 * and Convai movement defaults are applied (MaxSpeed=375, Acceleration=200,
	 * Deceleration=250, TurningBoost=3, plus nav-path-following defaults). For
	 * Character-based Blueprints the inherited CharacterMovementComponent is tuned
	 * with the equivalent walk-speed / nav-agent defaults instead. The Blueprint is
	 * compiled (when safe) and saved. Idempotent.
	 *
	 * @param CharacterBlueprintPath Object path of the character/pawn Blueprint, e.g. "/Game/NPCs/BP_Guard".
	 * @return A human-readable status string describing what was changed.
	 */
	UFUNCTION(meta = (AICallable))
	static FString SetupConvaiPawnMovement(const FString& CharacterBlueprintPath);

	/*
	 * Turns a character Blueprint into a talking Convai character.
	 *
	 * Adds BP_ConvaiChatbotComponent (the convenience chatbot component) and a native
	 * ConvaiFaceSyncComponent if missing; sets the chatbot's CharacterID and enables
	 * bAutoFillConversationPartnerFromPlayer; sets the face-sync LipSyncMode to the
	 * MetaHuman blendshape mode (BS_MHA) with interpolation enabled; and assigns the
	 * Convai MetaHuman face/body anim Blueprints to the skeletal mesh components named
	 * "Face" and "Body". Compiles and saves. Idempotent (re-uses existing components).
	 *
	 * @param CharacterBlueprintPath Object path of the MetaHuman/character Blueprint.
	 * @param CharacterId The Convai backend Character ID to assign to the chatbot component.
	 * @return A human-readable status string describing what was changed.
	 */
	UFUNCTION(meta = (AICallable))
	static FString SetupConvaiCharacter(const FString& CharacterBlueprintPath, const FString& CharacterId);

	/*
	 * Adds the Convai player component to the player's PAWN Blueprint (the controllable character the player possesses) so the
	 * player can speak to Convai characters. Adds BP_ConvaiPlayerComponent if missing,
	 * then compiles and saves. Idempotent.
	 *
	 * @param PlayerBlueprintPath Object path of the player PAWN Blueprint (the possessed/controllable character that owns the camera + input), e.g. "/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter".
	 * @return A human-readable status string describing what was changed.
	 */
	UFUNCTION(meta = (AICallable))
	static FString SetupConvaiPlayer(const FString& PlayerBlueprintPath);

	/*
	 * Spawns a NavMeshBoundsVolume into the current editor level so that Convai
	 * characters can path-find. By default the volume is centered and sized to cover
	 * the combined bounds of the level's actors; pass an explicit Location and Extent
	 * to override. After spawning, navigation is rebuilt.
	 *
	 * @param Location Optional world-space center for the volume. If unset, the level bounds center is used.
	 * @param Extent Optional half-extent (cm) of the volume box. If unset, derived from the level bounds (with a margin).
	 * @return A human-readable status string including the final volume location and extent.
	 */
	UFUNCTION(meta = (AICallable))
	static FString AddNavMeshVolumeForCurrentLevel(TOptional<FVector> Location, TOptional<FVector> Extent);

	/*
	 * Sets ONE property on a Blueprint AND propagates the new value to every already-placed level
	 * instance that still holds the old value (per-instance overrides are preserved). Handles BOTH:
	 *   - a COMPONENT-template property: pass the component's variable name in ComponentName;
	 *   - an ACTOR / Blueprint-level property (a BP variable / CDO property): leave ComponentName EMPTY.
	 *
	 * Use this INSTEAD of a raw property set + compile when the change must also reach characters
	 * ALREADY placed in the level. Verified: neither ObjectTools.set_properties nor compile_blueprint
	 * propagate template/CDO edits to already-placed instances (they only affect the template + NEW
	 * instances) — for components OR actor-level properties.
	 *
	 * The value is parsed via the property's own text importer, so any type works: "300.0" (float),
	 * "true" (bool), "Hello" (string/name), "(X=0,Y=0,Z=300)" (vector/struct), "/Game/P.A" (object).
	 * Compiles and saves the Blueprint. NOTE: edit through THIS tool from the start — a value already
	 * desynced by a prior raw set looks like a per-instance override and won't be propagated.
	 *
	 * @param BlueprintPath Object path of the Blueprint, e.g. "/Game/NPCs/BP_Guard".
	 * @param ComponentName The component's Blueprint variable name (e.g. "ProximitySphere"); leave EMPTY for an actor/BP-level property.
	 * @param PropertyName  The property name, e.g. "SphereRadius" (component) or "MyVar" (actor-level).
	 * @param ValueAsString The new value in Unreal text form (parsed by the property importer).
	 * @return A human-readable status string including how many placed instances were updated.
	 */
	UFUNCTION(meta = (AICallable))
	static FString SetBlueprintPropertyAndPropagate(const FString& BlueprintPath, const FString& ComponentName,
		const FString& PropertyName, const FString& ValueAsString);
};

/**
 * Convai action toolset: declares actions on a chatbot and synthesizes the
 * Blueprint event handlers that respond to them. Together these let an agent
 * stand up the Convai "action" affordance loop without touching the graph by hand.
 */
UCLASS(MinimalAPI)
class UConvaiActionToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/*
	 * Appends a parameterized action to a chatbot's Environment action list.
	 *
	 * Locates the BP_ConvaiChatbotComponent (or any UConvaiChatbotComponent) on the
	 * given Blueprint, enables EnvironmentData.bEnableActions, and adds an
	 * FConvaiAction named ActionName (with the given Description and parameters),
	 * preserving any existing actions. A same-named action is replaced in place.
	 * The Blueprint is compiled and saved.
	 *
	 * Note: these actions take effect at the next chatbot /connect — they prepare the
	 * NEXT session, they don't mutate a live one.
	 *
	 * @param CharacterBlueprintPath Object path of the Blueprint that owns the chatbot component.
	 * @param ActionName Canonical action name, e.g. "Wave", "Pick Up", "Say". Should match the handler event name.
	 * @param Description Optional human-language description of what the action does.
	 * @param Parameters Ordered list of declared parameters (name + type) the LLM should fill in.
	 * @return A human-readable status string describing what was changed.
	 */
	UFUNCTION(meta = (AICallable))
	static FString AddConvaiAction(const FString& CharacterBlueprintPath, const FString& ActionName,
		const FString& Description, const TArray<FConvaiToolsetActionParam>& Parameters);

	/*
	 * Synthesizes a Convai action handler in a Blueprint's event graph.
	 *
	 * Creates a Custom Event named EXACTLY ActionName with a single input pin of type
	 * FConvaiResultAction, and wires its exec output to a HandleActionCompletion call
	 * on the Blueprint's chatbot component (auto-wiring the chatbot self pin from the
	 * component variable when present). This is the runtime contract that
	 * UConvaiChatbotComponent::TriggerNamedBlueprintAction expects. The Blueprint is
	 * compiled. Replicates the core of ConvaiCreateActionHandlerSpawner WITHOUT any
	 * modal dialog.
	 *
	 * @param BlueprintPath Object path of the Blueprint to add the handler to.
	 * @param ActionName The exact action name; the created Custom Event is named the same.
	 * @return The created event name on success, or an error string prefixed with "Error:".
	 */
	UFUNCTION(meta = (AICallable))
	static FString CreateConvaiActionHandler(const FString& BlueprintPath, const FString& ActionName);
};
