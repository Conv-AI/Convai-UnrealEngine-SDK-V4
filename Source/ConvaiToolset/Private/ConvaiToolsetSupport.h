// Copyright Convai Inc. All Rights Reserved.

#pragma once

#include "Misc/EngineVersionComparison.h"

// The engine ToolsetRegistry plugin (UToolsetDefinition + the AICallable toolset
// machinery this module builds on) only exists on UE 5.8+. On 5.0-5.7 every
// toolset class/function/registration is compiled out and the module links as an
// empty module.
//
// IMPORTANT: the reflected types themselves (USTRUCT/UCLASS/UFUNCTION) live in
// Private/Gated/ConvaiToolset.h and are gated at the BUILD level: ConvaiToolset.Build.cs
// drops a ".ubtignore" into Private/Gated on engines older than 5.8 so UnrealHeaderTool
// never sees them. They must NOT be wrapped in a "#if CONVAI_TOOLSET_SUPPORTED" block
// because UHT forbids reflection macros inside any preprocessor block other than
// WITH_EDITORONLY_DATA (this is enforced identically on 5.0 through 5.8).
//
// This macro still gates the non-reflected glue (module registration and the .cpp
// implementation bodies) so they compile out cleanly wherever the reflected types
// are absent.
#define CONVAI_TOOLSET_SUPPORTED (!UE_VERSION_OLDER_THAN(5, 8, 0))
