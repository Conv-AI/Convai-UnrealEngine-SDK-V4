/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SSupportPage.cpp
 *
 * Implementation of the support page.
 */

#include "UI/Pages/SSupportPage.h"
#include "UI/Widgets/SCard.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/ConvaiStyleResources.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Interfaces/IPluginManager.h"
#include "MVVM/SamplesViewModel.h" // For FSampleItem
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SCompoundWidget.h"
#include "Brushes/SlateColorBrush.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Services/NavigationService.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/Routes.h"
#include "UI/Pages/SWebBrowserPage.h"
#include "UI/Factories/PageFactoryManager.h"
#include "Utility/ConvaiURLs.h"
#include "ConvaiEditor.h"
#include "HAL/PlatformProcess.h"
#include "Utility/ConvaiConstants.h"
#include "UI/Utility/ConvaiWidgetFactory.h"

void SSupportPage::Construct(const FArguments &InArgs)
{
    DocumentationImageBrush = FConvaiStyle::Get().GetBrush("Convai.Support.Documentation");
    YoutubeTutorialsImageBrush = FConvaiStyle::Get().GetBrush("Convai.Support.YoutubeTutorials");
    DeveloperForumImageBrush = FConvaiStyle::Get().GetBrush("Convai.Support.ConvaiDeveloperForum");

    BaseResourceCardSize = FVector2D(350.0f, 550.0f);
    CurrentCardSize = BaseResourceCardSize;
    ResourceCardBorderRadius = ConvaiEditor::Constants::Layout::Radius::StandardCard;
    const float ActualBorderThickness = ConvaiEditor::Constants::Layout::Components::StandardCard::BorderThickness;
    ResourceCardSpacing = 20.0f;

    TSharedRef<SWidget> ContentWidget =
        SNew(SBox)
            .Padding(FMargin(20.0f))
                [SNew(SScaleBox)
                     .Stretch(EStretch::ScaleToFit)
                     .StretchDirection(EStretchDirection::Both)
                         [SAssignNew(CardsContainer, SHorizontalBox)

                          + SHorizontalBox::Slot()
                                .Padding(ResourceCardSpacing / 2.0f)
                                .HAlign(HAlign_Center)
                                .VAlign(VAlign_Center)
                                .FillWidth(1.0f)
                                    [CreateSupportCard(DocumentationImageBrush, NSLOCTEXT("ConvaiEditor", "Documentation", "Documentation"), ActualBorderThickness, FOnClicked::CreateSP(this, &SSupportPage::OnDocumentationCardClicked))]

                          + SHorizontalBox::Slot()
                                .Padding(ResourceCardSpacing / 2.0f)
                                .HAlign(HAlign_Center)
                                .VAlign(VAlign_Center)
                                .FillWidth(1.0f)
                                    [CreateSupportCard(YoutubeTutorialsImageBrush, NSLOCTEXT("ConvaiEditor", "YoutubeTutorials", "Youtube Tutorials"), ActualBorderThickness, FOnClicked::CreateSP(this, &SSupportPage::OnYouTubeCardClicked))]

                          + SHorizontalBox::Slot()
                                .Padding(ResourceCardSpacing / 2.0f)
                                .HAlign(HAlign_Center)
                                .VAlign(VAlign_Center)
                                .FillWidth(1.0f)
                                    [CreateSupportCard(DeveloperForumImageBrush, NSLOCTEXT("ConvaiEditor", "DeveloperForum", "Convai Developer Forum"), ActualBorderThickness, FOnClicked::CreateSP(this, &SSupportPage::OnForumCardClicked))]]];

    SBasePage::Construct(
        SBasePage::FArguments()
            .Content()
                [ContentWidget]);
}

TSharedRef<SWidget> SSupportPage::CreateSupportCard(const FSlateBrush *ImageBrush, const FText &LabelText, float InBorderThickness, FOnClicked OnClickedDelegate)
{
    return FConvaiWidgetFactory::CreateClickableCard(
        LabelText,
        FText::GetEmpty(), // No description for support cards
        ImageBrush,
        OnClickedDelegate,
        CurrentCardSize,
        ResourceCardBorderRadius,
        InBorderThickness);
}

FReply SSupportPage::OnDocumentationCardClicked()
{
    const FString DocumentationURL = FConvaiURLs::GetAPIDocumentationURL();

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7)
    FPlatformProcess::LaunchURL(*DocumentationURL, nullptr, nullptr);
#else
    auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
    if (NavResult.IsSuccess())
    {
        TSharedPtr<INavigationService> NavService = NavResult.GetValue();

        auto PageFactoryManagerResult = FConvaiDIContainerManager::Get().Resolve<IPageFactoryManager>();
        if (PageFactoryManagerResult.IsSuccess())
        {
            PageFactoryManagerResult.GetValue()->UpdateWebBrowserURL(ConvaiEditor::Route::E::Documentation, DocumentationURL);
        }

        NavService->Navigate(ConvaiEditor::Route::E::Documentation);
    }
    else
    {
        FPlatformProcess::LaunchURL(*DocumentationURL, nullptr, nullptr);
        UE_LOG(LogConvaiEditor, Warning, TEXT("NavigationService unavailable, opening Documentation externally: %s"), *DocumentationURL);
    }
#endif

    return FReply::Handled();
}

FReply SSupportPage::OnYouTubeCardClicked()
{
    const FString YouTubeURL = FConvaiURLs::GetYouTubeURL();

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7)
    FPlatformProcess::LaunchURL(*YouTubeURL, nullptr, nullptr);
#else
    auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
    if (NavResult.IsSuccess())
    {
        TSharedPtr<INavigationService> NavService = NavResult.GetValue();

        auto PageFactoryManagerResult = FConvaiDIContainerManager::Get().Resolve<IPageFactoryManager>();
        if (PageFactoryManagerResult.IsSuccess())
        {
            PageFactoryManagerResult.GetValue()->UpdateWebBrowserURL(ConvaiEditor::Route::E::YouTubeVideo, YouTubeURL);
        }

        NavService->Navigate(ConvaiEditor::Route::E::YouTubeVideo);
    }
    else
    {
        FPlatformProcess::LaunchURL(*YouTubeURL, nullptr, nullptr);
        UE_LOG(LogConvaiEditor, Warning, TEXT("NavigationService unavailable, opening YouTube externally: %s"), *YouTubeURL);
    }
#endif

    return FReply::Handled();
}

FReply SSupportPage::OnForumCardClicked()
{
    const FString ForumURL = FConvaiURLs::GetForumURL();

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7)
    FPlatformProcess::LaunchURL(*ForumURL, nullptr, nullptr);
#else
    auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
    if (NavResult.IsSuccess())
    {
        TSharedPtr<INavigationService> NavService = NavResult.GetValue();

        auto PageFactoryManagerResult = FConvaiDIContainerManager::Get().Resolve<IPageFactoryManager>();
        if (PageFactoryManagerResult.IsSuccess())
        {
            PageFactoryManagerResult.GetValue()->UpdateWebBrowserURL(ConvaiEditor::Route::E::Forum, ForumURL);
        }

        NavService->Navigate(ConvaiEditor::Route::E::Forum);
    }
    else
    {
        FPlatformProcess::LaunchURL(*ForumURL, nullptr, nullptr);
        UE_LOG(LogConvaiEditor, Warning, TEXT("NavigationService unavailable, opening Forum externally: %s"), *ForumURL);
    }
#endif

    return FReply::Handled();
}
