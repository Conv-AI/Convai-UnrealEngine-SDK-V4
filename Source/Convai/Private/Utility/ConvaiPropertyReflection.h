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
 *     or arrays / sets / maps of any of those. Multi-token paths walk struct
 *     members, and may hop across object references to sub-actors, components
 *     and instanced subobjects (hops are IsValid-checked on every resolution,
 *     so a null hop just fails resolution until it becomes valid).
 *   - A pure (BlueprintPure / Const), parameter-less UFunction whose return
 *     type is itself a supported leaf — on the root or on the last hopped-to
 *     object.
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
	/** Hard cap on path segments — guards against runaway hand-typed paths. */
	constexpr int32 MaxPathSegments = 16;

	/** Returns true if Prop's leaf-type is one we can read into a string for broadcast.
	 *  For container properties (array / set / map), inner / key / value types must
	 *  themselves be supported. */
	bool IsSupportedLeaf(const FProperty* Prop);

	/** Resolves a dotted path on Root (see file header for the path rules).
	 *  Sets Out.* and Out.bSupported. Returns true if resolved (regardless of bSupported). */
	bool ResolvePath(UObject* Root, FName DottedPath, FConvaiResolvedProperty& Out);

	/** Reads the leaf value (property OR function return) into a string for broadcast. */
	FString FormatValue(const FConvaiResolvedProperty& Resolved);

#if WITH_EDITOR
	/**
	 * Best-effort Blueprint-variable rename healing: re-walks DottedPath against
	 * Root's current layout; segments that no longer resolve by name are
	 * re-matched by their stored GUID (InOutSegmentGuids, parallel to the path)
	 * and renamed in place, and GUIDs are backfilled for segments that do
	 * resolve. Covers BP class variables and SCS component variables — not
	 * native properties or function names. Returns true when OutHealedPath
	 * differs from DottedPath.
	 */
	bool HealPath(UObject* Root, FName DottedPath, TArray<FGuid>& InOutSegmentGuids, FName& OutHealedPath);
#endif
}
