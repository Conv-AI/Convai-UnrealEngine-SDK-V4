// Copyright Epic Games, Inc. All Rights Reserved.

#include "ConvaiVisionBase.h"
#include "UObject/CoreRedirects.h"

#define LOCTEXT_NAMESPACE "FConvaiVisionBaseModule"

void FConvaiVisionBaseModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module

	// Register class redirects to prevent breaking existing content when class names change
	TArray<FCoreRedirect> Redirects;
	Redirects.Add(FCoreRedirect{ECoreRedirectFlags::Type_Class, TEXT("EnvironmentWebcame"), TEXT("EnvironmentWebcam")});
	FCoreRedirects::AddRedirectList(Redirects, TEXT("ConvaiVisionBase"));
}

void FConvaiVisionBaseModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FConvaiVisionBaseModule, ConvaiVisionBase)