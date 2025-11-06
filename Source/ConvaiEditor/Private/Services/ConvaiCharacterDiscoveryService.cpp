/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiCharacterDiscoveryService.cpp
 *
 * Implementation of character discovery service for finding Convai characters in levels.
 */

#include "Services/ConvaiCharacterDiscoveryService.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "EngineUtils.h"            // For TActorIterator
#include "ConvaiChatbotComponent.h" // Correct header for Convai component
#include "ConvaiEditor.h"

void FConvaiCharacterDiscoveryService::Startup()
{
}

void FConvaiCharacterDiscoveryService::Shutdown()
{
}

void FConvaiCharacterDiscoveryService::GetAllConvaiCharacterIDsInLevel(UWorld *World, TArray<FString> &OutCharacterIDs)
{
    OutCharacterIDs.Empty();
    if (!World)
    {
        return;
    }
    int32 ActorCount = 0;
    int32 ComponentCount = 0;
    for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
    {
        ++ActorCount;
        AActor *Actor = *ActorItr;
        if (!Actor)
        {
            continue;
        }

        UConvaiChatbotComponent *ChatbotComponent = Actor->FindComponentByClass<UConvaiChatbotComponent>();
        if (ChatbotComponent)
        {
            ++ComponentCount;
            const FString &CharacterID = ChatbotComponent->CharacterID;
            if (!CharacterID.IsEmpty())
            {
                OutCharacterIDs.Add(CharacterID);
            }
        }
    }
}