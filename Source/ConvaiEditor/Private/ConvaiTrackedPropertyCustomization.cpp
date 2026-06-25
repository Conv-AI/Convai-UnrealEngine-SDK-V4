// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiTrackedPropertyCustomization.h"

#include "ConvaiObjectComponent.h"
#include "ConvaiDefinitions.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyHandle.h"
#include "GameFramework/Actor.h"
#include "Features/IModularFeatures.h"
#include "IPropertyAccessEditor.h"
#include "Widgets/Text/STextBlock.h"
#include "Modules/ModuleManager.h"
#include "Runtime/Launch/Resources/Version.h"

// FPropertyBindingWidgetArgs delegate evolution (verified against engine source):
//   - 5.3 and earlier: only the legacy non-chain delegates exist
//                      (FOnCanAcceptPropertyOrChildren, FOnCanBindProperty)
//   - 5.4:             FOnCanAcceptPropertyOrChildrenWithBindingChain is added,
//                      but the bind-chain variant and FOnHasAnyBindings are NOT yet present
//   - 5.5 and later:   FOnCanBindPropertyWithBindingChain and FOnHasAnyBindings added;
//                      the legacy non-chain delegates become UE_DEPRECATED(5.4)
// So the "use chain delegates" path is only safe at 5.5+. Earlier versions
// (including 5.4, which has the partial chain API) fall back to the legacy
// delegates — those still compile in 5.4 with no warning (the deprecation
// macro in 5.4's header is commented-out; it activates in 5.5).
#define CONVAI_HAS_BINDING_CHAIN_DELEGATES (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION > 4))

#define LOCTEXT_NAMESPACE "ConvaiTrackedPropertyCustomization"

namespace
{
	// Whitelist of struct types we accept as leaf bindings (and as function return types).
	// These are tiny "value-like" structs the LLM can usefully consume as strings;
	// any other struct must be drilled into for a specific member.
	bool IsAllowedStructLeaf(const UStruct* Struct)
	{
		if (!Struct) return false;
		return Struct == TBaseStructure<FVector>::Get()
			|| Struct == TBaseStructure<FRotator>::Get()
			|| Struct == TBaseStructure<FVector2D>::Get()
			|| Struct == TBaseStructure<FQuat>::Get()
			|| Struct == TBaseStructure<FTransform>::Get();
	}

	// Match the runtime-side ConvaiPropertyReflection::IsSupportedLeaf rule: anything
	// that involves dereferencing a UObject / interface / class / delegate is rejected
	// because the polling system can't safely dereference at high frequency. Structs
	// other than the small value-types above are rejected as leaves (the picker still
	// drills into them for a specific member). Containers are recursively gated on
	// their inner / key / value types — otherwise TArray<UObject*> etc. slips past
	// as "container of unsupported" and we'd broadcast "<unsupported>" elements.
	bool IsConvaiBindableProperty(const FProperty* Prop)
	{
		if (!Prop)
		{
			return false;
		}
		if (Prop->IsA(FObjectPropertyBase::StaticClass())
			|| Prop->IsA(FInterfaceProperty::StaticClass())
			|| Prop->IsA(FClassProperty::StaticClass())
			|| Prop->IsA(FDelegateProperty::StaticClass())
			|| Prop->IsA(FMulticastDelegateProperty::StaticClass()))
		{
			return false;
		}
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			return IsAllowedStructLeaf(StructProp->Struct);
		}
		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			return ArrayProp->Inner && IsConvaiBindableProperty(ArrayProp->Inner);
		}
		if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			return SetProp->ElementProp && IsConvaiBindableProperty(SetProp->ElementProp);
		}
		if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			return MapProp->KeyProp && MapProp->ValueProp
				&& IsConvaiBindableProperty(MapProp->KeyProp)
				&& IsConvaiBindableProperty(MapProp->ValueProp);
		}
		return true;
	}
}

TSharedRef<IPropertyTypeCustomization> FConvaiTrackedPropertyCustomization::MakeInstance()
{
	return MakeShared<FConvaiTrackedPropertyCustomization>();
}

void FConvaiTrackedPropertyCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& /*StructCustomizationUtils*/)
{
	HeaderRow
		.NameContent()
		[
			StructPropertyHandle->CreatePropertyNameWidget()
		];
}

void FConvaiTrackedPropertyCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& /*StructCustomizationUtils*/)
{
	PropertyPathHandle = StructPropertyHandle->GetChildHandle(
		GET_MEMBER_NAME_CHECKED(FConvaiTrackedProperty, PropertyPath));

	UClass* OwnerClass = ResolveOwnerActorClass(StructPropertyHandle);

	uint32 NumChildren = 0;
	StructPropertyHandle->GetNumChildren(NumChildren);

	for (uint32 i = 0; i < NumChildren; ++i)
	{
		TSharedPtr<IPropertyHandle> Child = StructPropertyHandle->GetChildHandle(i);
		if (!Child.IsValid() || !Child->GetProperty())
		{
			continue;
		}
		const FName ChildName = Child->GetProperty()->GetFName();

		if (ChildName == GET_MEMBER_NAME_CHECKED(FConvaiTrackedProperty, PropertyPath))
		{
			ChildBuilder.AddCustomRow(LOCTEXT("PropertyPath_Filter", "Property Path"))
				.NameContent()
				[
					Child->CreatePropertyNameWidget()
				]
				.ValueContent()
				.MinDesiredWidth(280.f)
				[
					MakeBindingWidget(OwnerClass)
				];
		}
		else
		{
			ChildBuilder.AddProperty(Child.ToSharedRef());
		}
	}
}

UClass* FConvaiTrackedPropertyCustomization::ResolveOwnerActorClass(TSharedRef<IPropertyHandle> StructPropertyHandle) const
{
	TArray<UObject*> OuterObjects;
	StructPropertyHandle->GetOuterObjects(OuterObjects);

	// The struct's outer can be either the component (CDO / blueprint editor case)
	// or the actor (level editor case where the actor is what was selected). Walk
	// the chain to find a ConvaiObjectComponent, then use its owner's class for
	// the binding picker root.
	for (UObject* Outer : OuterObjects)
	{
		if (!Outer)
		{
			continue;
		}
		if (UConvaiObjectComponent* Component = Cast<UConvaiObjectComponent>(Outer))
		{
			// Level-placed instance — GetOwner() returns the actor directly.
			if (AActor* Owner = Component->GetOwner())
			{
				return Owner->GetClass();
			}
			// Native-template-as-default-subobject (rare) — actor is in the outer chain.
			if (AActor* OwnerTemplate = Component->GetTypedOuter<AActor>())
			{
				return OwnerTemplate->GetClass();
			}
			// Blueprint SCS-stored component template (e.g. ConvaiObject_GEN_VARIABLE):
			// outer chain is USCS_Node → USimpleConstructionScript → UBlueprintGeneratedClass,
			// no AActor present. Walk up to the owning class directly.
			if (UClass* OwningClass = Component->GetTypedOuter<UClass>())
			{
				return OwningClass;
			}
		}
		if (AActor* OuterActor = Cast<AActor>(Outer))
		{
			return OuterActor->GetClass();
		}
	}
	return nullptr;
}

FName FConvaiTrackedPropertyCustomization::GetCurrentPath() const
{
	if (!PropertyPathHandle.IsValid())
	{
		return NAME_None;
	}
	FName Out;
	if (PropertyPathHandle->GetValue(Out) == FPropertyAccess::Success)
	{
		return Out;
	}
	return NAME_None;
}

