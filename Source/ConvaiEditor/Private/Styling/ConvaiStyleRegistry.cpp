/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiStyleRegistry.cpp
 *
 * Implementation of the style registry for Slate styles.
 */

#include "ConvaiStyleRegistry.h"
#include "Styling/IThemeManager.h"
#include "Styling/SlateStyleRegistry.h"
#include "Services/ConvaiDIContainer.h"
#include "Logging/ConvaiEditorThemeLog.h"
#include "Styling/SlateStyle.h"
#include "Brushes/SlateColorBrush.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Utility/ConvaiConstants.h"

const FName FConvaiStyleRegistry::StyleSetName(TEXT("ConvaiStyle"));

FConvaiStyleRegistry::FConvaiStyleRegistry()
    : bInitialized(false)
{
}

FConvaiStyleRegistry::~FConvaiStyleRegistry()
{
    if (bInitialized.Load())
    {
        Shutdown();
    }
}

void FConvaiStyleRegistry::Startup()
{
    auto Result = InitializeStyleRegistry(nullptr);
    if (Result.IsFailure())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ConvaiStyleRegistry: failed to initialize - %s"), *Result.GetError());
    }
}

void FConvaiStyleRegistry::Shutdown()
{
    auto Result = ShutdownStyleRegistry();
    if (Result.IsFailure())
    {
        UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyleRegistry: shutdown error - %s"), *Result.GetError());
    }
}

TConvaiResult<void> FConvaiStyleRegistry::InitializeStyleRegistry(const TSharedPtr<FJsonObject> &ThemeJson)
{
    if (bInitialized.Load())
    {
        UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyleRegistry: already initialized"));
        return TConvaiResult<void>::Success();
    }

    auto ThemeResult = FConvaiDIContainerManager::Get().Resolve<IThemeManager>();
    if (ThemeResult.IsFailure())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ConvaiStyleRegistry: failed to resolve ThemeManager - %s"), *ThemeResult.GetError());
        return TConvaiResult<void>::Failure(TEXT("Failed to resolve ThemeManager"));
    }

    ThemeManager = ThemeResult.GetValue();
    if (!ThemeManager.IsValid())
    {
        return TConvaiResult<void>::Failure(TEXT("ThemeManager is invalid"));
    }

    auto CreateResult = CreateStyleSet(ThemeJson);
    if (CreateResult.IsFailure())
    {
        return CreateResult;
    }

    auto RegisterResult = RegisterStyleSet();
    if (RegisterResult.IsFailure())
    {
        return RegisterResult;
    }

    if (auto PinnedThemeManager = ThemeManager.Pin())
    {
        PinnedThemeManager->OnThemeChanged().AddRaw(this, &FConvaiStyleRegistry::OnThemeChanged);
    }

    bInitialized.Store(true);
    return TConvaiResult<void>::Success();
}

TConvaiResult<void> FConvaiStyleRegistry::ShutdownStyleRegistry()
{
    if (!bInitialized.Load())
    {
        return TConvaiResult<void>::Success();
    }

    auto UnregisterResult = UnregisterStyleSet();
    if (UnregisterResult.IsFailure())
    {
        UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyleRegistry: failed to unregister style set - %s"), *UnregisterResult.GetError());
    }

    CleanupResources();

    bInitialized.Store(false);
    return TConvaiResult<void>::Success();
}

TSharedPtr<FSlateStyleSet> FConvaiStyleRegistry::GetStyleSet() const
{
    FReadScopeLock Lock(StyleSetLock);
    return StyleSet;
}

TSharedPtr<FSlateStyleSet> FConvaiStyleRegistry::GetMutableStyleSet()
{
    FWriteScopeLock Lock(StyleSetLock);
    return StyleSet;
}

bool FConvaiStyleRegistry::IsInitialized() const
{
    return bInitialized.Load() && StyleSet.IsValid();
}

FName FConvaiStyleRegistry::GetStyleSetName() const
{
    return StyleSetName;
}

TConvaiResult<void> FConvaiStyleRegistry::RefreshStyleSet()
{
    if (!IsInitialized())
    {
        return TConvaiResult<void>::Failure(TEXT("Style registry not initialized"));
    }

    auto PinnedThemeManager = ThemeManager.Pin();
    if (!PinnedThemeManager.IsValid())
    {
        return TConvaiResult<void>::Failure(TEXT("ThemeManager is no longer valid"));
    }

    auto UnregisterResult = UnregisterStyleSet();
    if (UnregisterResult.IsFailure())
    {
        UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyleRegistry: failed to unregister style set during refresh - %s"), *UnregisterResult.GetError());
    }

    auto CreateResult = CreateStyleSet(nullptr);
    if (CreateResult.IsFailure())
    {
        return CreateResult;
    }

    auto RegisterResult = RegisterStyleSet();
    if (RegisterResult.IsFailure())
    {
        return RegisterResult;
    }

    return TConvaiResult<void>::Success();
}

TConvaiResult<void> FConvaiStyleRegistry::RegisterColorOverride(const FName &Key, const FLinearColor &Color)
{
    if (!ValidateStyleKey(Key))
    {
        return TConvaiResult<void>::Failure(FString::Printf(TEXT("Invalid style key: %s"), *Key.ToString()));
    }

    FWriteScopeLock Lock(StyleSetLock);
    if (!StyleSet.IsValid())
    {
        return TConvaiResult<void>::Failure(TEXT("Style set not initialized"));
    }

    StyleSet->Set(Key, Color);
    return TConvaiResult<void>::Success();
}

