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
 * and the engine rebuilds it only when the sequence is saved. When the project's
 * skeleton differs from the one the plugin content was saved against, the stored
 * pose lands on the wrong bones and the body distorts.
 *
 * - On editor startup, if the project has the skeleton and retarget mesh these
 *   animations depend on, an even spread of up to 20 of them is checked. The
 *   first out-of-date pose stops the check and the user is asked whether to fix
 *   them; on Yes every sequence is checked and the out-of-date ones are rebuilt
 *   and saved. A No asks again on the next startup. When the sample is clean the
 *   project is treated as up to date and not checked again until its skeleton
 *   or the plugin version changes.
 * - While cooking, out-of-date poses are rebuilt silently in memory and the
 *   platform data recompressed, because the engine skips the rebuild for cook
 *   saves; nothing is written back to the plugin.
 */
class FConvaiRetargetPoseRefresher
{
public:
	static void Register();
	static void Unregister();
};
