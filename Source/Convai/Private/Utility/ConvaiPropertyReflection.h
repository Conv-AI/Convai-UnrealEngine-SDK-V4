// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"

/**
 * Internal helper used by UConvaiObjectComponent to resolve tracked-property
 * paths on the owning Actor and format their current values to strings for
 * SetContextState broadcasts.
 *
 * Two leaf kinds are supported:
 *   - A leaf UPROPERTY: bool / numeric / string / name / text / enum, the
 *     value-like structs FVector / FRotator / FVector2D / FQuat / FTransform,
 *     or arrays / sets / maps of any of those. Multi-token paths walk a chain
 *     of struct members to a leaf of any of these types.
 *   - A pure (BlueprintPure / Const), parameter-less UFunction whose return
 *     type is itself a supported leaf — addressed by name directly on the
 *     owning Actor's class. Impure functions, arg-taking functions, and
 *     functions with unsupported return types are rejected at the runtime
 *     resolver as well as the editor picker.
 */
struct FConvaiResolvedProperty
{
	FProperty* Leaf = nullptr;
	void* LeafContainer = nullptr;
	/** When non-null, the leaf is a parameter-less UFunction on FunctionTarget
	 *  (with a supported-leaf return type) instead of a UPROPERTY. FormatValue
	 *  invokes it and formats the return value. */
	UFunction* LeafFunction = nullptr;
	UObject* FunctionTarget = nullptr;
	bool bSupported = false;
};

namespace ConvaiPropertyReflection
{
	/** Returns true if Prop's leaf-type is one we can read into a string for broadcast.
	 *  For container properties (array / set / map), inner / key / value types must
	 *  themselves be supported. */
	bool IsSupportedLeaf(const FProperty* Prop);

	/**
	 * Resolves a dotted path on Root.
	 *   - Single token: tries a UPROPERTY first, then a pure parameter-less
	 *     UFunction whose return type is a supported leaf.
	 *   - Multi token: walks chain of struct members to a leaf UPROPERTY.
	 * Sets Out.* and Out.bSupported. Returns true if resolved (regardless of bSupported).
	 */
	bool ResolvePath(UObject* Root, FName DottedPath, FConvaiResolvedProperty& Out);

	/** Reads the leaf value (property OR function return) into a string for broadcast. */
	FString FormatValue(const FConvaiResolvedProperty& Resolved);
}
