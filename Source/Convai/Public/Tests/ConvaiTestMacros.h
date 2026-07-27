// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#ifndef WITH_CONVAI_TESTS
#define WITH_CONVAI_TESTS 0
#endif



DECLARE_LOG_CATEGORY_EXTERN(LogConvaiTest, Log, All);

class UConvaiTestBase;

// Convenience macros used inside a UConvaiTestBase subclass. They go through the test's logger
// so every entry ends up in both the plugin log and the per-run artifacts.
#define CONVAI_TEST_LOG(Test, Verbosity, Format, ...) \
	do { (Test)->LogEvent(ELogVerbosity::Verbosity, FString::Printf(Format, ##__VA_ARGS__)); } while (0)

#define CONVAI_TEST_TRACE(Test, PhaseName) \
	FConvaiTestScopedTrace ANONYMOUS_VARIABLE(ConvaiTrace_)((Test), PhaseName)


