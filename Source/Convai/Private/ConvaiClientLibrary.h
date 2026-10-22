// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace ConvaiModulePrivate
{
	/** False when Convai::StartupModule could not load convai_client. On Win64 it
	 *  is a delay-load import, and the first call into a library that did not load
	 *  raises an exception nothing handles, so a connect refuses instead. */
	bool IsClientLibraryUsable();

#if WITH_TESTS
	/** The verdict itself, for a unit row to set and put back. */
	bool& ClientLibraryUsableForTests();
#endif
}
