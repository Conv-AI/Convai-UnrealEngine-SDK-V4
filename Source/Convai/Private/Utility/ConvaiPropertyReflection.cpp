// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiPropertyReflection.h"

#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/Class.h"
#include "UObject/TextProperty.h"
#include "UObject/Field.h"
#include "HAL/UnrealMemory.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Runtime/Launch/Resources/Version.h"

#if WITH_EDITOR
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#endif

// FStrProperty moved out of UnrealType.h into its own header in UE 5.5.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#include "UObject/StrProperty.h"
#endif

// ExportTextItem_Direct (non-virtual) was added in UE 5.2. The legacy virtual
// ExportTextItem was removed in UE 5.7. Pick the one that exists on this engine.
#define CONVAI_HAS_EXPORTTEXTITEM_DIRECT (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2))

namespace
{
	constexpr int32 kMaxCollectionPreviewElements = 3;

	bool IsRejectedReferenceProperty(const FProperty* Prop)
	{
		return Prop->IsA(FObjectPropertyBase::StaticClass())
			|| Prop->IsA(FInterfaceProperty::StaticClass())
			|| Prop->IsA(FClassProperty::StaticClass())
			|| Prop->IsA(FDelegateProperty::StaticClass())
			|| Prop->IsA(FMulticastDelegateProperty::StaticClass());
	}

	// Tiny "value-like" structs we accept as leaves; everything else has to be drilled into.
	// Kept in sync with the editor picker filter (ConvaiTrackedPropertyCustomization.cpp).
	bool IsAllowedStructLeaf(const UStruct* Struct)
	{
		if (!Struct) return false;
		return Struct == TBaseStructure<FVector>::Get()
			|| Struct == TBaseStructure<FRotator>::Get()
			|| Struct == TBaseStructure<FVector2D>::Get()
			|| Struct == TBaseStructure<FQuat>::Get()
			|| Struct == TBaseStructure<FTransform>::Get();
	}

	// Format a single leaf value at ValuePtr through Prop, without recursing
	// into containers (used for individual elements of arrays/sets/map keys/values).
	FString FormatScalar(const FProperty* Prop, const void* ValuePtr)
	{
		if (!Prop || !ValuePtr)
		{
			return TEXT("<null>");
		}

		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			return BoolProp->GetPropertyValue(ValuePtr) ? TEXT("true") : TEXT("false");
		}

		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			const int64 Value = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				return Enum->GetNameStringByValue(Value);
			}
			return LexToString(Value);
		}

		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			const int64 Value = ByteProp->GetSignedIntPropertyValue(ValuePtr);
			if (UEnum* Enum = ByteProp->Enum)
			{
				return Enum->GetNameStringByValue(Value);
			}
			return LexToString(Value);
		}

		if (const FNumericProperty* NumericProp = CastField<FNumericProperty>(Prop))
		{
			return NumericProp->GetNumericPropertyValueToString(const_cast<void*>(ValuePtr));
		}

		if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			return StrProp->GetPropertyValue(ValuePtr);
		}

		if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			return NameProp->GetPropertyValue(ValuePtr).ToString();
		}

		if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
		{
			return TextProp->GetPropertyValue(ValuePtr).ToString();
		}

		if (IsRejectedReferenceProperty(Prop))
		{
			return TEXT("<unsupported>");
		}

		// Fallback for anything else (defensive — shouldn't trigger for filtered leaves).
		// ExportTextItem_Direct is 5.2+; the virtual ExportTextItem was removed in 5.7.
		FString Out;
#if CONVAI_HAS_EXPORTTEXTITEM_DIRECT
		Prop->ExportTextItem_Direct(Out, ValuePtr, /*Default*/ nullptr, /*Parent*/ nullptr, PPF_None);
#else
		Prop->ExportTextItem(Out, ValuePtr, /*Default*/ nullptr, /*Parent*/ nullptr, PPF_None);
