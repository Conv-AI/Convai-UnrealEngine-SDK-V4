// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiConversationComponent.h"

bool UConvaiConversationComponent::IsPlayer() const
{
    // Base implementation returns false
    // Player component will override this to return true
    return false;
}

FString UConvaiConversationComponent::GetConversationalName() const
{
    // Base implementation returns the component name
    // Derived classes should override this to return player name or character name
    return GetName();
}
