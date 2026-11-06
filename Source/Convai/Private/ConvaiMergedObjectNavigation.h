// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "Containers/ArrayView.h"

class AActor;
class UConvaiChatbotComponent;
class UConvaiObjectComponent;
struct FConvaiObjectEntry;
struct FConvaiResultAction;

namespace ConvaiMergedObjectNavigation
{
	/**
	 * Resolve one logical merged object to the nearest concrete member the source
	 * can already reach or navigate to. The logical name/description and named
	 * movement-point identity are preserved in OutResolvedEntry.
	 *
	 * Returns false, leaving OutResolvedEntry equal to LogicalEntry, when this is
	 * not a live multi-member merge group or no member is currently reachable.
	 * Game-thread only.
	 */
	bool ResolveNearestReachableMember(
		AActor* SourceActor,
		const FConvaiObjectEntry& LogicalEntry,
		TConstArrayView<UConvaiObjectComponent*> RegisteredComponents,
		FConvaiObjectEntry& OutResolvedEntry);

	/** Resolve object-reference params in an action against an explicit registry view. */
	void ResolveActionObjectReferences(
		UConvaiChatbotComponent& Chatbot,
		FConvaiResultAction& Action,
		TConstArrayView<UConvaiObjectComponent*> RegisteredComponents);

	/** Resolve object-reference params using the live Convai object registry. */
	void ResolveActionObjectReferences(
		UConvaiChatbotComponent& Chatbot,
		FConvaiResultAction& Action);

	/** Resolve a whole plan against one view of the live registry, each logical object
	 *  once: a group named by several actions costs one set of path queries. */
	void ResolvePlanObjectReferences(
		UConvaiChatbotComponent& Chatbot,
		TArrayView<FConvaiResultAction> Plan);

#if WITH_TESTS
	/** Member goal resolutions (each up to a synchronous path query) since a test reset it. */
	extern int32 MemberGoalResolutionsForTests;
#endif
}

