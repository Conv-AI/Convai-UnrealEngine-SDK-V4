// Copyright 2022 Convai Inc. All Rights Reserved.

#include "AnimGraphNode_ConvaiFaceSync.h"

#define LOCTEXT_NAMESPACE "ConvaiFaceSync"

FText UAnimGraphNode_ConvaiFaceSync::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("NodeTitle", "Convai Face Sync");
}

FText UAnimGraphNode_ConvaiFaceSync::GetTooltipText() const
{
	return LOCTEXT("Tooltip", "Reads Convai facial blendshapes and applies them as animation curves with separate upper/lower face alpha control and optional blendshape remapping.");
}

FText UAnimGraphNode_ConvaiFaceSync::GetMenuCategory() const
{
	return LOCTEXT("MenuCategory", "Convai");
}

FString UAnimGraphNode_ConvaiFaceSync::GetNodeCategory() const
{
	return TEXT("Convai");
}

#undef LOCTEXT_NAMESPACE
