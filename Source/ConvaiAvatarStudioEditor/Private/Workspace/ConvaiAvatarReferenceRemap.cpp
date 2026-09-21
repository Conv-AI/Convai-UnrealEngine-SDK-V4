// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarReferenceRemap.h"

#include "NiagaraParameterStore.h"
#include "NiagaraTypes.h"
#include "UObject/UnrealType.h"
#include "Misc/EngineVersionComparison.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#endif

namespace ConvaiAvatarReferenceRemap
{
namespace
{
	/** Some native serializers (notably FGroomDataflowSettings) only visit their
	 * UPROPERTYs when loading/saving, so FArchiveReplaceObjectRef cannot see them.
	 * Walk persisted properties directly without serializing native caches or
	 * following any referenced object back into the creator's source packages. */
	struct FNativeReferenceRemapper
	{
		const TMap<UObject*, UObject*>& Replacements;
		TMap<const FProperty*, bool> RelevantProperties;
		TSet<const UStruct*> InspectingStructs;
		TFunction<void(const FProperty*, const FString&)> Observe;

		bool Relevant(const FProperty* Property)
		{
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_SkipSerialization)) return false;
			if (const bool* Found = RelevantProperties.Find(Property)) return *Found;
			// Soft paths already pass through AssetTools' saving-mode rename archive.
			// Do not resolve them or interpret plain strings as object references.
			bool bRelevant = (Observe || !CastField<FSoftObjectProperty>(Property)) &&
				(CastField<FObjectPropertyBase>(Property) || CastField<FInterfaceProperty>(Property));
			if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
			{
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
				bRelevant = Struct->Struct == FInstancedStruct::StaticStruct() || Struct->Struct == FInstancedPropertyBag::StaticStruct();
#endif
				if (!bRelevant)
				{
					if (InspectingStructs.Contains(Struct->Struct)) return true;
					InspectingStructs.Add(Struct->Struct);
					for (TFieldIterator<FProperty> It(Struct->Struct); It && !bRelevant; ++It) bRelevant = Relevant(*It);
					InspectingStructs.Remove(Struct->Struct);
				}
			}
			else if (const FArrayProperty* Array = CastField<FArrayProperty>(Property)) bRelevant = Relevant(Array->Inner);
			else if (const FMapProperty* Map = CastField<FMapProperty>(Property)) bRelevant = Relevant(Map->KeyProp) || Relevant(Map->ValueProp);
			else if (const FSetProperty* Set = CastField<FSetProperty>(Property)) bRelevant = Relevant(Set->ElementProp);
			RelevantProperties.Add(Property, bRelevant);
			return bRelevant;
		}

		int32 StructValue(const UStruct* Type, void* Data)
		{
			if (!Type || !Data) return 0;
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
			if (Type == FInstancedStruct::StaticStruct())
			{
				FInstancedStruct& Value = *static_cast<FInstancedStruct*>(Data);
				return StructValue(Value.GetScriptStruct(), Value.GetMutableMemory());
			}
			if (Type == FInstancedPropertyBag::StaticStruct())
			{
				FStructView Value = static_cast<FInstancedPropertyBag*>(Data)->GetMutableValue();
				return StructValue(Value.GetScriptStruct(), Value.GetMemory());
			}
#endif
			int32 Changed = 0;
			for (TFieldIterator<FProperty> It(Type); It; ++It)
			{
				if (!Relevant(*It)) continue;
				for (int32 Index = 0; Index < It->ArrayDim; ++Index) Changed += PropertyValue(*It, It->ContainerPtrToValuePtr<void>(Data, Index));
			}
			return Changed;
		}

