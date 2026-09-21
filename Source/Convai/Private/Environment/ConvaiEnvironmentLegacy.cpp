// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Environment/ConvaiEnvironmentLegacy.h"
#include "ConvaiChatbotComponent.h"

// All methods delegate to the back-pointer chatbot. If the shim was created via the
// legacy CreateConvaiEnvironment factory (no owning chatbot), the calls are no-ops.

void UConvaiEnvironment::AddAction(FString Action)
{
	// Legacy callers passed bare action strings. Wrap into an FConvaiAction with no
	// description / no parameters so the new structured pipeline can process it.
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->AddActionByName(Action); }
}

void UConvaiEnvironment::AddActions(TArray<FString> ActionsToAdd)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get())
	{
		for (const FString& A : ActionsToAdd) { C->AddActionByName(A); }
	}
}

void UConvaiEnvironment::RemoveAction(FString Action)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->RemoveAction(Action); }
}

void UConvaiEnvironment::RemoveActions(TArray<FString> ActionsToRemove)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->RemoveActions(ActionsToRemove); }
}

void UConvaiEnvironment::ClearAllActions()
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->ClearActions(); }
}

void UConvaiEnvironment::AddObject(FConvaiObjectEntry Object)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->AddObject(Object); }
}

void UConvaiEnvironment::AddObjects(TArray<FConvaiObjectEntry> ObjectsToAdd)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->AddObjects(ObjectsToAdd); }
}

void UConvaiEnvironment::RemoveObject(FString ObjectName)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->RemoveObject(ObjectName); }
}

void UConvaiEnvironment::RemoveObjects(TArray<FString> ObjectNamesToRemove)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->RemoveObjects(ObjectNamesToRemove); }
}

void UConvaiEnvironment::ClearObjects()
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->ClearObjects(); }
}

void UConvaiEnvironment::AddCharacter(FConvaiObjectEntry Character)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->AddCharacter(Character); }
}

void UConvaiEnvironment::AddCharacters(TArray<FConvaiObjectEntry> CharactersToAdd)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->AddCharacters(CharactersToAdd); }
}

void UConvaiEnvironment::RemoveCharacter(FString InCharacterName)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->RemoveCharacter(InCharacterName); }
}

void UConvaiEnvironment::RemoveCharacters(TArray<FString> InCharacterNames)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->RemoveCharacters(InCharacterNames); }
}

void UConvaiEnvironment::ClearCharacters()
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get()) { C->ClearCharacters(); }
}

void UConvaiEnvironment::SetMainCharacter(FConvaiObjectEntry InMainCharacter)
{
	// Legacy graphs that used MainCharacter for gaze keep working. Conversation-partner
	// uses must migrate to SetConversationPartner explicitly — this shim does not touch it.
	if (UConvaiChatbotComponent* C = OwningChatbot.Get())
	{
		C->LookAtTarget = InMainCharacter.Ref.Get();
	}
}

void UConvaiEnvironment::ClearMainCharacter()
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get())
	{
		C->LookAtTarget = nullptr;
	}
}

FConvaiObjectEntry UConvaiEnvironment::GetMainCharacter() const
{
	FConvaiObjectEntry Entry;
	if (const UConvaiChatbotComponent* C = OwningChatbot.Get())
	{
		if (AActor* Target = C->LookAtTarget.Get())
		{
			Entry.Ref = Target;
			Entry.Name = Target->GetName();
		}
	}
	return Entry;
}

FConvaiObjectEntry UConvaiEnvironment::GetAttentionObject() const
{
	if (const UConvaiChatbotComponent* C = OwningChatbot.Get())
	{
		return C->EnvironmentData.CurrentAttentionObject;
	}
	return FConvaiObjectEntry();
}

TArray<FString> UConvaiEnvironment::GetActions() const
{
	// Legacy shape was a flat array of action names. Project the structured Actions list
	// down to its names so old BP graphs that read Environment.Actions still see strings.
	TArray<FString> Out;
	if (const UConvaiChatbotComponent* C = OwningChatbot.Get())
	{
		Out.Reserve(C->EnvironmentData.Actions.Num());
		for (const FConvaiAction& A : C->EnvironmentData.Actions)
		{
			if (A.bEnabled && !A.Name.IsEmpty())
			{
				Out.Add(A.Name);
			}
		}
	}
	return Out;
}

TArray<FConvaiObjectEntry> UConvaiEnvironment::GetObjects() const
{
	if (const UConvaiChatbotComponent* C = OwningChatbot.Get())
	{
		return C->EnvironmentData.Objects;
	}
	return TArray<FConvaiObjectEntry>();
}

TArray<FConvaiObjectEntry> UConvaiEnvironment::GetCharacters() const
{
	if (const UConvaiChatbotComponent* C = OwningChatbot.Get())
	{
		return C->EnvironmentData.Characters;
	}
	return TArray<FConvaiObjectEntry>();
}

void UConvaiEnvironment::SetAttentionObject(FConvaiObjectEntry InAttentionObject)
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get())
	{
		C->SetObjectInAttention(InAttentionObject);
	}
}

void UConvaiEnvironment::ClearAttentionObject()
{
	if (UConvaiChatbotComponent* C = OwningChatbot.Get())
	{
		C->SetObjectInAttention(FConvaiObjectEntry());
	}
}
