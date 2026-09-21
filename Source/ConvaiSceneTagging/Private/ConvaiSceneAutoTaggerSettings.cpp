// Copyright Convai. All Rights Reserved.

#include "ConvaiSceneAutoTaggerSettings.h"

#include "Misc/ConfigCacheIni.h"
#include "SceneAutoTaggerVisionProtocol.h"
#include "UObject/UnrealType.h"

namespace
{
void MigrateLegacyAutoTaggerConfig(UObject* Object, const TCHAR* LegacySection)
{
	if (!Object || !Object->HasAnyFlags(RF_ClassDefaultObject) || !GConfig)
	{
		return;
	}

	const FString NewSection = Object->GetClass()->GetPathName();
	if (GConfig->DoesSectionExist(*NewSection, GEditorPerProjectIni))
	{
		return;
	}

	const FConfigSection* OldValues = GConfig->GetSection(
		LegacySection,
		false,
		GEditorPerProjectIni);
	if (!OldValues)
	{
		return;
	}

	for (TFieldIterator<FProperty> PropertyIt(Object->GetClass()); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		if (!Property->HasAnyPropertyFlags(CPF_Config))
		{
			continue;
		}
		if (const FConfigValue* Value = OldValues->Find(Property->GetFName()))
		{
			Property->ImportText_Direct(
				*Value->GetValue(),
				Property->ContainerPtrToValuePtr<void>(Object),
				Object,
				PPF_None);
		}
	}
	Object->SaveConfig();
}
}

UConvaiSceneAutoTaggerSettings::UConvaiSceneAutoTaggerSettings()
{
	ExcludedNameFragments = {
		TEXT("floor"), TEXT("wall"), TEXT("ceiling"), TEXT("roof"), TEXT("baseboard"),
		TEXT("trim"), TEXT("navmesh"), TEXT("recast"), TEXT("collision"), TEXT("blockingvolume"),
		TEXT("postprocess"), TEXT("skylight"), TEXT("rectlight"), TEXT("spotlight")
	};

	ImportantNameFragments = {
		TEXT("artifact"), TEXT("artwork"), TEXT("bust"), TEXT("display"), TEXT("exhibit"),
		TEXT("painting"), TEXT("plaque"), TEXT("portrait"), TEXT("sculpture"), TEXT("statue"),
		TEXT("screen"), TEXT("relic"), TEXT("monument")
	};
}

int32 UConvaiSceneAutoTaggerSettings::GetEffectiveGridDimension() const
{
    return IsAccurateAnalysisEnabled() ? 3 : 4;
}

int32 UConvaiSceneAutoTaggerSettings::GetEffectiveCellResolution() const
{
    return IsAccurateAnalysisEnabled() ? 512 : DefaultCellResolution;
}

int32 UConvaiSceneAutoTaggerSettings::GetEffectiveEvidencePreviewResolution() const
{
	const int32 MinimumResolution = IsAccurateAnalysisEnabled()
		? MinimumAccurateEvidencePreviewResolution
		: MinimumFastEvidencePreviewResolution;
	return FMath::Max(
		MinimumResolution,
		FMath::Clamp(EvidencePreviewResolution, 256, 1024));
}

bool UConvaiSceneAutoTaggerSettings::IsAccurateAnalysisEnabled() const
{
    return AnalysisDetail == EConvaiSceneAutoTaggerAnalysisDetail::Detailed;
}

EConvaiSceneAutoTaggerDuplicateGroupingMode
UConvaiSceneAutoTaggerSettings::GetEffectiveDuplicateGroupingMode() const
{
	if (DuplicateGroupingMode != EConvaiSceneAutoTaggerDuplicateGroupingMode::Legacy
		&& DuplicateGroupingMode != EConvaiSceneAutoTaggerDuplicateGroupingMode::KeepSeparate
		&& DuplicateGroupingMode != EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters
		&& DuplicateGroupingMode != EConvaiSceneAutoTaggerDuplicateGroupingMode::MergeAllVerified)
	{
		return EConvaiSceneAutoTaggerDuplicateGroupingMode::KeepSeparate;
	}

	// Non-default modern values can also be selected transiently before SaveConfig.
	if (DuplicateGroupingMode == EConvaiSceneAutoTaggerDuplicateGroupingMode::KeepSeparate
		|| DuplicateGroupingMode == EConvaiSceneAutoTaggerDuplicateGroupingMode::MergeAllVerified)
	{
		return DuplicateGroupingMode;
	}

	const FString SettingsSection = UConvaiSceneAutoTaggerSettings::StaticClass()->GetPathName();
	FString PersistedModernMode;
	const bool bHasPersistedModernMode = GConfig
		&& GConfig->GetString(
			*SettingsSection,
			GET_MEMBER_NAME_STRING_CHECKED(
				UConvaiSceneAutoTaggerSettings,
				DuplicateGroupingMode),
			PersistedModernMode,
			GEditorPerProjectIni);
	if (bHasPersistedModernMode
		&& DuplicateGroupingMode == EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters)
	{
		return DuplicateGroupingMode;
	}

	// No modern selection was saved. Preserve the old combine bit only when its
	// key actually exists; the C++ compatibility initializer is not a user choice.
	bool bPersistedLegacyMerge = false;
	if (GConfig
		&& GConfig->GetBool(
			*SettingsSection,
			GET_MEMBER_NAME_STRING_CHECKED(
				UConvaiSceneAutoTaggerSettings,
				bMergeDuplicateGeometry),
			bPersistedLegacyMerge,
			GEditorPerProjectIni))
	{
		return bPersistedLegacyMerge
			? EConvaiSceneAutoTaggerDuplicateGroupingMode::MergeAllVerified
			: EConvaiSceneAutoTaggerDuplicateGroupingMode::KeepSeparate;
	}

	return EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters;
}

