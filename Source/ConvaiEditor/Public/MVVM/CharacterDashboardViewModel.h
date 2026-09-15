/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * CharacterDashboardViewModel.h
 *
 * ViewModel for the Convai Character Dashboard.
 */

#pragma once

#include "CoreMinimal.h"
#include "Models/ConvaiCharacterMetadata.h"
#include "Delegates/Delegate.h"
#include "MVVM/ViewModel.h"
#include "ConvaiEditor.h"
#include "Events/EventAggregator.h"

class IConvaiCharacterApiService;
class IConvaiCharacterDiscoveryService;

/** ViewModel for the Convai Character Dashboard. */
class FCharacterDashboardViewModel : public FViewModelBase
{
public:
    /** Delegate type for notifying when the character list is updated */
    DECLARE_MULTICAST_DELEGATE(FOnCharacterListUpdated);

    /** Default constructor - services will be resolved in Initialize() */
    FCharacterDashboardViewModel();

    /** Refreshes the character list by discovering actors and fetching metadata */
    void RefreshCharacterList(UWorld *World);

    /** Returns the current list of character metadata (thread-safe) */
    const TArray<TSharedPtr<FConvaiCharacterMetadata>> &GetCharacters() const;

    /** Event delegate for UI to bind to character list updates */
    FOnCharacterListUpdated &OnCharacterListUpdated() { return CharacterListUpdatedEvent; }

    /** Initialize the ViewModel */
    virtual void Initialize() override;

    /** Clean up resources and delegates */
    virtual void Shutdown() override;

    /** Returns the type name used for registry lookup */
    static FName StaticType() { return TEXT("FCharacterDashboardViewModel"); }

private:
    /** Character metadata collection */
    TArray<TSharedPtr<FConvaiCharacterMetadata>> Characters;

    /** Weak pointers to services to avoid circular references */
    TWeakPtr<IConvaiCharacterApiService> ApiService;
    TWeakPtr<IConvaiCharacterDiscoveryService> DiscoveryService;

    /** Event for character list updates */
    FOnCharacterListUpdated CharacterListUpdatedEvent;

    /** Thread-safe access to character collection */
    FCriticalSection CharactersMutex;

    /** Event subscription for network restoration (RAII-based automatic cleanup) */
    ConvaiEditor::FEventSubscription NetworkRestoredSubscription;

    /** Cached World pointer for refresh operations */
    TWeakObjectPtr<UWorld> CachedWorld;

    /** Safe getter for ApiService with validity check */
    TSharedPtr<IConvaiCharacterApiService> GetApiService() const
    {
        auto Pinned = ApiService.Pin();
        if (!Pinned.IsValid())
        {
            UE_LOG(LogConvaiEditor, Warning,
                   TEXT("ApiService is no longer valid in CharacterDashboardViewModel"));
        }
        return Pinned;
    }

    /** Safe getter for DiscoveryService with validity check */
    TSharedPtr<IConvaiCharacterDiscoveryService> GetDiscoveryService() const
    {
        auto Pinned = DiscoveryService.Pin();
        if (!Pinned.IsValid())
        {
            UE_LOG(LogConvaiEditor, Warning,
                   TEXT("DiscoveryService is no longer valid in CharacterDashboardViewModel"));
        }
        return Pinned;
    }
};