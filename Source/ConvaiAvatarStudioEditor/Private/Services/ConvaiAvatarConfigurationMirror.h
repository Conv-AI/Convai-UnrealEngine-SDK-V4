// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

/** jsDelivr copy of a published configuration URL on the configured branch, or empty when there is none.
 *  Used only after raw.githubusercontent.com fails; some networks block it while the CDN stays reachable. */
FString ConvaiAvatarConfigurationMirrorUrl(const FString& Url);