void UConvaiSceneAutoTaggerSettings::PostInitProperties()
{
	Super::PostInitProperties();
	MigrateLegacyAutoTaggerConfig(
		this,
		TEXT("/Script/ConvaiSceneAutoTagger.ConvaiSceneAutoTaggerSettings"));
}

FString UConvaiSceneAutoTaggerUserState::GetSceneContextForMap(const FString& MapPackageName) const
{
	if (const FString* Found = SceneContextsByMap.Find(MapPackageName))
	{
		return *Found;
	}
	return FString();
}

void UConvaiSceneAutoTaggerUserState::SetSceneContextForMap(
	const FString& MapPackageName,
	const FString& SceneContext)
{
	if (!MapPackageName.StartsWith(TEXT("/Game/")))
	{
		return;
	}
	const FString Normalized = ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(SceneContext);
	if (Normalized.IsEmpty())
	{
		SceneContextsByMap.Remove(MapPackageName);
	}
	else
	{
		SceneContextsByMap.Add(MapPackageName, Normalized);
	}
	SaveConfig();
}

FString UConvaiSceneAutoTaggerUserState::GetDescriptionFocusForMap(const FString& MapPackageName) const
{
	if (const FString* Found = DescriptionFocusByMap.Find(MapPackageName))
	{
		return *Found;
	}
	return FString();
}

void UConvaiSceneAutoTaggerUserState::SetDescriptionFocusForMap(
	const FString& MapPackageName,
	const FString& DescriptionFocus)
{
	if (!MapPackageName.StartsWith(TEXT("/Game/")))
	{
		return;
	}
	const FString Normalized =
		ConvaiSceneAutoTagger::VisionProtocol::NormalizeDescriptionFocus(DescriptionFocus);
	if (Normalized.IsEmpty())
	{
		DescriptionFocusByMap.Remove(MapPackageName);
	}
	else
	{
		DescriptionFocusByMap.Add(MapPackageName, Normalized);
	}
	SaveConfig();
}

FString UConvaiSceneAutoTaggerUserState::GetLastSelectedVisionCharacterID() const
{
	return LastSelectedVisionCharacterID.TrimStartAndEnd();
}

void UConvaiSceneAutoTaggerUserState::SetLastSelectedVisionCharacterID(
	const FString& CharacterID)
{
	const FString Normalized = CharacterID.TrimStartAndEnd();
	if (LastSelectedVisionCharacterID == Normalized)
	{
		return;
	}
	LastSelectedVisionCharacterID = Normalized;
	SaveConfig();
}

bool UConvaiSceneAutoTaggerUserState::HasAcceptedPrivacyConsent(const FString& ConsentKey) const
{
	return !ConsentKey.IsEmpty() && AcceptedPrivacyConsentKeys.Contains(ConsentKey);
}

void UConvaiSceneAutoTaggerUserState::PostInitProperties()
{
	Super::PostInitProperties();
	MigrateLegacyAutoTaggerConfig(
		this,
		TEXT("/Script/ConvaiSceneAutoTagger.ConvaiSceneAutoTaggerUserState"));
}

void UConvaiSceneAutoTaggerUserState::SetPrivacyConsentAccepted(
	const FString& ConsentKey,
	const bool bAccepted)
{
	if (ConsentKey.IsEmpty())
	{
		return;
	}
	if (bAccepted)
	{
		AcceptedPrivacyConsentKeys.Add(ConsentKey);
	}
	else
	{
		AcceptedPrivacyConsentKeys.Remove(ConsentKey);
	}
	// EditorPerProjectUserSettings writes under the user's Saved config hierarchy;
	// accepting the disclosure never dirties project content or source-controlled defaults.
	SaveConfig();
}
