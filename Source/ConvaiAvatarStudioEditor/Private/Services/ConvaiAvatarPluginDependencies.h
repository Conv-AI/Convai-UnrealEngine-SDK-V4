// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once

#include "PluginReferenceDescriptor.h"

namespace ConvaiAvatarPluginDependencies
{
	// Source installation runs in the host editor. Dependencies restricted to other
	// platforms/targets must not cause an impossible enable-and-restart loop.
	inline bool IsRequired(const FPluginReferenceDescriptor& Reference, const FString& Platform,
		EBuildConfiguration Configuration, EBuildTargetType Target)
	{
		return Reference.bEnabled && !Reference.bOptional
			&& Reference.IsEnabledForPlatform(Platform)
			&& Reference.IsSupportedTargetPlatform(Platform)
			&& Reference.IsEnabledForTargetConfiguration(Configuration)
			&& Reference.IsEnabledForTarget(Target);
	}
}
