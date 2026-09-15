/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * TokenLoaderBase.h
 *
 * Base class for token loaders with key construction utilities.
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Base class for token loaders with key construction utilities.
 */
class FTokenLoaderBase
{
protected:
    /** Builds a standardized key for the style set */
    static FString BuildKey(const TCHAR *Prefix, const FString &PathSegment)
    {
        return FString::Printf(TEXT("Convai.%s.%s"), Prefix, *PathSegment);
    }
};
