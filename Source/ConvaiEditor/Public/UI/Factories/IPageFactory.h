/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IPageFactory.h
 *
 * Interface for page factories that create UI pages.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Services/Routes.h"
#include "Services/ConvaiDIContainer.h"

class SWidget;

/**
 * Interface for page factories that create UI pages.
 */
class CONVAIEDITOR_API IPageFactory : public IConvaiService
{
public:
    virtual ~IPageFactory() = default;

    /** Creates a page widget */
    virtual TConvaiResult<TSharedRef<SWidget>> CreatePage() = 0;

    /** Returns the route this factory handles */
    virtual ConvaiEditor::Route::E GetRoute() const = 0;

    /** Returns whether this factory can handle the given route */
    virtual bool CanHandle(ConvaiEditor::Route::E Route) const
    {
        return GetRoute() == Route;
    }

    /** Returns the factory type name */
    virtual FName GetFactoryType() const = 0;

    /** Updates URL for web browser factories */
    virtual bool UpdateURL(const FString &NewURL) { return false; }

    /** Returns the service type for registration */
    static FName StaticType() { return TEXT("IPageFactory"); }
};

/**
 * Base implementation for page factories.
 */
class CONVAIEDITOR_API FPageFactoryBase : public IPageFactory
{
public:
    FPageFactoryBase(ConvaiEditor::Route::E InRoute) : Route(InRoute) {}

    virtual void Startup() override {}
    virtual void Shutdown() override {}
    virtual ConvaiEditor::Route::E GetRoute() const override { return Route; }
    virtual FName GetFactoryType() const override { return TEXT("FPageFactoryBase"); }

protected:
    ConvaiEditor::Route::E Route;
};