#endif
		return Out;
	}

	// True when the only parm-flagged property is the return value — i.e. the
	// function takes no inputs. The editor picker enforces this too; the runtime
	// check covers hand-typed paths (invoking an arg-taking function with
	// zero-initialized inputs every poll would be silently wrong).
	bool IsParameterless(const UFunction* Function)
	{
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm) && *It != Function->GetReturnProperty())
			{
				return false;
			}
		}
		return true;
	}

	// Invoke a parameter-less UFunction on Target and format its return value using
	// the same FormatScalar dispatcher used for property leaves. Return type can be
	// any of the supported leaf kinds (FString / FName / FText / bool / numeric / enum /
	// whitelisted struct).
	FString InvokeFunctionAndFormatReturn(UObject* Target, UFunction* Function)
	{
		if (!IsValid(Target) || !Function)
		{
			return TEXT("<unsupported>");
		}
		FProperty* ReturnProp = Function->GetReturnProperty();
		if (!ReturnProp)
		{
			return TEXT("<unsupported>");
		}

		const SIZE_T ParmsSize = Function->ParmsSize;
		void* Parms = ParmsSize > 0 ? FMemory_Alloca(ParmsSize) : nullptr;
		if (Parms)
		{
			FMemory::Memzero(Parms, ParmsSize);
			// Initialize parm-flagged properties so destructors are safe.
			for (TFieldIterator<FProperty> It(Function); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Parm))
				{
					It->InitializeValue_InContainer(Parms);
				}
			}
		}

		Target->ProcessEvent(Function, Parms);

		FString Result;
		if (Parms)
		{
			Result = FormatScalar(ReturnProp, ReturnProp->ContainerPtrToValuePtr<void>(Parms));
			for (TFieldIterator<FProperty> It(Function); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Parm))
				{
					It->DestroyValue_InContainer(Parms);
				}
			}
		}
		return Result;
	}
}

bool ConvaiPropertyReflection::IsSupportedLeaf(const FProperty* Prop)
{
	if (!Prop)
	{
		return false;
	}
	if (IsRejectedReferenceProperty(Prop))
	{
		return false;
	}
	// Structs are only allowed as leaves if they're in the small "value-like" whitelist
	// (FVector / FRotator / FVector2D / FQuat / FTransform) — everything else has to be
	// drilled into for a specific member.
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		return IsAllowedStructLeaf(StructProp->Struct);
	}
	// Containers are only supported when their inner / key / value types are themselves
	// supported leaves. Otherwise something like TArray<UObject*> slips past as a
	// container we'd then format as "<unsupported>" elements at runtime, which
	// undermines the no-object-refs policy.
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		return ArrayProp->Inner && IsSupportedLeaf(ArrayProp->Inner);
	}
	if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		return SetProp->ElementProp && IsSupportedLeaf(SetProp->ElementProp);
	}
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		return MapProp->KeyProp && MapProp->ValueProp
			&& IsSupportedLeaf(MapProp->KeyProp)
			&& IsSupportedLeaf(MapProp->ValueProp);
	}
	return Prop->IsA(FBoolProperty::StaticClass())
		|| Prop->IsA(FNumericProperty::StaticClass())
		|| Prop->IsA(FStrProperty::StaticClass())
		|| Prop->IsA(FNameProperty::StaticClass())
		|| Prop->IsA(FTextProperty::StaticClass())
		|| Prop->IsA(FEnumProperty::StaticClass())
		|| Prop->IsA(FByteProperty::StaticClass());
}

// Object references an intermediate path segment may hop across: hard/weak refs
// to sub-actors, components, or instanced (EditInlineNew) subobjects. Soft refs
// are excluded (could trigger asset loads from the poll), class refs and
// arbitrary UObject refs (materials, meshes, engine internals) stay closed.
// Kept in sync with the editor picker copy (ConvaiTrackedPropertyCustomization.cpp).
static bool IsTraversableObjectProperty(const FProperty* Prop)
{
	const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop);
	if (!ObjProp || !ObjProp->PropertyClass
		|| Prop->IsA(FSoftObjectProperty::StaticClass())
		|| Prop->IsA(FClassProperty::StaticClass()))
	{
		return false;
	}
	return Prop->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance)
		|| ObjProp->PropertyClass->IsChildOf(AActor::StaticClass())
		|| ObjProp->PropertyClass->IsChildOf(UActorComponent::StaticClass());
}

