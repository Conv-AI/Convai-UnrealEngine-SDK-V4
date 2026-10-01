/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiStyleRegistry.h
 *
 * Thread-safe management of Slate style sets with theme integration.
 */

#pragma once

#include "CoreMinimal.h"
#include "Styling/IConvaiStyleRegistry.h"
#include "Styling/SlateStyleRegistry.h"
#include "Templates/SharedPointer.h"
#include "HAL/CriticalSection.h"

class FJsonObject;
class IThemeManager;

/**
 * Thread-safe management of Slate style sets with theme integration.
 */
class FConvaiStyleRegistry : public IConvaiStyleRegistry
{
public:
    FConvaiStyleRegistry();
    virtual ~FConvaiStyleRegistry();

    /** Returns the service type name for DI container registration */
    static FName StaticType() { return TEXT("IConvaiStyleRegistry"); }

    /** Initializes the service and resolves dependencies */
    virtual void Startup() override;

    /** Cleans up resources before shutdown */
    virtual void Shutdown() override;

    /** Initializes the style registry with optional theme data */
    virtual TConvaiResult<void> InitializeStyleRegistry(const TSharedPtr<FJsonObject> &ThemeJson = nullptr) override;

    /** Shuts down the style registry and unregisters the style set */
    virtual TConvaiResult<void> ShutdownStyleRegistry() override;

    /** Returns the current style set */
    virtual TSharedPtr<FSlateStyleSet> GetStyleSet() const override;

    /** Returns the mutable style set for modifications */
    virtual TSharedPtr<FSlateStyleSet> GetMutableStyleSet() override;

    /** Checks if the registry is initialized */
    virtual bool IsInitialized() const override;

    /** Returns the style set name */
    virtual FName GetStyleSetName() const override;

    /** Refreshes the style set with current theme data */
    virtual TConvaiResult<void> RefreshStyleSet() override;

    /** Registers a color override in the style set */
    virtual TConvaiResult<void> RegisterColorOverride(const FName &Key, const FLinearColor &Color) override;

    /** Registers a float value override in the style set */
    virtual TConvaiResult<void> RegisterFloatOverride(const FName &Key, float Value) override;

    /** Registers a vector override in the style set */
    virtual TConvaiResult<void> RegisterVectorOverride(const FName &Key, const FVector2D &Vector) override;

    /** Registers a brush override in the style set */
    virtual TConvaiResult<void> RegisterBrushOverride(const FName &Key, const FSlateBrush &Brush) override;

protected:
    /** Validates a style key before registration */
    virtual bool ValidateStyleKey(const FName &Key) const override;

    /** Validates the style set integrity */
    virtual bool ValidateStyleSet() const override;

private:
    mutable FRWLock StyleSetLock;
    TSharedPtr<FSlateStyleSet> StyleSet;
    TAtomic<bool> bInitialized;
    static const FName StyleSetName;
    TWeakPtr<IThemeManager> ThemeManager;

    TConvaiResult<void> CreateStyleSet(const TSharedPtr<FJsonObject> &ThemeJson);
    TConvaiResult<void> RegisterStyleSet();
    TConvaiResult<void> UnregisterStyleSet();
    void OnThemeChanged();
    void CleanupResources();
};
