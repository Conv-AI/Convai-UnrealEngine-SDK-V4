/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiCharacterDiscoveryService.h
 *
 * Service for discovering Convai characters in levels.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"

/**
 * Interface for discovering Convai characters in levels.
 */
class CONVAIEDITOR_API IConvaiCharacterDiscoveryService : public IConvaiService
{
public:
    virtual ~IConvaiCharacterDiscoveryService() = default;

    /** Finds all Convai character IDs in world */
    virtual void GetAllConvaiCharacterIDsInLevel(UWorld *World, TArray<FString> &OutCharacterIDs) = 0;

    static FName StaticType() { return TEXT("IConvaiCharacterDiscoveryService"); }
};

/**
 * Discovers Convai characters in levels.
 */
class CONVAIEDITOR_API FConvaiCharacterDiscoveryService : public IConvaiCharacterDiscoveryService
{
public:
    FConvaiCharacterDiscoveryService() = default;
    virtual ~FConvaiCharacterDiscoveryService() override = default;

    virtual void Startup() override;
    virtual void Shutdown() override;

    virtual void GetAllConvaiCharacterIDsInLevel(UWorld *World, TArray<FString> &OutCharacterIDs) override;
};