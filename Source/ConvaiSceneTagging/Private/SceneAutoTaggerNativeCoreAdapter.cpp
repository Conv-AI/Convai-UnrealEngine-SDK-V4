
// Copyright Convai. All Rights Reserved.

#include "SceneAutoTaggerNativeCoreAdapter.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace
{
	constexpr int32 MaxCoreIdentityBytes = 256;

	bool IsContinuationByte(const uint8 Byte)
	{
		return (Byte & 0xc0u) == 0x80u;
	}

	bool IsValidBoundedUtf8(const char* Value)
	{
		if (Value == nullptr || Value[0] == '\0')
		{
			return false;
		}

		const uint8* Bytes = reinterpret_cast<const uint8*>(Value);
		int32 Index = 0;
		while (Index < MaxCoreIdentityBytes)
		{
			const uint8 Lead = Bytes[Index];
			if (Lead == 0u)
			{
				return true;
			}

			if (Lead <= 0x7fu)
			{
				++Index;
				continue;
			}

			int32 ContinuationCount = 0;
			if (Lead >= 0xc2u && Lead <= 0xdfu)
			{
				ContinuationCount = 1;
			}
			else if (Lead >= 0xe0u && Lead <= 0xefu)
			{
				ContinuationCount = 2;
			}
			else if (Lead >= 0xf0u && Lead <= 0xf4u)
			{
				ContinuationCount = 3;
			}
			else
			{
				return false;
			}

			if (Index + ContinuationCount >= MaxCoreIdentityBytes)
			{
				return false;
			}

			for (int32 Offset = 1; Offset <= ContinuationCount; ++Offset)
			{
				if (!IsContinuationByte(Bytes[Index + Offset]))
				{
					return false;
				}
			}

			const uint8 Second = Bytes[Index + 1];
			if ((Lead == 0xe0u && Second < 0xa0u)
				|| (Lead == 0xedu && Second > 0x9fu)
				|| (Lead == 0xf0u && Second < 0x90u)
				|| (Lead == 0xf4u && Second > 0x8fu))
			{
				return false;
			}

			Index += ContinuationCount + 1;
		}

		return false;
	}
}

FSceneAutoTaggerNativeCoreAdapter::FSceneAutoTaggerNativeCoreAdapter(FString InExplicitDllPath)
	: ExplicitDllPath(MoveTemp(InExplicitDllPath))
{
}

FSceneAutoTaggerNativeCoreAdapter::~FSceneAutoTaggerNativeCoreAdapter()
{
	Shutdown();
}

FSceneAutoTaggerNativeCoreAdapter& FSceneAutoTaggerNativeCoreAdapter::Get()
{
	static FSceneAutoTaggerNativeCoreAdapter Instance;
	return Instance;
}

bool FSceneAutoTaggerNativeCoreAdapter::Initialize()
{
	if (IsAvailable())
	{
		return true;
	}

	Shutdown();

#if !PLATFORM_WINDOWS
	SetUnavailable(TEXT("Scene Auto Tagger native analysis is currently available only on Win64."));
	return false;
#else
	if (!ExplicitDllPath.IsEmpty() && FPaths::IsRelative(ExplicitDllPath))
	{
		SetUnavailable(TEXT("Scene Auto Tagger native analysis requires an absolute DLL path."));
		return false;
	}

	LoadedDllPath = ResolveDllPath();
	if (LoadedDllPath.IsEmpty())
	{
		SetUnavailable(TEXT("The Convai plugin directory could not be resolved for Scene Auto Tagger native analysis."));
		return false;
	}

	if (!IFileManager::Get().FileExists(*LoadedDllPath))
	{
		SetUnavailable(FString::Printf(
			TEXT("Scene Auto Tagger native analysis is unavailable because its core DLL is missing: %s"),
			*LoadedDllPath));
		return false;
	}

	DllHandle = FPlatformProcess::GetDllHandle(*LoadedDllPath);
	if (DllHandle == nullptr)
	{
		SetUnavailable(FString::Printf(
			TEXT("Scene Auto Tagger native analysis could not load its core DLL: %s"),
			*LoadedDllPath));
		return false;
	}

	using FGetApiFunction = const void* (CONVAI_SAT_CALL *)(uint32_t);
	const FGetApiFunction GetApiFunction = reinterpret_cast<FGetApiFunction>(
		FPlatformProcess::GetDllExport(DllHandle, TEXT("convai_sat_get_api")));
	if (GetApiFunction == nullptr)
	{
		SetUnavailable(TEXT("Scene Auto Tagger native analysis found an incompatible core DLL (missing API entry point)."));
		return false;
	}

	const convai_sat_api_v4* CandidateApi = static_cast<const convai_sat_api_v4*>(
		GetApiFunction(CONVAI_SAT_ABI_VERSION_4));
	FString ValidationDiagnostic;
	if (!ValidateApi(CandidateApi, ValidationDiagnostic))
	{
		SetUnavailable(MoveTemp(ValidationDiagnostic));
		return false;
	}

	Api = CandidateApi;
	Diagnostic.Reset();
	return true;
#endif
}

