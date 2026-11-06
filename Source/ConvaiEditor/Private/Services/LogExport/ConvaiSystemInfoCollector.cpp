/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiSystemInfoCollector.cpp
 *
 * Implementation of system information collector.
 */

#include "Services/LogExport/ConvaiSystemInfoCollector.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "RHI.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

TSharedPtr<FJsonObject> FConvaiSystemInfoCollector::CollectInfo() const
{
	TSharedPtr<FJsonObject> SystemInfo = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> OSInfo = CollectOSInfo();
	if (OSInfo.IsValid())
	{
		SystemInfo->SetObjectField(TEXT("OperatingSystem"), OSInfo);
	}

	TSharedPtr<FJsonObject> HardwareInfo = CollectHardwareInfo();
	if (HardwareInfo.IsValid())
	{
		SystemInfo->SetObjectField(TEXT("Hardware"), HardwareInfo);
	}

	TSharedPtr<FJsonObject> LocaleInfo = CollectLocaleInfo();
	if (LocaleInfo.IsValid())
	{
		SystemInfo->SetObjectField(TEXT("Locale"), LocaleInfo);
	}

	SystemInfo->SetStringField(TEXT("CollectionTimestamp"), FDateTime::UtcNow().ToIso8601());

	return SystemInfo;
}

TSharedPtr<FJsonObject> FConvaiSystemInfoCollector::CollectOSInfo() const
{
	TSharedPtr<FJsonObject> OSInfo = MakeShared<FJsonObject>();

	OSInfo->SetStringField(TEXT("Platform"), FPlatformProperties::PlatformName());

	OSInfo->SetStringField(TEXT("OSVersion"), FPlatformMisc::GetOSVersion());

	OSInfo->SetBoolField(TEXT("Is64Bit"), FPlatformMisc::Is64bitOperatingSystem());

	OSInfo->SetStringField(TEXT("ComputerName"), FPlatformProcess::ComputerName());

	OSInfo->SetStringField(TEXT("UserName"), FPlatformProcess::UserName());

	FDisplayMetrics DisplayMetrics;
	FDisplayMetrics::RebuildDisplayMetrics(DisplayMetrics);

	TSharedPtr<FJsonObject> DisplayInfo = MakeShared<FJsonObject>();
	DisplayInfo->SetNumberField(TEXT("PrimaryDisplayWidth"), (double)DisplayMetrics.PrimaryDisplayWidth);
	DisplayInfo->SetNumberField(TEXT("PrimaryDisplayHeight"), (double)DisplayMetrics.PrimaryDisplayHeight);

	OSInfo->SetObjectField(TEXT("Display"), DisplayInfo);

	return OSInfo;
}

TSharedPtr<FJsonObject> FConvaiSystemInfoCollector::CollectHardwareInfo() const
{
	TSharedPtr<FJsonObject> HardwareInfo = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> CPUInfo = MakeShared<FJsonObject>();
	CPUInfo->SetStringField(TEXT("Brand"), GetCPUBrand());
	CPUInfo->SetNumberField(TEXT("LogicalCores"), (double)FPlatformMisc::NumberOfCores());
	CPUInfo->SetNumberField(TEXT("PhysicalCores"), (double)FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	HardwareInfo->SetObjectField(TEXT("CPU"), CPUInfo);

	TSharedPtr<FJsonObject> MemoryInfo = MakeShared<FJsonObject>();
	MemoryInfo->SetNumberField(TEXT("TotalPhysicalGB"), (double)GetTotalPhysicalRAM());

	FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	MemoryInfo->SetNumberField(TEXT("AvailablePhysicalMB"), MemStats.AvailablePhysical / (1024.0 * 1024.0));
	MemoryInfo->SetNumberField(TEXT("UsedPhysicalMB"), MemStats.UsedPhysical / (1024.0 * 1024.0));

	HardwareInfo->SetObjectField(TEXT("Memory"), MemoryInfo);

	TArray<TSharedPtr<FJsonValue>> GPUArray = GetGPUInfo();
	if (GPUArray.Num() > 0)
	{
		HardwareInfo->SetArrayField(TEXT("GPUs"), GPUArray);
	}

	return HardwareInfo;
}

TSharedPtr<FJsonObject> FConvaiSystemInfoCollector::CollectLocaleInfo() const
{
	TSharedPtr<FJsonObject> LocaleInfo = MakeShared<FJsonObject>();

	FCulturePtr Culture = FInternationalization::Get().GetCurrentCulture();
	if (Culture.IsValid())
	{
		LocaleInfo->SetStringField(TEXT("CultureName"), Culture->GetName());
		LocaleInfo->SetStringField(TEXT("DisplayName"), Culture->GetDisplayName());
		LocaleInfo->SetStringField(TEXT("EnglishName"), Culture->GetEnglishName());
		LocaleInfo->SetStringField(TEXT("NativeName"), Culture->GetNativeName());
		LocaleInfo->SetStringField(TEXT("TwoLetterISOLanguageName"), Culture->GetTwoLetterISOLanguageName());
		LocaleInfo->SetStringField(TEXT("ThreeLetterISOLanguageName"), Culture->GetThreeLetterISOLanguageName());
	}

	LocaleInfo->SetStringField(TEXT("DefaultLocale"), FPlatformMisc::GetDefaultLocale());

	return LocaleInfo;
}

FString FConvaiSystemInfoCollector::GetCPUBrand() const
{
	return FPlatformMisc::GetCPUBrand();
}

TArray<TSharedPtr<FJsonValue>> FConvaiSystemInfoCollector::GetGPUInfo() const
{
	TArray<TSharedPtr<FJsonValue>> GPUArray;

	TSharedPtr<FJsonObject> PrimaryGPU = MakeShared<FJsonObject>();
	PrimaryGPU->SetStringField(TEXT("AdapterName"), GRHIAdapterName);
	PrimaryGPU->SetStringField(TEXT("DriverVersion"), GRHIAdapterUserDriverVersion);
	PrimaryGPU->SetStringField(TEXT("RHIName"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown"));
	PrimaryGPU->SetBoolField(TEXT("SupportsRayTracing"), GRHISupportsRayTracing);

	GPUArray.Add(MakeShared<FJsonValueObject>(PrimaryGPU));

	return GPUArray;
}

float FConvaiSystemInfoCollector::GetTotalPhysicalRAM() const
{
	const FPlatformMemoryConstants &MemoryConstants = FPlatformMemory::GetConstants();
	return MemoryConstants.TotalPhysical / (1024.0f * 1024.0f * 1024.0f);
}