		int32 PropertyValue(FProperty* Property, void* Data)
		{
			if (!Relevant(Property)) return 0;
			if (FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
			{
				// Inspection reads the stored path only. Normal remapping keeps using AssetTools.
				if (Observe) Observe(Property, SoftProperty->GetPropertyValue(Data).ToSoftObjectPath().ToString());
				return 0;
			}
			if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				UObject* Source = ObjectProperty->GetObjectPropertyValue(Data);
				if (Observe && Source) Observe(Property, Source->GetPathName());
				if (UObject* const* Target = Replacements.Find(Source); Target && *Target != Source)
				{
					ObjectProperty->SetObjectPropertyValue(Data, *Target);
					return 1;
				}
				return 0;
			}
			if (FInterfaceProperty* InterfaceProperty = CastField<FInterfaceProperty>(Property))
			{
				FScriptInterface& Value = *static_cast<FScriptInterface*>(Data);
				if (Observe && Value.GetObject()) Observe(Property, Value.GetObject()->GetPathName());
				if (UObject* const* Target = Replacements.Find(Value.GetObject()); Target && *Target != Value.GetObject())
				{
					Value.SetObject(*Target);
					Value.SetInterface(*Target ? (*Target)->GetInterfaceAddress(InterfaceProperty->InterfaceClass) : nullptr);
					return 1;
				}
				return 0;
			}
			if (FStructProperty* Struct = CastField<FStructProperty>(Property)) return StructValue(Struct->Struct, Data);
			int32 Changed = 0;
			if (FArrayProperty* Array = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper Values(Array, Data);
				for (int32 Index = 0; Index < Values.Num(); ++Index) Changed += PropertyValue(Array->Inner, Values.GetRawPtr(Index));
			}
			else if (FMapProperty* Map = CastField<FMapProperty>(Property))
			{
				FScriptMapHelper Values(Map, Data);
				bool bKeysChanged = false;
				for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index)
				{
					if (!Values.IsValidIndex(Index)) continue;
					const int32 KeyChanges = PropertyValue(Map->KeyProp, Values.GetKeyPtr(Index));
					bKeysChanged |= KeyChanges > 0;
					Changed += KeyChanges + PropertyValue(Map->ValueProp, Values.GetValuePtr(Index));
				}
				if (bKeysChanged) Values.Rehash();
			}
			else if (FSetProperty* Set = CastField<FSetProperty>(Property))
			{
				FScriptSetHelper Values(Set, Data);
				for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index)
					if (Values.IsValidIndex(Index)) Changed += PropertyValue(Set->ElementProp, Values.GetElementPtr(Index));
				if (Changed) Values.Rehash();
			}
			return Changed;
		}
	};

	struct FRemapper
	{
		const TMap<UObject*, UObject*>& Replacements;
		TMap<const FProperty*, bool> RelevantProperties;
		TSet<const UStruct*> InspectingStructs;

		bool IsRelevant(const FProperty* Property)
		{
			if (const bool* Cached = RelevantProperties.Find(Property)) return *Cached;
			bool bRelevant = false;
			if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
			{
				bRelevant = Struct->Struct == FNiagaraTypeDefinitionHandle::StaticStruct() || Struct->Struct == FNiagaraTypeDefinition::StaticStruct();
				if (!bRelevant)
				{
					// Recursive schemas are conservatively visited; UObject pointers are never followed.
					if (InspectingStructs.Contains(Struct->Struct)) return true;
					InspectingStructs.Add(Struct->Struct);
					for (TFieldIterator<FProperty> It(Struct->Struct); It && !bRelevant; ++It) bRelevant = IsRelevant(*It);
					InspectingStructs.Remove(Struct->Struct);
				}
			}
			else if (const FArrayProperty* Array = CastField<FArrayProperty>(Property)) bRelevant = IsRelevant(Array->Inner);
			else if (const FMapProperty* Map = CastField<FMapProperty>(Property)) bRelevant = IsRelevant(Map->KeyProp) || IsRelevant(Map->ValueProp);
			else if (const FSetProperty* Set = CastField<FSetProperty>(Property)) bRelevant = IsRelevant(Set->ElementProp);
			RelevantProperties.Add(Property, bRelevant);
			return bRelevant;
		}

		bool RemapType(FNiagaraTypeDefinition& Type)
		{
			if (!Type.IsValid()) return false;
			UObject* Source = Type.IsEnum() ? static_cast<UObject*>(Type.GetEnum()) : static_cast<UObject*>(Type.GetStruct());
			UObject* const* Replacement = Replacements.Find(Source);
			if (!Replacement || !*Replacement || *Replacement == Source) return false;
			FNiagaraTypeDefinition NewType;
			if (UEnum* Enum = Cast<UEnum>(*Replacement)) NewType = FNiagaraTypeDefinition(Enum);
			else if (UScriptStruct* Struct = Cast<UScriptStruct>(*Replacement)) NewType = FNiagaraTypeDefinition(Struct, FNiagaraTypeDefinition::EAllowUnfriendlyStruct::Allow);
			else if (UClass* Class = Cast<UClass>(*Replacement)) NewType = FNiagaraTypeDefinition(Class);
			else return false;
			NewType.SetFlags(static_cast<FNiagaraTypeDefinition::FTypeFlags>(Type.GetFlags()));
			Type = MoveTemp(NewType); // Rebuild path/hash/layout caches for the copied type.
			return true;
		}

		int32 StructValue(UStruct* Type, void* Data)
		{
			if (Type == FNiagaraTypeDefinitionHandle::StaticStruct())
			{
				FNiagaraTypeDefinitionHandle& Handle = *static_cast<FNiagaraTypeDefinitionHandle*>(Data);
				FNiagaraTypeDefinition LocalType = *Handle;
				if (!RemapType(LocalType)) return 0;
				// UE 5.8's handle serializer ignores reference collectors. Register a new local
				// handle instead of changing the registry entry shared with the creator's source.
				Handle = FNiagaraTypeDefinitionHandle(LocalType);
				return 1;
			}
			if (Type == FNiagaraTypeDefinition::StaticStruct()) return RemapType(*static_cast<FNiagaraTypeDefinition*>(Data)) ? 1 : 0;
			int32 Changed = 0;
			for (TFieldIterator<FProperty> It(Type); It; ++It)
			{
				if (!IsRelevant(*It)) continue;
				for (int32 Index = 0; Index < It->ArrayDim; ++Index) Changed += PropertyValue(*It, It->ContainerPtrToValuePtr<void>(Data, Index));
			}
			if (Changed && Type->IsChildOf(FNiagaraParameterStore::StaticStruct())) static_cast<FNiagaraParameterStore*>(Data)->SortParameters();
			return Changed;
		}

		int32 PropertyValue(FProperty* Property, void* Data)
		{
			if (!IsRelevant(Property)) return 0;
			if (FStructProperty* Struct = CastField<FStructProperty>(Property)) return StructValue(Struct->Struct, Data);
			int32 Changed = 0;
			if (FArrayProperty* Array = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper Values(Array, Data);
				for (int32 Index = 0; Index < Values.Num(); ++Index) Changed += PropertyValue(Array->Inner, Values.GetRawPtr(Index));
			}
			else if (FMapProperty* Map = CastField<FMapProperty>(Property))
			{
				FScriptMapHelper Values(Map, Data);
				bool bKeysChanged = false;
				for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index)
				{
					if (!Values.IsValidIndex(Index)) continue;
					const int32 KeysChanged = PropertyValue(Map->KeyProp, Values.GetKeyPtr(Index));
					bKeysChanged |= KeysChanged > 0;
					Changed += KeysChanged + PropertyValue(Map->ValueProp, Values.GetValuePtr(Index));
				}
				if (bKeysChanged) Values.Rehash();
			}
			else if (FSetProperty* Set = CastField<FSetProperty>(Property))
			{
				FScriptSetHelper Values(Set, Data);
				for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index)
				{
					if (Values.IsValidIndex(Index)) Changed += PropertyValue(Set->ElementProp, Values.GetElementPtr(Index));
				}
				if (Changed) Values.Rehash();
			}
			return Changed;
		}
	};
}

