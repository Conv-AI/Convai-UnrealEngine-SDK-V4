// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_Base.h"
#include "Animation/AnimNode_ConvaiFaceSync.h"
#include "AnimGraphNode_ConvaiFaceSync.generated.h"

/**
 * Editor graph node for the Convai Face Sync animation node.
 * Exposes the FAnimNode_ConvaiFaceSync in the AnimGraph editor.
 */
UCLASS()
class CONVAIANIMGRAPH_API UAnimGraphNode_ConvaiFaceSync : public UAnimGraphNode_Base
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Settings")
	FAnimNode_ConvaiFaceSync Node;

public:
	// UEdGraphNode interface
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetMenuCategory() const override;

	// UAnimGraphNode_Base interface
	virtual FString GetNodeCategory() const override;
};
