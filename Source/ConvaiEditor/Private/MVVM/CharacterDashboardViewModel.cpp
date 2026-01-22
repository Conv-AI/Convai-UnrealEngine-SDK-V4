/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * CharacterDashboardViewModel.cpp
 *
 * Implementation of the character dashboard view model.
 */

#include "MVVM/CharacterDashboardViewModel.h"
#include "Services/ConvaiCharacterDiscoveryService.h"
#include "Services/ConvaiCharacterApiService.h"
#include "Services/ConvaiDIContainer.h"
#include "Utility/ConvaiValidationUtils.h"
#include "ConvaiEditor.h"
#include "Engine/World.h"
#include "Async/Async.h"
#include "Events/EventAggregator.h"
#include "Events/EventTypes.h"

FCharacterDashboardViewModel::FCharacterDashboardViewModel()
    : FViewModelBase()
{
}

void FCharacterDashboardViewModel::Initialize()
{
    FViewModelBase::Initialize();

    FConvaiValidationUtils::ResolveServiceWithCallbacks<IConvaiCharacterApiService>(
        TEXT("FCharacterDashboardViewModel::Initialize"),
        [this](TSharedPtr<IConvaiCharacterApiService> Service)
        {
            ApiService = Service;
        },
        [](const FString &Error)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("CharacterDashboardViewModel: ApiService resolution failed - %s"), *Error);
        });

    FConvaiValidationUtils::ResolveServiceWithCallbacks<IConvaiCharacterDiscoveryService>(
        TEXT("FCharacterDashboardViewModel::Initialize"),
        [this](TSharedPtr<IConvaiCharacterDiscoveryService> Service)
        {
            DiscoveryService = Service;
        },
        [](const FString &Error)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("CharacterDashboardViewModel: DiscoveryService resolution failed - %s"), *Error);
        });

    TWeakPtr<FCharacterDashboardViewModel> WeakViewModel = SharedThis(this);
    NetworkRestoredSubscription = ConvaiEditor::FEventAggregator::Get().Subscribe<ConvaiEditor::FNetworkRestoredEvent>(
        WeakViewModel,
        [WeakViewModel](const ConvaiEditor::FNetworkRestoredEvent &Event)
        {
            if (TSharedPtr<FCharacterDashboardViewModel> ViewModel = WeakViewModel.Pin())
            {
                if (ViewModel->CachedWorld.IsValid())
                {
                    ViewModel->RefreshCharacterList(ViewModel->CachedWorld.Get());
                }
                else
                {
                    UE_LOG(LogConvaiEditor, Warning, TEXT("CharacterDashboardViewModel: No cached World available for auto-refresh"));
                }
            }
        });
}

void FCharacterDashboardViewModel::Shutdown()
{
    NetworkRestoredSubscription.Unsubscribe();

    {
        FScopeLock Lock(&CharactersMutex);
        Characters.Empty();
    }

    CharacterListUpdatedEvent.Clear();

    ApiService.Reset();
    DiscoveryService.Reset();

    CachedWorld.Reset();

    FViewModelBase::Shutdown();
}

void FCharacterDashboardViewModel::RefreshCharacterList(UWorld *World)
{
    if (World)
    {
        CachedWorld = World;
    }

    auto ApiSvc = GetApiService();
    auto DiscoverySvc = GetDiscoveryService();

    if (!World || !ApiSvc.IsValid() || !DiscoverySvc.IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning,
               TEXT("Cannot refresh character list: Invalid dependencies (World=%s, ApiService=%s, DiscoveryService=%s)"),
               World ? TEXT("Valid") : TEXT("Null"),
               ApiSvc.IsValid() ? TEXT("Valid") : TEXT("Invalid"),
               DiscoverySvc.IsValid() ? TEXT("Valid") : TEXT("Invalid"));
        return;
    }

    TArray<FString> CharacterIDs;
    DiscoverySvc->GetAllConvaiCharacterIDsInLevel(World, CharacterIDs);

    TArray<TSharedPtr<FConvaiCharacterMetadata>> NewCharacters;
    TArray<TFuture<TOptional<FConvaiCharacterMetadata>>> Futures;
    for (const FString &ID : CharacterIDs)
    {
        Futures.Add(ApiSvc->FetchCharacterMetadataAsync(ID));
    }

    Async(EAsyncExecution::ThreadPool, [this, Futures = MoveTemp(Futures)]() mutable
          {
        TArray<TSharedPtr<FConvaiCharacterMetadata>> Results;
        for (TFuture<TOptional<FConvaiCharacterMetadata>>& Future : Futures)
        {
            Future.Wait();
            if (Future.Get().IsSet())
            {
                Results.Add(MakeShared<FConvaiCharacterMetadata>(Future.Get().GetValue()));
            }
        }
        {
            FScopeLock Lock(&CharactersMutex);
            Characters = MoveTemp(Results);
        }
        AsyncTask(ENamedThreads::GameThread, [this]() {
            CharacterListUpdatedEvent.Broadcast();
        }); });
}

const TArray<TSharedPtr<FConvaiCharacterMetadata>> &FCharacterDashboardViewModel::GetCharacters() const
{
    return Characters;
}