TSharedRef<SWidget> FConvaiTrackedPropertyCustomization::MakeBindingWidget(UClass* OwnerClass)
{
	// Ensure the PropertyAccessEditor plugin module is loaded — it registers the
	// IPropertyAccessEditor modular feature on its own StartupModule, so just
	// touching it guarantees the feature is available.
	FModuleManager::Get().LoadModule(TEXT("PropertyAccessEditor"));

	if (!OwnerClass || !IModularFeatures::Get().IsModularFeatureAvailable("PropertyAccessEditor"))
	{
		// Fallback: plain FName editor (better than nothing if the picker can't be built —
		// commandlet, headless cook, or the property handle's outer chain has no
		// reachable ConvaiObjectComponent / Actor class).
		return PropertyPathHandle.IsValid()
			? PropertyPathHandle->CreatePropertyValueWidget()
			: SNew(STextBlock).Text(LOCTEXT("PropertyPath_Unavailable", "Binding picker unavailable"));
	}

	IPropertyAccessEditor& PropertyAccessEditor =
		IModularFeatures::Get().GetModularFeature<IPropertyAccessEditor>("PropertyAccessEditor");

	// Capture a weak-handle to the property so the closure doesn't outlive it.
	TWeakPtr<IPropertyHandle> WeakPath = PropertyPathHandle;

	FPropertyBindingWidgetArgs Args;

	// SPropertyBinding gates its property-menu population on CanBindProperty(Args.Property, ...).
	// If Args.Property is null, our OnCanBindPropertyWithBindingChain lambda receives null and
	// returns false → the whole menu stays empty. Point it at our FName destination handle.
	Args.Property = PropertyPathHandle->GetProperty();

	Args.bAllowFunctionBindings        = true;  // allow pure, FString-returning functions (see OnCanBindFunction)
	Args.bAllowFunctionLibraryBindings = false;
	Args.bAllowPropertyBindings        = true;
	Args.bAllowStructMemberBindings    = true;
	Args.bAllowNewBindings             = false; // no "Create New Binding" entry — we're not creating BP functions
	Args.bAllowArrayElementBindings    = false;
	Args.bAllowUObjectFunctions        = false;
	Args.bAllowStructFunctions         = false;
	Args.bGeneratePureBindings         = true;  // only show pure / const functions

	// Reject object refs / interfaces / classes / delegates AND their subtrees so
	// the user can't drill across actor boundaries — matches the runtime polling
	// safety policy. Structs are still walked into; the "no struct leaf" rule below
	// just suppresses the duplicate "bind whole struct" entry next to the submenu
	// (FVector / FRotator / etc. value-structs are excepted via IsConvaiBindableProperty).
	auto CanAcceptLambda = [](FProperty* InProperty) -> bool
	{
		return IsConvaiBindableProperty(InProperty);
	};
	auto CanBindLambda = [](FProperty* InProperty) -> bool
	{
		if (!IsConvaiBindableProperty(InProperty))
		{
			return false;
		}
		if (const FStructProperty* StructProp = CastField<FStructProperty>(InProperty))
		{
			// Drop the leaf entry for non-value-like structs so they only appear as drill-down.
			// IsConvaiBindableProperty already let value-like structs (FVector / FRotator / ...) pass.
			return IsAllowedStructLeaf(StructProp->Struct);
		}
		return true;
	};
#if CONVAI_HAS_BINDING_CHAIN_DELEGATES
	Args.OnCanAcceptPropertyOrChildrenWithBindingChain = FOnCanAcceptPropertyOrChildrenWithBindingChain::CreateLambda(
		[CanAcceptLambda](FProperty* InProperty, TConstArrayView<FBindingChainElement> /*InChain*/) -> bool
		{
			return CanAcceptLambda(InProperty);
		});
	Args.OnCanBindPropertyWithBindingChain = FOnCanBindPropertyWithBindingChain::CreateLambda(
		[CanBindLambda](FProperty* InProperty, TConstArrayView<FBindingChainElement> /*InChain*/) -> bool
		{
			return CanBindLambda(InProperty);
		});
#else
	Args.OnCanAcceptPropertyOrChildren = FOnCanAcceptPropertyOrChildren::CreateLambda(CanAcceptLambda);
	Args.OnCanBindProperty = FOnCanBindProperty::CreateLambda(CanBindLambda);
#endif

	// Pure, parameter-less functions whose return type we know how to format as a string.
	// Same rule we use for property leaves — bool, numeric, string, name, text, enum,
	// FVector / FRotator / FVector2D / FQuat / FTransform structs.
	Args.OnCanBindFunction = FOnCanBindFunction::CreateLambda(
		[](UFunction* InFunction) -> bool
		{
			if (!InFunction)
			{
				return false;
			}
			FProperty* ReturnProp = InFunction->GetReturnProperty();
			if (!IsConvaiBindableProperty(ReturnProp))
			{
				return false;
			}
			// No input parameters: the only parm-flagged property is the return.
			int32 NumParms = 0;
			for (TFieldIterator<FProperty> It(InFunction); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Parm))
				{
					++NumParms;
				}
			}
			return NumParms == 1; // only the return value counts
		});

	Args.OnCanBindToClass = FOnCanBindToClass::CreateLambda([](UClass*) -> bool { return true; });

	// Set the dotted-path FName when the user picks something. We flatten the chain
	// manually instead of using IPropertyAccessEditor::MakeStringPath because the
	// first element of every chain is the BindingContextStruct-index marker (Field is
	// null, ArrayIndex is the struct index) — MakeStringPath check(false)s on it.
	Args.OnAddBinding = FOnAddBinding::CreateLambda(
		[WeakPath](FName /*InPropertyName*/, const TArray<FBindingChainElement>& InBindingChain)
		{
			TSharedPtr<IPropertyHandle> Path = WeakPath.Pin();
			if (!Path.IsValid())
			{
				return;
			}

			TArray<FString> Segments;
			for (const FBindingChainElement& Element : InBindingChain)
			{
				if (FProperty* Property = Element.Field.Get<FProperty>())
				{
					Segments.Add(Property->GetName());
				}
				else if (UFunction* Function = Element.Field.Get<UFunction>())
				{
					Segments.Add(Function->GetName());
				}
				// Skip the null-Field context-root marker.
			}
			const FString Joined = FString::Join(Segments, TEXT("."));
			Path->SetValue(FName(*Joined));
		});

	// Show the current path on the button.
	Args.CurrentBindingText = TAttribute<FText>::CreateLambda(
		[WeakPath]() -> FText
		{
			TSharedPtr<IPropertyHandle> Path = WeakPath.Pin();
			if (!Path.IsValid())
			{
				return LOCTEXT("PropertyPath_None", "<pick a property>");
			}
			FName Current;
			if (Path->GetValue(Current) == FPropertyAccess::Success && !Current.IsNone())
			{
				return FText::FromString(Current.ToString());
			}
			return LOCTEXT("PropertyPath_None", "<pick a property>");
		});

	Args.OnCanRemoveBinding = FOnCanRemoveBinding::CreateLambda(
		[WeakPath](FName /*InPropertyName*/) -> bool
		{
			TSharedPtr<IPropertyHandle> Path = WeakPath.Pin();
			if (!Path.IsValid())
			{
				return false;
			}
			FName Current;
			return Path->GetValue(Current) == FPropertyAccess::Success && !Current.IsNone();
		});

	Args.OnRemoveBinding = FOnRemoveBinding::CreateLambda(
		[WeakPath](FName /*InPropertyName*/)
		{
			if (TSharedPtr<IPropertyHandle> Path = WeakPath.Pin())
			{
				Path->SetValue(FName(NAME_None));
			}
		});

#if CONVAI_HAS_BINDING_CHAIN_DELEGATES
	Args.OnHasAnyBindings = FOnHasAnyBindings::CreateLambda(
		[WeakPath]() -> bool
		{
			TSharedPtr<IPropertyHandle> Path = WeakPath.Pin();
			if (!Path.IsValid())
			{
				return false;
			}
			FName Current;
			return Path->GetValue(Current) == FPropertyAccess::Success && !Current.IsNone();
		});
#endif

	// Context struct: the owning Actor's UClass — picker walks its properties
	// recursively to MaxDepth (default 10). FBindingContextStruct's default constructor
	// only exists in UE 5.2+; the parameterized ctor works across all versions.
	TArray<FBindingContextStruct> Contexts;
	Contexts.Emplace(OwnerClass);

	return PropertyAccessEditor.MakePropertyBindingWidget(Contexts, Args);
}

#undef LOCTEXT_NAMESPACE
