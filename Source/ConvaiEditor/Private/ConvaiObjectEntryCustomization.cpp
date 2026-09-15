// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiObjectEntryCustomization.h"

#include "ConvaiDefinitions.h"
#include "ConvaiObjectComponent.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyHandle.h"

TSharedRef<IPropertyTypeCustomization> FConvaiObjectEntryCustomization::MakeInstance()
{
	return MakeShared<FConvaiObjectEntryCustomization>();
}

void FConvaiObjectEntryCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& /*StructCustomizationUtils*/)
{
	// Keep the default header rendering. CustomizeChildren below is where we
	// filter out deprecated / context-sensitive fields. When the outer property
	// has meta=(ShowOnlyInnerProperties), UE skips rendering this header
	// entirely and CustomizeChildren is what populates the parent panel.
	HeaderRow
		.NameContent()
		[
			StructPropertyHandle->CreatePropertyNameWidget()
		];
}

void FConvaiObjectEntryCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& /*StructCustomizationUtils*/)
{
	// Detect whether this struct is being shown on a ConvaiObjectComponent — if so,
	// Ref is auto-bound to GetOwner() at BeginPlay and exposing it just invites
	// the user to mis-target. In every other context (chatbot environment arrays,
	// action-result fields, etc.) Ref is real user input.
	bool bOuterIsObjectComponent = false;
	{
		TArray<UObject*> OuterObjects;
		StructPropertyHandle->GetOuterObjects(OuterObjects);
		for (UObject* Outer : OuterObjects)
		{
			if (Outer && Outer->IsA(UConvaiObjectComponent::StaticClass()))
			{
				bOuterIsObjectComponent = true;
				break;
			}
		}
	}

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

		// Globally hidden: deprecated auto-written snapshot. Use Resolve Goal
		// Location's Out Goal Location output instead. Kept as a UPROPERTY for
		// back-compat with old BP graphs that read it, but never shown.
		if (ChildName == GET_MEMBER_NAME_CHECKED(FConvaiObjectEntry, OptionalPositionVector))
		{
			continue;
		}

		// Conditionally hidden on UConvaiObjectComponent only.
		if (bOuterIsObjectComponent && ChildName == GET_MEMBER_NAME_CHECKED(FConvaiObjectEntry, Ref))
		{
			continue;
		}

		ChildBuilder.AddProperty(Child.ToSharedRef());
	}
}
