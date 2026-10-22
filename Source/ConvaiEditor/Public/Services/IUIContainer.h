/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IUIContainer.h
 *
 * Interface for UI page container management.
 */

#pragma once

#include "CoreMinimal.h"

// Forward declarations
class SWidget;

/**
 * Interface for UI page container management.
 */
class CONVAIEDITOR_API IUIContainer
{
public:
    virtual ~IUIContainer() = default;

    /** Adds page to container */
    virtual int32 AddPage(TSharedRef<SWidget> Content) = 0;

    /** Shows page by index */
    virtual void ShowPage(int32 PageIndex) = 0;

    /** Returns true if container is valid */
    virtual bool IsValid() const = 0;

    /** Returns total page count */
    virtual int32 GetPageCount() const = 0;

    /** Returns page widget by index */
    virtual TSharedPtr<SWidget> GetPage(int32 PageIndex) const = 0;
};
