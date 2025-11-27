/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * PageFactoryManager.cpp
 *
 * Implementation of Page Factory Manager for centralized page creation.
 */

#include "UI/Factories/PageFactoryManager.h"
#include "UI/Factories/ConvaiPageFactories.h"
#include "ConvaiEditor.h"

void FPageFactoryManager::Startup()
{
}

void FPageFactoryManager::Shutdown()
{
    FWriteScopeLock WriteLock(FactoriesLock);

    for (auto &FactoryPair : Factories)
    {
        if (FactoryPair.Value.IsValid())
        {
            FactoryPair.Value->Shutdown();
        }
    }

    Factories.Empty();
}

bool FPageFactoryManager::RegisterFactory(TSharedPtr<IPageFactory> Factory)
{
    if (!Factory.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("PageFactoryManager: cannot register null page factory"));
        return false;
    }

    FWriteScopeLock WriteLock(FactoriesLock);

    ConvaiEditor::Route::E Route = Factory->GetRoute();

    if (Factories.Contains(Route))
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("PageFactoryManager: replacing existing page factory for route: %s"),
               *ConvaiEditor::Route::ToString(Route));

        if (Factories[Route].IsValid())
        {
            Factories[Route]->Shutdown();
        }
    }

    Factory->Startup();

    Factories.Add(Route, Factory);

    return true;
}

bool FPageFactoryManager::UnregisterFactory(ConvaiEditor::Route::E Route)
{
    FWriteScopeLock WriteLock(FactoriesLock);

    TSharedPtr<IPageFactory> *FoundFactory = Factories.Find(Route);
    if (!FoundFactory || !FoundFactory->IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("PageFactoryManager: no factory found to unregister for route: %s"),
               *ConvaiEditor::Route::ToString(Route));
        return false;
    }

    (*FoundFactory)->Shutdown();

    Factories.Remove(Route);

    return true;
}

TConvaiResult<TSharedPtr<SWidget>> FPageFactoryManager::CreatePage(ConvaiEditor::Route::E Route)
{
    TSharedPtr<IPageFactory> Factory;

    {
        FReadScopeLock ReadLock(FactoriesLock);

        TSharedPtr<IPageFactory> *FoundFactory = Factories.Find(Route);
        if (!FoundFactory || !FoundFactory->IsValid())
        {
            FString ErrorMsg = FString::Printf(TEXT("No factory registered for route: %s"),
                                               *ConvaiEditor::Route::ToString(Route));
            UE_LOG(LogConvaiEditor, Error, TEXT("PageFactoryManager: %s"), *ErrorMsg);
            return TConvaiResult<TSharedPtr<SWidget>>::Failure(ErrorMsg);
        }

        Factory = *FoundFactory;
    }

    auto PageResult = Factory->CreatePage();
    if (PageResult.IsFailure())
    {
        FString ErrorMsg = FString::Printf(TEXT("Factory failed to create page for route %s: %s"),
                                           *ConvaiEditor::Route::ToString(Route),
                                           *PageResult.GetError());
        UE_LOG(LogConvaiEditor, Error, TEXT("PageFactoryManager: %s"), *ErrorMsg);
        return TConvaiResult<TSharedPtr<SWidget>>::Failure(ErrorMsg);
    }

    TSharedPtr<SWidget> Page = PageResult.GetValue();
    return TConvaiResult<TSharedPtr<SWidget>>::Success(Page);
}

bool FPageFactoryManager::HasFactory(ConvaiEditor::Route::E Route) const
{
    FReadScopeLock ReadLock(FactoriesLock);
    return Factories.Contains(Route);
}

TArray<ConvaiEditor::Route::E> FPageFactoryManager::GetRegisteredRoutes() const
{
    FReadScopeLock ReadLock(FactoriesLock);

    TArray<ConvaiEditor::Route::E> Routes;
    Factories.GetKeys(Routes);
    return Routes;
}

bool FPageFactoryManager::UpdateWebBrowserURL(ConvaiEditor::Route::E Route, const FString &NewURL)
{
    FReadScopeLock ReadLock(FactoriesLock);

    TSharedPtr<IPageFactory> *FoundFactory = Factories.Find(Route);
    if (!FoundFactory || !FoundFactory->IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("PageFactoryManager: no factory found for route: %s"),
               *ConvaiEditor::Route::ToString(Route));
        return false;
    }

    TSharedPtr<IPageFactory> Factory = *FoundFactory;
    if (Factory->UpdateURL(NewURL))
    {
        return true;
    }

    UE_LOG(LogConvaiEditor, Warning, TEXT("PageFactoryManager: factory for route %s does not support URL updates (type: %s)"),
           *ConvaiEditor::Route::ToString(Route), *Factory->GetFactoryType().ToString());
    return false;
}
