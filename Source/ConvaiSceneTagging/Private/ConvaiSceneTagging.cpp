// Copyright Convai. All Rights Reserved.

#include "Modules/ModuleManager.h"

// The scene-tagging pipeline is a library module: consumers (the editor tagger
// and Assembly Studio) initialize the native core adapter themselves.
IMPLEMENT_MODULE(FDefaultModuleImpl, ConvaiSceneTagging)