int32 RemapNativeReferences(UObject* Destination, const TMap<UObject*, UObject*>& Replacements)
{
	if (!Destination || Replacements.IsEmpty() || Replacements.Contains(Destination)) return 0;
	FNativeReferenceRemapper Remapper{Replacements, {}, {}, {}};
	return Remapper.StructValue(Destination->GetClass(), Destination);
}

void DescribeReferencesToPackage(UObject* Destination, FName SourcePackage, int32 MaxDetails, TArray<FString>& OutDetails)
{
	if (!Destination || OutDetails.Num() >= MaxDetails) return;
	const FString Source = SourcePackage.ToString();
	const FString Owner = Destination->GetPathName();
	const FString OwnerClass = Destination->GetClass()->GetPathName();
	auto Record = [&](const FString& Field, const FString& Reference)
	{
		if (OutDetails.Num() >= MaxDetails ||
			!(Reference.Equals(Source, ESearchCase::IgnoreCase) || Reference.StartsWith(Source + TEXT("."), ESearchCase::IgnoreCase))) return;
		OutDetails.AddUnique(FString::Printf(TEXT("owner=%s; field=%s; reference=%s; owner_class=%s"), *Owner, *Field, *Reference, *OwnerClass));
	};
	Record(TEXT("UObject.Class"), OwnerClass);
	const TMap<UObject*, UObject*> NoReplacements;
	FNativeReferenceRemapper Inspector{NoReplacements, {}, {},
		[&](const FProperty* Property, const FString& Reference) { Record(Property->GetPathName(), Reference); }};
	Inspector.StructValue(Destination->GetClass(), Destination);
}

int32 RemapNiagaraTypes(UStruct* Type, void* Data, const TMap<UObject*, UObject*>& Replacements)
{
	FRemapper Remapper{Replacements};
	return Type && Data ? Remapper.StructValue(Type, Data) : 0;
}
}
