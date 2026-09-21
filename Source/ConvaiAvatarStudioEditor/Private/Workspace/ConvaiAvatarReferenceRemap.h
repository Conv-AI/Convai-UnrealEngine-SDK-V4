// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace ConvaiAvatarReferenceRemap
{
	/** Rewrite reflected Niagara type handles in owned destination memory, never the shared type registry. */
	int32 RemapNiagaraTypes(UStruct* Type, void* Data, const TMap<UObject*, UObject*>& Replacements);
	/** Replace persisted reflected references skipped by native serializers.
	 * Referenced UObjects are replaced but never traversed; outer/archetype pointers and arbitrary strings are untouched. */
	int32 RemapNativeReferences(UObject* Destination, const TMap<UObject*, UObject*>& Replacements);
	/** Read-only diagnostics for one already-owned package that escaped remapping.
	 * Never resolves soft references or follows referenced UObjects; appends at most MaxDetails total. */
	void DescribeReferencesToPackage(UObject* Destination, FName SourcePackage, int32 MaxDetails, TArray<FString>& OutDetails);
}
