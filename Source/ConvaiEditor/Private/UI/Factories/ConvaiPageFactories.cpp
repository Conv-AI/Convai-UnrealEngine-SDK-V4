/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiPageFactories.cpp
 *
 * Implementation of concrete page factories with Result-based error handling.
 */

#include "UI/Factories/ConvaiPageFactories.h"
#include "UI/Pages/SHomePage.h"
#include "UI/Pages/SSamplesPage.h"
#include "UI/Pages/SAccountPage.h"
#include "UI/Pages/SSettingsPage.h"
#include "UI/Pages/SSupportPage.h"
#include "UI/Pages/SWebBrowserPage.h"
#include "UI/Shell/SConvaiShell.h"
#include "ConvaiEditor.h"

TConvaiResult<TSharedRef<SWidget>> FHomePageFactory::CreatePage()
{
    TSharedRef<SWidget> Page = SNew(SHomePage);
    return TConvaiResult<TSharedRef<SWidget>>::Success(Page);
}

TConvaiResult<TSharedRef<SWidget>> FSamplesPageFactory::CreatePage()
{
    TSharedRef<SWidget> Page = SNew(SSamplesPage);
    return TConvaiResult<TSharedRef<SWidget>>::Success(Page);
}

TConvaiResult<TSharedRef<SWidget>> FAccountPageFactory::CreatePage()
{
    TSharedRef<SWidget> Page = SNew(SAccountPage);
    return TConvaiResult<TSharedRef<SWidget>>::Success(Page);
}

TConvaiResult<TSharedRef<SWidget>> FSettingsPageFactory::CreatePage()
{
    TSharedRef<SWidget> Page = SNew(SSettingsPage);
    return TConvaiResult<TSharedRef<SWidget>>::Success(Page);
}

TConvaiResult<TSharedRef<SWidget>> FSupportPageFactory::CreatePage()
{
    TSharedRef<SWidget> Page = SNew(SSupportPage);
    return TConvaiResult<TSharedRef<SWidget>>::Success(Page);
}

TConvaiResult<TSharedRef<SWidget>> FWebBrowserPageFactory::CreatePage()
{
    if (URL.IsEmpty())
    {
        return TConvaiResult<TSharedRef<SWidget>>::Failure(TEXT("Cannot create Web Browser Page with empty URL"));
    }

    TSharedRef<SWidget> Page = SNew(SWebBrowserPage)
                                   .URL(URL);

    return TConvaiResult<TSharedRef<SWidget>>::Success(Page);
}
