// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
class FJsonObject;
namespace ConvaiAvatarNativeRuntimeContent
{
    const FString& BuiltinGroomContract();
    bool IsBuiltinGroomRoot(const FString& Package);
    bool IsValidRequirement(const FString& Contract);
    bool Supports(const FString& Contract);
    /** Portable source recognition grants no publishing compatibility. */
    bool IsSharedBuiltinGroomRoot(const FString& Package, const FString& Contract, bool bPortableSourceImport = false);
    /** Operator-owned declaration for this exact product target, engine and platform. */
    FString ReadDeclaration(const TSharedPtr<FJsonObject>& Version, const FString& AssetsRoot);
    bool CheckTarget(const FString& Required, const FString& Available, bool bIncludesLinux, FString& Error);
}
