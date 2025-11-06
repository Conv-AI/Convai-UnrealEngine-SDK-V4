/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * FontTokensLoader.cpp
 *
 * Implementation of font token loading from constants.
 */

#include "Styling/TokenLoaders/FontTokensLoader.h"
#include "Utility/ConvaiConstants.h"
#include "Logging/ConvaiEditorThemeLog.h"
#include "Interfaces/IPluginManager.h"
#include "Fonts/SlateFontInfo.h"
#include "Misc/Paths.h"

void FFontTokensLoader::Load(const TSharedPtr<FJsonObject> &Tokens, TSharedPtr<FSlateStyleSet> Style)
{
    if (!Style.IsValid())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("FontTokensLoader: invalid Style parameter"));
        return;
    }

    LoadFontTokensFromConstants(Style);
}

void FFontTokensLoader::LoadFontTokensFromConstants(TSharedPtr<FSlateStyleSet> Style)
{
    using namespace ConvaiEditor::Constants::Typography;

    const FString PluginBaseDir = IPluginManager::Get().FindPlugin(TEXT("Convai"))->GetBaseDir();
    const FString FontsDir = FPaths::Combine(*PluginBaseDir, ConvaiEditor::Constants::PluginResources::Fonts);

    TMap<FString, FString> FontFamilies;
    FontFamilies.Add(TEXT("IBMPlexSansBold"), FPaths::Combine(*FontsDir, ConvaiEditor::Constants::Typography::FontFiles::Bold));
    FontFamilies.Add(TEXT("IBMPlexSansRegular"), FPaths::Combine(*FontsDir, ConvaiEditor::Constants::Typography::FontFiles::Regular));
    FontFamilies.Add(TEXT("IBMPlexSansMedium"), FPaths::Combine(*FontsDir, ConvaiEditor::Constants::Typography::FontFiles::Medium));

    RegisterFontStyle(Style, TEXT("nav"), Styles::Nav::Family, Styles::Nav::Size, FontFamilies);
    RegisterFontStyle(Style, TEXT("dropdown"), Styles::Dropdown::Family, Styles::Dropdown::Size, FontFamilies);
    RegisterFontStyle(Style, TEXT("dropdownNav"), Styles::DropdownNav::Family, Styles::DropdownNav::Size, FontFamilies);
    RegisterFontStyle(Style, TEXT("dropdownIcon"), Styles::DropdownIcon::Family, Styles::DropdownIcon::Size, FontFamilies);
    RegisterFontStyle(Style, TEXT("sampleCardTitle"), Styles::SampleCardTitle::Family, Styles::SampleCardTitle::Size, FontFamilies);
    RegisterFontStyle(Style, TEXT("sampleCardTag"), Styles::SampleCardTag::Family, Styles::SampleCardTag::Size, FontFamilies);
    RegisterFontStyle(Style, TEXT("accountSectionTitle"), Styles::AccountSectionTitle::Family, Styles::AccountSectionTitle::Size, FontFamilies);
    RegisterFontStyle(Style, TEXT("accountLabel"), Styles::AccountLabel::Family, Styles::AccountLabel::Size, FontFamilies);
    RegisterFontStyle(Style, TEXT("accountValue"), Styles::AccountValue::Family, Styles::AccountValue::Size, FontFamilies);
    RegisterFontStyle(Style, TEXT("supportResourceLabel"), Styles::SupportResourceLabel::Family, Styles::SupportResourceLabel::Size, FontFamilies);
    RegisterFontStyle(Style, TEXT("infoBox"), Styles::InfoBox::Family, Styles::InfoBox::Size, FontFamilies);
}

void FFontTokensLoader::RegisterFontStyle(TSharedPtr<FSlateStyleSet> Style, const FString &StyleName,
                                          const FString &FamilyName, float FontSize,
                                          const TMap<FString, FString> &FontFamilies)
{
    if (FontFamilies.Contains(FamilyName))
    {
        FString FontPath = FontFamilies[FamilyName];
        FSlateFontInfo FontInfo(FontPath, FMath::RoundToInt(FontSize));

        FName StyleKey = FName(*BuildKey(TEXT("Font"), StyleName));
        Style->Set(StyleKey, FontInfo);
    }
    else
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("FontTokensLoader: font family not found: %s"), *FamilyName);
    }
}
