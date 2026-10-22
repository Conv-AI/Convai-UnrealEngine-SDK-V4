// Copyright Convai Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "ConvaiToolsetSupport.h" // defines CONVAI_TOOLSET_SUPPORTED

// The reflected toolset types live in Private/Gated/ConvaiToolset.h, which is excluded
// from the build (and from UHT) on engines older than 5.8 via a .ubtignore. Only pull
// it in -- along with the 5.8-only ToolsetRegistry -- when the toolset is supported.
#if CONVAI_TOOLSET_SUPPORTED
#include "ConvaiToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#endif

#define LOCTEXT_NAMESPACE "FConvaiToolsetModule"

/**
 * Editor module that exposes Convai setup as first-class AI toolset tools.
 *
 * Mirrors the minimal pattern of FEditorToolsetModule: on startup it registers
 * each UToolsetDefinition subclass with the global UToolsetRegistry so the
 * AICallable UFUNCTIONs become discoverable as MCP tools; on shutdown it
 * unregisters them in reverse order.
 */
class FConvaiToolsetModule : public IModuleInterface
{
	virtual void StartupModule() override
	{
#if CONVAI_TOOLSET_SUPPORTED
		UToolsetRegistry::RegisterToolsetClass(UConvaiSetupToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(UConvaiActionToolset::StaticClass());
#endif
	}

	virtual void ShutdownModule() override
	{
#if CONVAI_TOOLSET_SUPPORTED
		UToolsetRegistry::UnregisterToolsetClass(UConvaiActionToolset::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UConvaiSetupToolset::StaticClass());
#endif
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FConvaiToolsetModule, ConvaiToolset)
