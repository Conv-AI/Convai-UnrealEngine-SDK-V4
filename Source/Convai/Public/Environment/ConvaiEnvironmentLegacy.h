// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ConvaiDefinitions.h"
#include "ConvaiEnvironmentLegacy.generated.h"

class UConvaiChatbotComponent;

/**
 * DEPRECATED — legacy migration shim for the pre-refactor `UConvaiEnvironment` UObject.
 *
 * The old environment object held Actions, Objects, Characters, MainCharacter, and
 * AttentionObject and exposed Add/Remove/Clear/Set BP methods. That whole surface is
 * now split across the chatbot:
 *
 *   - Add/Remove/Clear Object        → UConvaiChatbotComponent::AddObject / RemoveObject / ClearObjects
 *   - Add/Remove/Clear Character     → UConvaiChatbotComponent::AddCharacter / RemoveCharacter / ClearCharacters
 *   - Add/Remove/Clear Action        → UConvaiChatbotComponent::AddAction / RemoveAction / ClearActions
 *   - SetAttentionObject             → UConvaiChatbotComponent::SetObjectInAttention
 *   - SetMainCharacter (gaze)        → UConvaiChatbotComponent::LookAtTarget
 *   - SetMainCharacter (partner)     → UConvaiChatbotComponent::SetConversationPartner
 *
 * The class name is preserved so existing BP asset references resolve. Each method
 * delegates to OwningChatbot and emits a deprecation warning pointing at the
 * replacement API.
 */
UCLASS(BlueprintType)
class CONVAI_API UConvaiEnvironment : public UObject
{
	GENERATED_BODY()

public:
	/** Back-pointer set by UConvaiChatbotComponent::GetEnvironment(). */
	UPROPERTY(Transient)
	TWeakObjectPtr<UConvaiChatbotComponent> OwningChatbot;

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction,
				DeprecationMessage = "Create Convai Environment is no longer needed. The chatbot now owns its environment as a USTRUCT field; mutate via the granular API."))
	static UConvaiEnvironment* CreateConvaiEnvironment()
	{
		return NewObject<UConvaiEnvironment>();
	}

	// ── Actions ──────────────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::AddAction."))
	void AddAction(FString Action);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::AddActions."))
	void AddActions(TArray<FString> ActionsToAdd);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::RemoveAction."))
	void RemoveAction(FString Action);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::RemoveActions."))
	void RemoveActions(TArray<FString> ActionsToRemove);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::ClearActions."))
	void ClearAllActions();

	// ── Objects ──────────────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::AddObject."))
	void AddObject(FConvaiObjectEntry Object);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::AddObjects."))
	void AddObjects(TArray<FConvaiObjectEntry> ObjectsToAdd);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::RemoveObject."))
	void RemoveObject(FString ObjectName);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::RemoveObjects."))
	void RemoveObjects(TArray<FString> ObjectNamesToRemove);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::ClearObjects."))
	void ClearObjects();

	// ── Characters ───────────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::AddCharacter."))
	void AddCharacter(FConvaiObjectEntry Character);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::AddCharacters."))
	void AddCharacters(TArray<FConvaiObjectEntry> CharactersToAdd);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::RemoveCharacter."))
	void RemoveCharacter(FString InCharacterName);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::RemoveCharacters."))
	void RemoveCharacters(TArray<FString> InCharacterNames);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use UConvaiChatbotComponent::ClearCharacters."))
	void ClearCharacters();

	// ── Main Character (split into LookAtTarget + ConversationPartner) ──

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction,
				DeprecationMessage = "MainCharacter has been split. Use UConvaiChatbotComponent::SetConversationPartner for who the bot is talking to, or assign LookAtTarget directly for animation gaze. This shim writes to LookAtTarget only."))
	void SetMainCharacter(FConvaiObjectEntry InMainCharacter);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction,
				DeprecationMessage = "Set UConvaiChatbotComponent::LookAtTarget to nullptr instead."))
	void ClearMainCharacter();

	// ── Legacy read-only UPROPERTYs (computed via BlueprintGetter) ────
	// These let old BP graphs that did `Environment.MainCharacter`, `Environment.Actions`,
	// etc. as direct property reads keep working. Each getter pulls from OwningChatbot's
	// new state — no separate storage.

	UPROPERTY(BlueprintReadOnly, BlueprintGetter = GetMainCharacter,
		Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedProperty, DeprecationMessage = "MainCharacter has been split. Use UConvaiChatbotComponent::ConversationPartner for who the bot is talking to, or LookAtTarget for animation gaze. This shim reflects LookAtTarget."))
	FConvaiObjectEntry MainCharacter;

	UFUNCTION(BlueprintGetter)
	FConvaiObjectEntry GetMainCharacter() const;

	UPROPERTY(BlueprintReadOnly, BlueprintGetter = GetAttentionObject,
		Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedProperty, DeprecationMessage = "Use UConvaiChatbotComponent::SetObjectInAttention. This shim reflects EnvironmentData.CurrentAttentionObject."))
	FConvaiObjectEntry AttentionObject;

	UFUNCTION(BlueprintGetter)
	FConvaiObjectEntry GetAttentionObject() const;

	UPROPERTY(BlueprintReadOnly, BlueprintGetter = GetActions,
		Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedProperty, DeprecationMessage = "Mutate via UConvaiChatbotComponent::AddAction / RemoveAction. This shim reflects EnvironmentData.Actions."))
	TArray<FString> Actions;

	UFUNCTION(BlueprintGetter)
	TArray<FString> GetActions() const;

	UPROPERTY(BlueprintReadOnly, BlueprintGetter = GetObjects,
		Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedProperty, DeprecationMessage = "Mutate via UConvaiChatbotComponent::AddObject / RemoveObject. This shim reflects EnvironmentData.Objects."))
	TArray<FConvaiObjectEntry> Objects;

	UFUNCTION(BlueprintGetter)
	TArray<FConvaiObjectEntry> GetObjects() const;

	UPROPERTY(BlueprintReadOnly, BlueprintGetter = GetCharacters,
		Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedProperty, DeprecationMessage = "Mutate via UConvaiChatbotComponent::AddCharacter / RemoveCharacter. This shim reflects EnvironmentData.Characters."))
	TArray<FConvaiObjectEntry> Characters;

	UFUNCTION(BlueprintGetter)
	TArray<FConvaiObjectEntry> GetCharacters() const;

	// ── Attention Object setters ─────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction,
				DeprecationMessage = "Use UConvaiChatbotComponent::SetObjectInAttention."))
	void SetAttentionObject(FConvaiObjectEntry InAttentionObject);

	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction,
				DeprecationMessage = "Call UConvaiChatbotComponent::SetObjectInAttention with a default-constructed FConvaiObjectEntry to clear."))
	void ClearAttentionObject();
};

/**
 * DEPRECATED — kept so BP graphs that referenced UConvaiActionContext still resolve.
 */
UCLASS(Blueprintable)
class CONVAI_API UConvaiActionContext : public UConvaiEnvironment
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Convai|Action API|DEPRECATED",
		meta = (DeprecatedFunction, DeprecationMessage = "Use the chatbot's granular API directly."))
	static UConvaiActionContext* CreateConvaiActionContext()
	{
		return NewObject<UConvaiActionContext>();
	}
};