TConvaiResult<void> FConvaiStyleRegistry::RegisterBrushOverride(const FName &Key, const FSlateBrush &Brush)
{
    if (!ValidateStyleKey(Key))
    {
        return TConvaiResult<void>::Failure(FString::Printf(TEXT("Invalid style key: %s"), *Key.ToString()));
    }

    FWriteScopeLock Lock(StyleSetLock);
    if (!StyleSet.IsValid())
    {
        return TConvaiResult<void>::Failure(TEXT("Style set not initialized"));
    }

    StyleSet->Set(Key, new FSlateBrush(Brush));
    return TConvaiResult<void>::Success();
}

TConvaiResult<void> FConvaiStyleRegistry::RegisterFloatOverride(const FName &Key, float Value)
{
    if (!ValidateStyleKey(Key))
    {
        return TConvaiResult<void>::Failure(FString::Printf(TEXT("Invalid style key: %s"), *Key.ToString()));
    }

    FWriteScopeLock Lock(StyleSetLock);
    if (!StyleSet.IsValid())
    {
        return TConvaiResult<void>::Failure(TEXT("Style set not initialized"));
    }

    StyleSet->Set(Key, Value);
    return TConvaiResult<void>::Success();
}

TConvaiResult<void> FConvaiStyleRegistry::RegisterVectorOverride(const FName &Key, const FVector2D &Vector)
{
    if (!ValidateStyleKey(Key))
    {
        return TConvaiResult<void>::Failure(FString::Printf(TEXT("Invalid style key: %s"), *Key.ToString()));
    }

    FWriteScopeLock Lock(StyleSetLock);
    if (!StyleSet.IsValid())
    {
        return TConvaiResult<void>::Failure(TEXT("Style set not initialized"));
    }

    StyleSet->Set(Key, Vector);
    return TConvaiResult<void>::Success();
}

TConvaiResult<void> FConvaiStyleRegistry::CreateStyleSet(const TSharedPtr<FJsonObject> &ThemeJson)
{
    FWriteScopeLock Lock(StyleSetLock);

    auto PinnedThemeManager = ThemeManager.Pin();
    if (!PinnedThemeManager.IsValid())
    {
        return TConvaiResult<void>::Failure(TEXT("ThemeManager is no longer valid"));
    }

    StyleSet = PinnedThemeManager->GetStyle();
    if (!StyleSet.IsValid())
    {
        StyleSet = MakeShared<FSlateStyleSet>(StyleSetName);

        const FString PluginBaseDir = IPluginManager::Get().FindPlugin(TEXT("Convai"))->GetBaseDir();
        const FString ResourceRootPath = FPaths::Combine(PluginBaseDir, ConvaiEditor::Constants::PluginResources::Root);
        StyleSet->SetContentRoot(ResourceRootPath);

        UE_LOG(LogConvaiEditorTheme, Warning, TEXT("Created fallback style set - theme manager returned invalid style"));
    }

    if (!ValidateStyleSet())
    {
        return TConvaiResult<void>::Failure(TEXT("Style set validation failed"));
    }

    return TConvaiResult<void>::Success();
}

TConvaiResult<void> FConvaiStyleRegistry::RegisterStyleSet()
{
    FReadScopeLock Lock(StyleSetLock);

    if (!StyleSet.IsValid())
    {
        return TConvaiResult<void>::Failure(TEXT("Cannot register invalid style set"));
    }

    if (FSlateStyleRegistry::FindSlateStyle(StyleSetName))
    {
        UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyleRegistry: style set already registered: %s"), *StyleSetName.ToString());
        return TConvaiResult<void>::Success();
    }

    FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
    return TConvaiResult<void>::Success();
}

TConvaiResult<void> FConvaiStyleRegistry::UnregisterStyleSet()
{
    if (!FSlateStyleRegistry::FindSlateStyle(StyleSetName))
    {
        return TConvaiResult<void>::Success();
    }

    FSlateStyleRegistry::UnRegisterSlateStyle(StyleSetName);
    return TConvaiResult<void>::Success();
}

void FConvaiStyleRegistry::OnThemeChanged()
{
    auto RefreshResult = RefreshStyleSet();
    if (RefreshResult.IsFailure())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ConvaiStyleRegistry: failed to refresh style set on theme change - %s"), *RefreshResult.GetError());
    }
}

bool FConvaiStyleRegistry::ValidateStyleSet() const
{
    if (!StyleSet.IsValid())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ConvaiStyleRegistry: style set is invalid"));
        return false;
    }

    if (StyleSet->GetStyleSetName() != StyleSetName)
    {
        UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ConvaiStyleRegistry: style set name mismatch: expected %s, got %s"),
               *StyleSetName.ToString(), *StyleSet->GetStyleSetName().ToString());
    }

    return true;
}

bool FConvaiStyleRegistry::ValidateStyleKey(const FName &Key) const
{
    if (Key.IsNone())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ConvaiStyleRegistry: style key cannot be None"));
        return false;
    }

    FString KeyString = Key.ToString();
    if (KeyString.IsEmpty())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ConvaiStyleRegistry: style key cannot be empty"));
        return false;
    }

    return true;
}

void FConvaiStyleRegistry::CleanupResources()
{
    FWriteScopeLock Lock(StyleSetLock);

    ThemeManager.Reset();
    StyleSet.Reset();
}