void FSceneAutoTaggerNativeCoreAdapter::Shutdown()
{
	Api = nullptr;
	if (DllHandle != nullptr)
	{
		FPlatformProcess::FreeDllHandle(DllHandle);
		DllHandle = nullptr;
	}

	LoadedDllPath.Reset();
	Diagnostic.Reset();
}

FString FSceneAutoTaggerNativeCoreAdapter::ResolveDllPath() const
{
	FString Result = ExplicitDllPath;
	if (Result.IsEmpty())
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Convai"));
		if (!Plugin.IsValid())
		{
			return FString();
		}

		const FString SourceDllPath = FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Source"),
			TEXT("ThirdParty"),
			TEXT("ConvaiSceneAutoTaggerCore"),
			TEXT("Win64"),
			TEXT("bin"),
			TEXT("convai_scene_auto_tagger_core.dll"));
		const FString StagedDllPath = FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Binaries"),
			TEXT("ThirdParty"),
			TEXT("ConvaiSceneAutoTaggerCore"),
			TEXT("Win64"),
			TEXT("convai_scene_auto_tagger_core.dll"));
		Result = IFileManager::Get().FileExists(*SourceDllPath)
			? SourceDllPath
			: StagedDllPath;
	}

	Result = FPaths::ConvertRelativePathToFull(Result);
	FPaths::NormalizeFilename(Result);
	return Result;
}

bool FSceneAutoTaggerNativeCoreAdapter::ValidateApi(
	const convai_sat_api_v4* InApi,
	FString& OutDiagnostic) const
{
	if (InApi == nullptr)
	{
		OutDiagnostic = TEXT("Scene Auto Tagger native analysis found an incompatible core DLL (ABI 4 is unsupported).");
		return false;
	}

	if (InApi->struct_size < sizeof(convai_sat_api_v4)
		|| InApi->abi_version != CONVAI_SAT_ABI_VERSION_4)
	{
		OutDiagnostic = TEXT("Scene Auto Tagger native analysis found an incompatible core DLL (invalid ABI table).");
		return false;
	}

	if (!IsValidBoundedUtf8(InApi->core_version_utf8)
		|| !IsValidBoundedUtf8(InApi->view_policy_id_utf8)
		|| !IsValidBoundedUtf8(InApi->vision_protocol_id_utf8))
	{
		OutDiagnostic = TEXT("Scene Auto Tagger native analysis found an incompatible core DLL (invalid identity metadata).");
		return false;
	}

	if (InApi->build_view_plan == nullptr
		|| InApi->select_view == nullptr
		|| InApi->evaluate_capture == nullptr
		|| InApi->compare_evidence == nullptr
		|| InApi->vision_autotag == nullptr
		|| InApi->vision_request == nullptr
		|| InApi->vision_result_view == nullptr
		|| InApi->vision_result_free == nullptr)
	{
		OutDiagnostic = TEXT("Scene Auto Tagger native analysis found an incompatible core DLL (incomplete function table).");
		return false;
	}

	return true;
}

FString FSceneAutoTaggerNativeCoreAdapter::GetVisionProtocolId() const
{
	return Api != nullptr
		? FString(UTF8_TO_TCHAR(Api->vision_protocol_id_utf8))
		: FString();
}

void FSceneAutoTaggerNativeCoreAdapter::SetUnavailable(FString InDiagnostic)
{
	Api = nullptr;
	Diagnostic = MoveTemp(InDiagnostic);
	if (DllHandle != nullptr)
	{
		FPlatformProcess::FreeDllHandle(DllHandle);
		DllHandle = nullptr;
	}
}
