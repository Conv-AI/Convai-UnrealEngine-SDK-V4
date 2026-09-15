/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IConvaiStyleRegistry.h
 *
 * Interface for managing Slate style registration and lifecycle.
 */

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"
#include "Templates/SharedPointer.h"
#include "ConvaiEditor.h"
#include "Services/ConvaiDIContainer.h"

class FJsonObject;

/** Interface for Slate style registry management. */
class CONVAIEDITOR_API IConvaiStyleRegistry : public IConvaiService
{
public:
    virtual ~IConvaiStyleRegistry() = default;

    static FName StaticType() { return TEXT("IConvaiStyleRegistry"); }

    virtual TConvaiResult<void> InitializeStyleRegistry(const TSharedPtr<FJsonObject> &ThemeJson = nullptr) = 0;
    virtual TConvaiResult<void> ShutdownStyleRegistry() = 0;
    virtual TSharedPtr<FSlateStyleSet> GetStyleSet() const = 0;
    virtual TSharedPtr<FSlateStyleSet> GetMutableStyleSet() = 0;
    virtual bool IsInitialized() const = 0;
    virtual FName GetStyleSetName() const = 0;
    virtual TConvaiResult<void> RefreshStyleSet() = 0;
    virtual TConvaiResult<void> RegisterColorOverride(const FName &Key, const FLinearColor &Color) = 0;
    virtual TConvaiResult<void> RegisterFloatOverride(const FName &Key, float Value) = 0;
    virtual TConvaiResult<void> RegisterVectorOverride(const FName &Key, const FVector2D &Vector) = 0;
    virtual TConvaiResult<void> RegisterBrushOverride(const FName &Key, const FSlateBrush &Brush) = 0;

protected:
    virtual bool ValidateStyleKey(const FName &Key) const = 0;
    virtual bool ValidateStyleSet() const = 0;
};

using IConvaiStyleRegistryService = IConvaiStyleRegistry;
