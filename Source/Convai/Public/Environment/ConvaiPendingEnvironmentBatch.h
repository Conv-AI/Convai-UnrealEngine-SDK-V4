// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Aggregates Environment mutations within a debounce window so multiple
 * Add/Remove/Clear calls coalesce into a single update-scene-metadata send.
 *
 * The wire payload is a full snapshot of the chatbot's current Environment.Objects
 * and Environment.Characters minus the connect-time snapshot, so we don't need
 * per-key delta tracking like FConvaiPendingContextBatch — a single dirty flag is
 * enough to schedule a flush.
 */
struct CONVAI_API FConvaiPendingEnvironmentBatch
{
	/** Set by Add/Remove/Clear methods on Objects or Characters. Cleared on flush. */
	bool bSceneMetadataDirty = false;

	bool HasWork() const { return bSceneMetadataDirty; }

	void Clear() { bSceneMetadataDirty = false; }
};
