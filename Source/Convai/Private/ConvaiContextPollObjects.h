// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "ConvaiContextSubsystem.h"
#include "ConvaiSubsystem.h"

namespace ConvaiContextPrivate
{
	/** A logical object's movement settings over all its members, so the result does not depend
	 *  on their order: strongest response, safest delivery, any urgent flush. */
	struct FResolvedMovementSettings
	{
		bool bAwarenessEnabled = false;
		bool bStateEnabled = false;
		bool bStateKeyCollision = false;
		EC_RunLLMOption StartedResponse = EC_RunLLMOption::Never;
		EC_RunLLMOption StoppedResponse = EC_RunLLMOption::Never;
		EConvaiContextDelivery Delivery = EConvaiContextDelivery::SendNormally;
		bool bFlushImmediately = false;
	};
}

/** The logical objects one context poll reads, built once after the object poll. Members are
 *  raw pointers: garbage collection cannot run inside the poll, but Blueprint a pass reaches
 *  can destroy a member, so every pass checks IsValid before it touches one. */
struct UConvaiContextSubsystem::FPollObjects
{
	/** What the passes derive from one group, once per poll. */
	struct FDerived
	{
		/** "Object:<lower-case name>", the key of the motion and spatial caches. */
		FString CacheKey;
		/** The sanitized "<Name>.Movement" state key. */
		FString MovementStateKey;
		ConvaiContextPrivate::FResolvedMovementSettings Movement;
	};

	explicit FPollObjects(const UConvaiSubsystem& Subsystem);

	TArray<UConvaiSubsystem::FConvaiObjectGroup> Groups;
	/** Parallel to Groups. */
	TArray<FDerived> Derived;
};
