/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiPageFactoryUtils.cpp
 *
 * Implementation of centralized page factory creation utilities.
 */

#include "UI/Utility/ConvaiPageFactoryUtils.h"
#include "UI/Factories/PageFactoryManager.h"
#include "UI/Shell/SConvaiShell.h"
#include "UI/Pages/SHomePage.h"
#include "UI/Pages/SSamplesPage.h"
#include "UI/Pages/SAccountPage.h"
#include "UI/Pages/SSettingsPage.h"
#include "UI/Pages/SSupportPage.h"
#include "UI/Pages/SWebBrowserPage.h"
#include "Services/Routes.h"
#include "Utility/ConvaiURLs.h"
#include "ConvaiEditor.h"

TSharedPtr<FWebBrowserPageFactory> FConvaiPageFactoryUtils::CreateWebBrowserFactory(ConvaiEditor::Route::E Route, const FString &URL)
{
    return MakeShared<FWebBrowserPageFactory>(Route, URL);
}

TSharedPtr<FSupportPageFactory> FConvaiPageFactoryUtils::CreateSupportPageFactory(TWeakPtr<SConvaiShell> ParentShell)
{
    return MakeShared<FSupportPageFactory>(ParentShell);
}

TArray<TSharedPtr<IPageFactory>> FConvaiPageFactoryUtils::CreateStandardFactories(TWeakPtr<SConvaiShell> ParentShell)
{
    TArray<TSharedPtr<IPageFactory>> Factories;

    Factories.Add(MakeShared<FHomePageFactory>());
    Factories.Add(MakeShared<FSamplesPageFactory>());
    Factories.Add(MakeShared<FSettingsPageFactory>());
    Factories.Add(MakeShared<FAccountPageFactory>());
    Factories.Add(CreateSupportPageFactory(ParentShell));

    Factories.Add(CreateWebBrowserFactory(ConvaiEditor::Route::E::Dashboard, FConvaiURLs::GetDashboardURL()));
    Factories.Add(CreateWebBrowserFactory(ConvaiEditor::Route::E::Experiences, FConvaiURLs::GetExperiencesURL()));
    Factories.Add(CreateWebBrowserFactory(ConvaiEditor::Route::E::Documentation, FConvaiURLs::GetDocumentationURL()));
    Factories.Add(CreateWebBrowserFactory(ConvaiEditor::Route::E::Forum, FConvaiURLs::GetForumURL()));
    Factories.Add(CreateWebBrowserFactory(ConvaiEditor::Route::E::YouTubeVideo, FConvaiURLs::GetYouTubeURL()));

    return Factories;
}

bool FConvaiPageFactoryUtils::RegisterFactoryWithLogging(
    TSharedPtr<IPageFactoryManager> FactoryManager,
    TSharedPtr<IPageFactory> Factory,
    const FString &RouteName)
{
    if (!FactoryManager.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Page factory manager is null for route: %s"), *RouteName);
        return false;
    }

    if (!Factory.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Page factory instance is null for route: %s"), *RouteName);
        return false;
    }

    const bool bSuccess = FactoryManager->RegisterFactory(Factory);
    if (!bSuccess)
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Page factory registration failed for route: %s"), *RouteName);
    }

    return bSuccess;
}

int32 FConvaiPageFactoryUtils::RegisterFactoriesWithLogging(
    TSharedPtr<IPageFactoryManager> FactoryManager,
    const TArray<TSharedPtr<IPageFactory>> &Factories)
{
    if (!FactoryManager.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Page factory manager is null"));
        return 0;
    }

    int32 SuccessCount = 0;
    for (const auto &Factory : Factories)
    {
        if (Factory.IsValid())
        {
            const bool bSuccess = FactoryManager->RegisterFactory(Factory);
            if (bSuccess)
            {
                SuccessCount++;
            }
        }
    }

    return SuccessCount;
}
