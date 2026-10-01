/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiRetargetPoseRefresher.h
 *
 * Keeps the retarget pose stored in Convai's MetaHuman animations in sync with
 * the project's MetaHuman skeleton.
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Convai's MetaHuman animations use the project's
 * /Game/MetaHumans/Common/.../metahuman_base_skel and a preview mesh as their
 * retarget source. Each sequence stores that mesh's pose per skeleton bone index,
 * and the engine rebuilds it only when the sequence is saved by a user. When the
 * project's skeleton differs from the one the plugin content was saved against,
 * the stored pose lands on the wrong bones and the body distorts until someone
 * resaves the animations by hand.
 *
 * This rebuilds the pose for the current skeleton:
 * - in the editor, the tick after a Convai sequence loads (not in PostLoad, which
 *   UpdateRetargetSourceAssetData forbids), then recompresses it, in memory only;
 * - while cooking, in the sequence's PreSave, then recompresses synchronously for
 *   the target platform, because the engine skips the rebuild for cook saves.
 */
class FConvaiRetargetPoseRefresher
{
public:
	static void Register();
	static void Unregister();
};
