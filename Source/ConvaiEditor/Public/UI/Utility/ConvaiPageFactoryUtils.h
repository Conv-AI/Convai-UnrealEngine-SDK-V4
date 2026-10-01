/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiPageFactoryUtils.h
 *
 * Centralized utilities for page factory creation and registration.
 * Eliminates code duplication in page factory creation logic.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Factories/IPageFactory.h"
#include "UI/Factories/PageFactoryManager.h"
#include "UI/Factories/ConvaiPageFactories.h"
#include "Services/Routes.h"
#include "ConvaiEditor.h"

class SWidget;
class SConvaiShell;
class SHomePage;
class SSamplesPage;
class SAccountPage;
class SSettingsPage;
class SSupportPage;
class SWebBrowserPage;

/** Page factory creation utilities. */
class CONVAIEDITOR_API FConvaiPageFactoryUtils
{
public:
    static TSharedPtr<FWebBrowserPageFactory> CreateWebBrowserFactory(ConvaiEditor::Route::E Route, const FString &URL);
    static TSharedPtr<FSupportPageFactory> CreateSupportPageFactory(TWeakPtr<SConvaiShell> ParentShell);
    static TArray<TSharedPtr<IPageFactory>> CreateStandardFactories(TWeakPtr<SConvaiShell> ParentShell);
    static bool RegisterFactoryWithLogging(
        TSharedPtr<IPageFactoryManager> FactoryManager,
        TSharedPtr<IPageFactory> Factory,
        const FString &RouteName);
    static int32 RegisterFactoriesWithLogging(
        TSharedPtr<IPageFactoryManager> FactoryManager,
        const TArray<TSharedPtr<IPageFactory>> &Factories);

private:
    FConvaiPageFactoryUtils() = delete;
    ~FConvaiPageFactoryUtils() = delete;
};

#define CREATE_STANDARD_PAGE_FACTORY(PageType, Route) \
    FConvaiPageFactoryUtils::CreateStandardFactory<PageType, F##PageType##Factory>(Route, TEXT(#PageType))

#define CREATE_WEB_BROWSER_FACTORY(Route, URL) \
    FConvaiPageFactoryUtils::CreateWebBrowserFactory(Route, URL)
