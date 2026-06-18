// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"

class IPropertyHandle;
class FDetailWidgetRow;
class IDetailChildrenBuilder;

/**
 * Struct-level details customization for FConvaiTrackedProperty.
 *
 * Replaces the default FName text field for PropertyPath with the AnimGraph-style
 * "Bind" widget that UE ships via IPropertyAccessEditor::MakePropertyBindingWidget.
 * Users get a hierarchical, searchable picker that walks the owning Actor's class
 * to arbitrary depth (struct members, sub-struct members, etc.).
 *
 * Filtered out at the picker level:
 *   - Object references / interfaces / classes / delegates (runtime polling can't
 *     safely dereference UObject refs at high frequency).
 *   - Containers (TArray / TSet / TMap) whose element / key / value type is itself
 *     rejected by these rules.
 *   - Whole-struct leaves except value-like types (FVector / FRotator / FVector2D /
 *     FQuat / FTransform) — other structs only appear as drill-down submenus.
 *
 * Function bindings ARE allowed, but only for pure (BlueprintPure / Const),
 * parameter-less functions whose return type is itself a bindable leaf. So
 * GetActorLocation / GetActorRotation / author-added pure getters show up under
 * Functions, but impure or arg-taking functions do not.
 *
 * All other fields (Description, StateValueDescriptions, ShouldRespond) keep
 * their default presentation.
 */
class FConvaiTrackedPropertyCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;

	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle,
		IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;

private:
	TSharedPtr<IPropertyHandle> PropertyPathHandle;

	/** Discovers the owning Actor's UClass from the struct property's outer chain. */
	UClass* ResolveOwnerActorClass(TSharedRef<IPropertyHandle> StructPropertyHandle) const;

	/** Reads the current FName path from the property handle. */
	FName GetCurrentPath() const;

	/** Builds the AnimGraph-style binding widget for the PropertyPath field. */
	TSharedRef<SWidget> MakeBindingWidget(UClass* OwnerClass);
};
