/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IThemeProvider.h
 *
 * Interface for theme management.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/ConvaiDIContainer.h"

/**
 * Interface for theme management.
 */
class CONVAIEDITOR_API IThemeProvider : public IConvaiService
{
public:
    virtual ~IThemeProvider() = default;

    virtual FString GetThemeId() const = 0;
    virtual void SetThemeId(const FString &InThemeId) = 0;

    static FName StaticType() { return TEXT("IThemeProvider"); }
};