bool ConvaiPropertyReflection::ResolvePath(UObject* Root, FName DottedPath, FConvaiResolvedProperty& Out)
{
	Out = FConvaiResolvedProperty{};

	if (!Root || DottedPath.IsNone())
	{
		return false;
	}

	const FString PathString = DottedPath.ToString();
	TArray<FString> Tokens;
	PathString.ParseIntoArray(Tokens, TEXT("."), /*CullEmpty*/ true);
	if (Tokens.Num() == 0 || Tokens.Num() > MaxPathSegments)
	{
		return false;
	}

	// CurrentObject is non-null only while the walk sits directly ON an object
	// (the Root, or the target of the last object hop) — that's the only place a
	// function leaf can live (functions belong to UClass, not to inner structs).
	UObject* CurrentObject = Root;
	UStruct* CurrentStruct = Root->GetClass();
	void* CurrentContainer = Root;

	for (int32 i = 0; i < Tokens.Num(); ++i)
	{
		const FName TokenName(*Tokens[i]);
		const bool bLastToken = (i + 1 == Tokens.Num());
		FProperty* Found = CurrentStruct ? CurrentStruct->FindPropertyByName(TokenName) : nullptr;

		// Function leaf: final token with no same-named property, sitting on an
		// object. The picker only offers pure, parameter-less functions whose
		// return is a supported leaf type — runtime mirrors that check as defense
		// in depth (a user could otherwise hand-type a non-pure function name and
		// have it invoked every poll tick).
		if (!Found && bLastToken && CurrentObject)
		{
			if (UFunction* Func = CurrentObject->FindFunction(TokenName))
			{
				const bool bIsPure = Func->HasAnyFunctionFlags(FUNC_BlueprintPure | FUNC_Const);
				if (bIsPure && IsParameterless(Func) && IsSupportedLeaf(Func->GetReturnProperty()))
				{
					Out.LeafFunction = Func;
					Out.FunctionTarget = CurrentObject;
					Out.bSupported = true;
					return true;
				}
			}
			return false;
		}
		if (!Found)
		{
			return false;
		}

		if (bLastToken)
		{
			Out.Leaf = Found;
			Out.LeafContainer = CurrentContainer;
			Out.bSupported = IsSupportedLeaf(Found);
			return true;
		}

		// Intermediate segment: struct member drill-down, or object hop.
		if (FStructProperty* StructProp = CastField<FStructProperty>(Found))
		{
			if (!StructProp->Struct)
			{
				return false;
			}
			CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
			CurrentStruct = StructProp->Struct;
			CurrentObject = nullptr; // inside a struct now — no function leaves past this point
			continue;
		}
		if (IsTraversableObjectProperty(Found))
		{
			const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Found);
			UObject* Next = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CurrentContainer));
			if (!IsValid(Next))
			{
				// Null / pending-kill hop: fail resolution now; the poll retries,
				// so tracking resumes the moment the reference becomes valid.
				return false;
			}
			CurrentObject = Next;
			CurrentStruct = Next->GetClass(); // runtime class, so BP-added members resolve
			CurrentContainer = Next;
			continue;
		}
		return false;
	}

	return false; // unreachable — every iteration returns or continues
}

FString ConvaiPropertyReflection::FormatValue(const FConvaiResolvedProperty& Resolved)
{
	// Function leaf: invoke and format the return value.
	if (Resolved.LeafFunction)
	{
		return InvokeFunctionAndFormatReturn(Resolved.FunctionTarget, Resolved.LeafFunction);
	}

	if (!Resolved.Leaf || !Resolved.LeafContainer)
	{
		return TEXT("<null>");
	}

	const FProperty* Prop = Resolved.Leaf;
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Resolved.LeafContainer);

	// Containers — count + first N elements.
	if (const FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper Helper(ArrProp, ValuePtr);
		const int32 Num = Helper.Num();
		FString Preview;
		const int32 Take = FMath::Min(Num, kMaxCollectionPreviewElements);
		for (int32 i = 0; i < Take; ++i)
		{
			if (i > 0) Preview += TEXT(", ");
			Preview += FormatScalar(ArrProp->Inner, Helper.GetRawPtr(i));
		}
		return FString::Printf(TEXT("%d elements: [%s]"), Num, *Preview);
	}

	if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		FScriptSetHelper Helper(SetProp, ValuePtr);
		const int32 Num = Helper.Num();
		FString Preview;
		int32 Emitted = 0;
		for (int32 i = 0; i < Helper.GetMaxIndex() && Emitted < kMaxCollectionPreviewElements; ++i)
		{
			if (!Helper.IsValidIndex(i)) continue;
			if (Emitted > 0) Preview += TEXT(", ");
			Preview += FormatScalar(Helper.GetElementProperty(), Helper.GetElementPtr(i));
			++Emitted;
		}
		return FString::Printf(TEXT("%d elements: [%s]"), Num, *Preview);
	}

	if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		FScriptMapHelper Helper(MapProp, ValuePtr);
		const int32 Num = Helper.Num();
		FString Preview;
		int32 Emitted = 0;
		for (int32 i = 0; i < Helper.GetMaxIndex() && Emitted < kMaxCollectionPreviewElements; ++i)
		{
			if (!Helper.IsValidIndex(i)) continue;
			if (Emitted > 0) Preview += TEXT(", ");
			Preview += FormatScalar(Helper.GetKeyProperty(), Helper.GetKeyPtr(i));
			Preview += TEXT("=");
			Preview += FormatScalar(Helper.GetValueProperty(), Helper.GetValuePtr(i));
			++Emitted;
		}
		return FString::Printf(TEXT("%d pairs: [%s]"), Num, *Preview);
	}

	return FormatScalar(Prop, ValuePtr);
}

