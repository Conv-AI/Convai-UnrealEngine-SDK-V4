/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiPageFactories.h
 *
 * Page factory implementations for all Convai UI pages.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Factories/IPageFactory.h"

class SConvaiShell;

/**
 * Factory for creating the home page.
 */
class CONVAIEDITOR_API FHomePageFactory : public FPageFactoryBase
{
public:
    FHomePageFactory() : FPageFactoryBase(ConvaiEditor::Route::E::Home) {}
    virtual TConvaiResult<TSharedRef<SWidget>> CreatePage() override;
    virtual FName GetFactoryType() const override { return TEXT("FHomePageFactory"); }
    static FName StaticType() { return TEXT("FHomePageFactory"); }
};

/**
 * Factory for creating the samples page.
 */
class CONVAIEDITOR_API FSamplesPageFactory : public FPageFactoryBase
{
public:
    FSamplesPageFactory() : FPageFactoryBase(ConvaiEditor::Route::E::Samples) {}
    virtual TConvaiResult<TSharedRef<SWidget>> CreatePage() override;
    virtual FName GetFactoryType() const override { return TEXT("FSamplesPageFactory"); }
    static FName StaticType() { return TEXT("FSamplesPageFactory"); }
};

/**
 * Factory for creating the account page.
 */
class CONVAIEDITOR_API FAccountPageFactory : public FPageFactoryBase
{
public:
    FAccountPageFactory() : FPageFactoryBase(ConvaiEditor::Route::E::Account) {}
    virtual TConvaiResult<TSharedRef<SWidget>> CreatePage() override;
    virtual FName GetFactoryType() const override { return TEXT("FAccountPageFactory"); }
    static FName StaticType() { return TEXT("FAccountPageFactory"); }
};

/**
 * Factory for creating the settings page.
 */
class CONVAIEDITOR_API FSettingsPageFactory : public FPageFactoryBase
{
public:
    FSettingsPageFactory() : FPageFactoryBase(ConvaiEditor::Route::E::Settings) {}
    virtual TConvaiResult<TSharedRef<SWidget>> CreatePage() override;
    virtual FName GetFactoryType() const override { return TEXT("FSettingsPageFactory"); }
    static FName StaticType() { return TEXT("FSettingsPageFactory"); }
};

/**
 * Factory for creating the support page.
 */
class CONVAIEDITOR_API FSupportPageFactory : public FPageFactoryBase
{
public:
    FSupportPageFactory(TWeakPtr<SConvaiShell> InParentShell = nullptr)
        : FPageFactoryBase(ConvaiEditor::Route::E::Support), ParentShell(InParentShell) {}

    virtual TConvaiResult<TSharedRef<SWidget>> CreatePage() override;
    virtual FName GetFactoryType() const override { return TEXT("FSupportPageFactory"); }

    /** Sets the parent shell for the support page */
    void SetParentShell(TWeakPtr<SConvaiShell> InParentShell) { ParentShell = InParentShell; }

    static FName StaticType() { return TEXT("FSupportPageFactory"); }

private:
    TWeakPtr<SConvaiShell> ParentShell;
};

/**
 * Factory for creating web browser pages.
 */
class CONVAIEDITOR_API FWebBrowserPageFactory : public FPageFactoryBase
{
public:
    FWebBrowserPageFactory(ConvaiEditor::Route::E InRoute, const FString &InURL)
        : FPageFactoryBase(InRoute), URL(InURL) {}

    virtual TConvaiResult<TSharedRef<SWidget>> CreatePage() override;
    virtual FName GetFactoryType() const override { return TEXT("FWebBrowserPageFactory"); }
    virtual bool UpdateURL(const FString &NewURL) override
    {
        URL = NewURL;
        return true;
    }

    /** Returns the URL for this web browser page */
    const FString &GetURL() const { return URL; }

    /** Sets the URL for this web browser page */
    void SetURL(const FString &InURL) { URL = InURL; }

    static FName StaticType() { return TEXT("FWebBrowserPageFactory"); }

private:
    FString URL;
};
