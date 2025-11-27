/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * PageFactoryManager.h
 *
 * Manager for page factories and page creation.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Services/Routes.h"
#include "UI/Factories/IPageFactory.h"
#include "Services/ConvaiDIContainer.h"

class SWidget;

/**
 * Interface for managing page factories.
 */
class CONVAIEDITOR_API IPageFactoryManager : public IConvaiService
{
public:
    virtual ~IPageFactoryManager() = default;

    /** Registers a page factory */
    virtual bool RegisterFactory(TSharedPtr<IPageFactory> Factory) = 0;

    /** Unregisters a factory for a specific route */
    virtual bool UnregisterFactory(ConvaiEditor::Route::E Route) = 0;

    /** Creates a page for the given route */
    virtual TConvaiResult<TSharedPtr<SWidget>> CreatePage(ConvaiEditor::Route::E Route) = 0;

    /** Returns whether a factory is registered for the given route */
    virtual bool HasFactory(ConvaiEditor::Route::E Route) const = 0;

    /** Returns all registered routes */
    virtual TArray<ConvaiEditor::Route::E> GetRegisteredRoutes() const = 0;

    /** Updates the URL for a web browser factory */
    virtual bool UpdateWebBrowserURL(ConvaiEditor::Route::E Route, const FString &NewURL) = 0;

    /** Returns the service type for registration */
    static FName StaticType() { return TEXT("IPageFactoryManager"); }
};

/**
 * Page factory manager implementation.
 */
class CONVAIEDITOR_API FPageFactoryManager : public IPageFactoryManager
{
public:
    FPageFactoryManager() = default;
    virtual ~FPageFactoryManager() = default;

    virtual void Startup() override;
    virtual void Shutdown() override;
    virtual bool RegisterFactory(TSharedPtr<IPageFactory> Factory) override;
    virtual bool UnregisterFactory(ConvaiEditor::Route::E Route) override;
    virtual TConvaiResult<TSharedPtr<SWidget>> CreatePage(ConvaiEditor::Route::E Route) override;
    virtual bool HasFactory(ConvaiEditor::Route::E Route) const override;
    virtual TArray<ConvaiEditor::Route::E> GetRegisteredRoutes() const override;
    virtual bool UpdateWebBrowserURL(ConvaiEditor::Route::E Route, const FString &NewURL) override;

    static FName StaticType() { return TEXT("IPageFactoryManager"); }

private:
    mutable FRWLock FactoriesLock;
    TMap<ConvaiEditor::Route::E, TSharedPtr<IPageFactory>> Factories;
};