#if WITH_EDITOR

// Rename-stable GUID for one path segment, or invalid when the segment has none
// (native properties, functions). GetOwnerClass() is the class that DECLARED the
// property, so its generating Blueprint holds the GUID — as a NewVariables entry
// for value variables, or the SCS node's VariableGuid for component templates.
// UBlueprint / USCS_Node are Engine-module types — no UnrealEd dependency.
static FGuid GetPropertySegmentGuid(const FProperty* Prop)
{
	if (UClass* OwnerClass = Prop ? Prop->GetOwnerClass() : nullptr)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(OwnerClass->ClassGeneratedBy))
		{
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				if (Var.VarName == Prop->GetFName())
				{
					return Var.VarGuid;
				}
			}
			if (Blueprint->SimpleConstructionScript)
			{
				for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (Node && Node->GetVariableName() == Prop->GetFName())
					{
						return Node->VariableGuid;
					}
				}
			}
		}
	}
	return FGuid();
}

bool ConvaiPropertyReflection::HealPath(UObject* Root, FName DottedPath, TArray<FGuid>& InOutSegmentGuids, FName& OutHealedPath)
{
	OutHealedPath = DottedPath;
	if (DottedPath.IsNone())
	{
		return false;
	}

	TArray<FString> Tokens;
	DottedPath.ToString().ParseIntoArray(Tokens, TEXT("."), /*CullEmpty*/ true);
	if (Tokens.Num() == 0 || Tokens.Num() > MaxPathSegments)
	{
		return false;
	}

	InOutSegmentGuids.SetNum(Tokens.Num()); // pad with invalid GUIDs / drop stale extras

	UStruct* CurrentStruct = Root ? Root->GetClass() : nullptr;
	void* CurrentContainer = Root; // may go null after a null hop — healing continues class-only
	bool bChanged = false;

	for (int32 i = 0; CurrentStruct && i < Tokens.Num(); ++i)
	{
		FProperty* Found = CurrentStruct->FindPropertyByName(FName(*Tokens[i]));

		// Name lookup failed → try to re-latch by the stored GUID.
		if (!Found && InOutSegmentGuids[i].IsValid())
		{
			for (TFieldIterator<FProperty> It(CurrentStruct); It; ++It)
			{
				if (GetPropertySegmentGuid(*It) == InOutSegmentGuids[i])
				{
					Found = *It;
					Tokens[i] = Found->GetName();
					bChanged = true;
					break;
				}
			}
		}
		if (!Found)
		{
			// Function leaf, deleted variable, or no GUID on record — nothing more
			// we can do for this and later segments; keep them as-is.
			break;
		}

		// Capture / refresh the GUID for every segment that resolves — this IS
		// the recording mechanism: binding a path fires PostEditChange, which
		// runs a heal pass and lands here.
		const FGuid SegmentGuid = GetPropertySegmentGuid(Found);
		if (SegmentGuid.IsValid())
		{
			InOutSegmentGuids[i] = SegmentGuid;
		}

		// Advance the walk exactly like ResolvePath.
		if (FStructProperty* StructProp = CastField<FStructProperty>(Found))
		{
			CurrentContainer = CurrentContainer ? StructProp->ContainerPtrToValuePtr<void>(CurrentContainer) : nullptr;
			CurrentStruct = StructProp->Struct;
		}
		else if (IsTraversableObjectProperty(Found))
		{
			const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Found);
			UObject* Next = CurrentContainer
				? ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CurrentContainer))
				: nullptr;
			// Prefer the live instance's runtime class (BP-added members resolve);
			// fall back to the declared class when the hop is null in-editor.
			UClass* DeclaredClass = ObjProp->PropertyClass;
			CurrentStruct = IsValid(Next) ? Next->GetClass() : DeclaredClass;
			CurrentContainer = IsValid(Next) ? static_cast<void*>(Next) : nullptr;
		}
		else
		{
			break; // leaf reached
		}
	}

	if (bChanged)
	{
		OutHealedPath = FName(*FString::Join(Tokens, TEXT(".")));
	}
	return bChanged;
}

#endif // WITH_EDITOR
