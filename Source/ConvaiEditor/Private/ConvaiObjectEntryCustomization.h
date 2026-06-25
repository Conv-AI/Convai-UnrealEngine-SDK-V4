// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"

class IPropertyHandle;
class FDetailWidgetRow;
class IDetailChildrenBuilder;

/**
 * Struct-level details-panel customization for FConvaiObjectEntry, applied
 * everywhere the struct is shown (UConvaiObjectComponent::ObjectEntry,
 * UConvaiChatbotComponent::EnvironmentData.Objects, EnvironmentData.Characters,
 * any action-context fields, etc.).
 *
 * Visibility rules:
 *   - OptionalPositionVector is hidden everywhere. It carries the
 *     DeprecatedProperty meta but UE still shows deprecated fields with just a
 *     tooltip warning — we want it gone from the panel entirely now that
 *     Resolve Goal Location is the canonical path.
 *   - Ref is hidden only when the outer is UConvaiObjectComponent. The
 *     component auto-binds Ref to GetOwner() at BeginPlay, so a user-pickable
 *     widget there is misleading. In every other usage (e.g. the chatbot's
 *     environment arrays) the user does pick Ref manually, so it stays.
 */
class FConvaiObjectEntryCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;

	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle,
		IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;
};
