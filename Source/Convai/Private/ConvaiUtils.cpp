// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiUtils.h"
#include "Misc/FileHelper.h"
#include "Http.h"
#include "Containers/UnrealString.h"
#include "Containers/Map.h"
#include "Sound/SoundWave.h"
#include "Memory/SharedBuffer.h"
#include "AudioDevice.h"
#include "Interfaces/IAudioFormat.h"
#include "UObject/Object.h"
#include "GameFramework/PlayerController.h"
#include "Math/Vector.h"
#include "Camera/PlayerCameraManager.h"
#include "UObject/UObjectHash.h"
#include "Math/UnrealMathUtility.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/App.h"
#if WITH_EDITOR
#include "Engine/World.h"  // GPlayInEditorID
#endif
#if PLATFORM_LINUX
	#include "Linux/LinuxPlatformFile.h"
#else
	#include "HAL/PlatformFileManager.h"
#endif
#include "Engine/GameInstance.h"
#include "ConvaiSubsystem.h"
#include "Engine/GameEngine.h"
#include "GameFramework/Pawn.h"
#include "AudioDecompress.h"
#include "Runtime/Launch/Resources/Version.h"

// JSON field name macro for UE 5.4+ TCHAR requirement
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 4
	#define JSON_FIELD(name) TEXT(name)
#else
	#define JSON_FIELD(name) name
#endif

#include "../Convai.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiObjectComponent.h"

#include "Interfaces/IPluginManager.h"
#include "Engine/EngineTypes.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "UObject/FieldIterator.h"

#if ENGINE_MAJOR_VERSION == 5
#include "AudioDecompress.h"
#endif

DEFINE_LOG_CATEGORY(ConvaiUtilsLog);
DEFINE_LOG_CATEGORY(ConvaiFormValidationLog);

namespace
{
	FString GetAbsolutePathFromFilePath(const FString& FilePath)
	{
		FString ProcessedPath;
		// Check if path is relative
		if (FPaths::IsRelative(FilePath))
		{
			// Combine the build directory with the relative file path
			ProcessedPath = FPaths::Combine(FPaths::LaunchDir(), FilePath);
		}
		else
		{
			// Path is absolute, use as is
			ProcessedPath = FilePath;
		}
		return ProcessedPath;
	}
	
	const TMap<FString, FString>& GetDefaultCustomParams()
	{
		static const TMap<FString, FString> Defaults = {
			{TEXT("LLMProvider"),          TEXT("dynamic")},
			{TEXT("ConnectionType"),       TEXT("audio")},
			{TEXT("AEC"),                  TEXT("1")},
			{TEXT("VAD"),                  TEXT("1")},
			{TEXT("AECType"),              TEXT("Internal")},
			{TEXT("ReferenceCaptureTap"),  TEXT("Listener")},
			{TEXT("MicCaptureTap"),        TEXT("Listener")},
			{TEXT("AECStreamDelayMs"),     TEXT("")},
			{TEXT("NoiseSuppression"),     TEXT("1")},
			{TEXT("GainControl"),          TEXT("1")},
			{TEXT("VADMode"),              TEXT("3")},
			{TEXT("HighPassFilter"),       TEXT("1")},
			{TEXT("ChunkSize"),            TEXT("10")},
			{TEXT("OutputFPS"),            TEXT("90")},
			{TEXT("FramesBufferDuration"), TEXT("0.5")},
			{TEXT("LipSyncTimeOffset"),    TEXT("0.02")},
			{TEXT("EmotionsProvider"),     TEXT("nrclex")},
		    {TEXT("ConnectionProxyTTL"),     TEXT("-1.0f")},
		    {TEXT("PrepConnectionTTL"),      TEXT("120.0f")},
			{TEXT("LipSyncStarvationFallback"),     TEXT("5.0f")},

			// ConvaiObjectComponent shared poll clock (seconds) — the cadence at
			// which all registered Object Components are evaluated for tracked-property
			// changes. Treated as a plain tick rate; the chatbot's debounce window
			// already coalesces same-tick updates so no relative floor is enforced.
			{TEXT("ObjectPollIntervalSeconds"),         TEXT("0.25")},
			
			{TEXT("ClientReadyRetrySecs"),              TEXT("0.5f")},
			{TEXT("ClientReadyTimeoutSecs"),            TEXT("45.0f")},

			// Defaults for newly-constructed ConvaiChatbotComponents (seconds).
			// Per-instance UPROPERTY still overrides these on existing instances.
			{TEXT("ContextDebounceWindow"),             TEXT("0.5")},
			{TEXT("ContextMaxDebounceWindow"),          TEXT("3.0")},

			// FaceSync component
			{TEXT("ConvaiFaceSyncSimulateFreeze"),      TEXT("0")},

			// LipSync anim node overrides — empty string means "not set, fall back to UPROPERTY"
			{TEXT("LipSyncAnimUpperFaceAlpha"),           TEXT("")},
			{TEXT("LipSyncAnimLowerFaceAlpha"),           TEXT("")},
			{TEXT("LipSyncAnimStarvationBlendDuration"),  TEXT("")},
			{TEXT("LipSyncAnimGlobalMultiplier"),         TEXT("")},
			{TEXT("LipSyncAnimGlobalOffset"),             TEXT("")},
			{TEXT("LipSyncAnimLowerFaceSmoothingSpeed"),  TEXT("")},
			{TEXT("LipSyncAnimUpperFaceSmoothingSpeed"),  TEXT("")},
			{TEXT("LipSyncAnimEnableLowerFaceSmoothing"), TEXT("")},
			{TEXT("LipSyncAnimEnableUpperFaceSmoothing"), TEXT("")},
			{TEXT("LipSyncAnimBypassSmoothing"),          TEXT("")},
			{TEXT("LipSyncAnimBypassStarvationBlend"),    TEXT("")},
			{TEXT("LipSyncAnimApplyMode"),                TEXT("")},
		};
		return Defaults;
	}

	FString ResolveCustomParam(const TCHAR* Key)
	{
		const auto& Defaults = GetDefaultCustomParams();
		const FString* DefaultPtr = Defaults.Find(Key);
		FString Value = DefaultPtr ? *DefaultPtr : TEXT("");

		if (const FString* FoundValue = Convai::Get().GetConvaiSettings()->CustomPrams.Find(Key))
		{
			Value = *FoundValue;
		}

		return UCommandLineUtils::GetCommandLineFlagValueAsString(Key, Value);
	}
};

UConvaiSubsystem* UConvaiUtils::GetConvaiSubsystem(const UObject* WorldContextObject)
{
	//UWorld* World = WorldPtr.Get();

	if (!WorldContextObject)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("WorldContextObject ptr is invalid!"));
		return nullptr;
	}

	const UGameInstance* GameInstance = UGameplayStatics::GetGameInstance(WorldContextObject);
	if (!GameInstance)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("Could not get pointer to a GameInstance"));
		return nullptr;
	}


	if (UConvaiSubsystem* ConvaiSubsystem = GameInstance->GetSubsystem<UConvaiSubsystem>())
	{
		return ConvaiSubsystem;
	}
	else
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("Could not get pointer to Convai Subsystem"));
		return nullptr;
	}

}

void UConvaiUtils::StereoToMono(TArray<uint8> stereoWavBytes, TArray<uint8>& monoWavBytes)
{
	//Change wav headers
	for (int i = 0; i < 44; i++)
	{
		//NumChannels starts from 22 to 24
		if (i == 22)
		{
			short NumChannels = (*(short*)&stereoWavBytes[i]);
			//CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("NumChannels %d"), NumChannels);
			if (NumChannels == 1)
			{
				monoWavBytes = stereoWavBytes;
				return;
			}
			NumChannels = 1;
			monoWavBytes.Append((uint8*)&NumChannels, sizeof(NumChannels));
			i++;
		}
		//ByteRate starts from 28 to 32
		else if (i == 28)
		{
			int ByteRate = (*(int*)&stereoWavBytes[i]) / 2;
			monoWavBytes.Append((uint8*)&ByteRate, sizeof(ByteRate));
			i += 3;
		}
		//BlockAlign starts from 32 to 34
		else if (i == 32)
		{
			short BlockAlign = (*(short*)&stereoWavBytes[i]) / 2;
			monoWavBytes.Append((uint8*)&BlockAlign, sizeof(BlockAlign));
			i++;
		}
		//SubChunkSize starts from 40 to 44
		else if (i == 40)
		{
			int SubChunkSize = (*(int*)&stereoWavBytes[i]) / 2;
			monoWavBytes.Append((uint8*)&SubChunkSize, sizeof(SubChunkSize));
			i += 3;
		}
		else
		{
			monoWavBytes.Add(stereoWavBytes[i]);
		}
	}

	//Copies only the left channel and ignores the right channel
	for (int i = 44; i < stereoWavBytes.Num(); i += 4)
	{
		monoWavBytes.Add(stereoWavBytes[i]);
		monoWavBytes.Add(stereoWavBytes[i + 1]);
	}
}

bool UConvaiUtils::ReadFileAsByteArray(const FString FilePath, TArray<uint8>& Bytes)
{
	FString ProcessedFilePath = GetAbsolutePathFromFilePath(FilePath);
	return FFileHelper::LoadFileToArray(Bytes, *ProcessedFilePath, 0);
}

bool UConvaiUtils::SaveByteArrayAsFile(FString FilePath, TArray<uint8> Bytes)
{
	return FFileHelper::SaveArrayToFile(Bytes, *FilePath);
}

FString UConvaiUtils::ByteArrayToString(TArray<uint8> Bytes)
{
	FString s = BytesToString(Bytes.GetData(), Bytes.Num());
	FString Fixed;

	for (int i = 0; i < s.Len(); i++)
	{
		const TCHAR c = *(*s + i) - 1;
		Fixed.AppendChar(c);
	}
	return Fixed;
}

bool UConvaiUtils::WriteStringToFile(const FString& StringToWrite, const FString& FilePath)
{
	return FFileHelper::SaveStringToFile(StringToWrite, *FilePath);
}

bool UConvaiUtils::ReadStringFromFile(FString& OutString, const FString& FilePath)
{
	FString ProcessedFilePath = GetAbsolutePathFromFilePath(FilePath);
	return FFileHelper::LoadFileToString(OutString, *ProcessedFilePath);
}

double UConvaiUtils::CalculateAudioDuration(uint32 AudioSize, uint8 Channels, uint32 SampleRate, uint8 SampleSize)
{
	if (Channels == 0 || SampleRate == 0 || SampleSize == 0)
	{
		// Avoid division by zero
		return 0;
	}

	// Calculate the duration in seconds
	return static_cast<double>(AudioSize) / static_cast<double>(Channels * SampleRate * SampleSize);
}

void UConvaiUtils::ConvaiGetLookedAtCharacter(UObject* WorldContextObject, APlayerController* PlayerController, float Radius, bool PlaneView, TArray<UObject*> IncludedCharacters, TArray<UObject*> ExcludedCharacters, UConvaiChatbotComponent*& ConvaiCharacter, bool& Found)
{
	Found = false;
	float FocuseDotThresshold = 0.5;
	FVector CameraLocation, CameraForward;

	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("Could not get a pointer to world!"));
		return;
	}

	if (!PlayerController)
	{
		PlayerController = UGameplayStatics::GetPlayerController(WorldContextObject, 0);
	}

	if (!PlayerController)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("GetLookedAtCharacter: Could not get a pointer to PlayerController"));
		return;
	}

	if (PlayerController->PlayerCameraManager)
	{
		CameraLocation = PlayerController->PlayerCameraManager->GetCameraLocation();
		CameraForward = PlayerController->PlayerCameraManager->GetTransformComponent()->GetForwardVector();
	}
	else if (PlayerController->GetPawn())
	{
		CameraLocation = PlayerController->GetPawn()->GetActorLocation();
		CameraForward = PlayerController->GetPawn()->GetActorForwardVector();
	}
	else
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("GetLookedAtCharacter: Could not get a camera location"));
		return;
	}


	if (PlaneView)
	{
		CameraLocation.Z = 0;
		CameraForward.Z = 0;
		CameraForward.Normalize();
	}

	TArray<UObject*> ConvaiCharacters;
	GetObjectsOfClass(UConvaiChatbotComponent::StaticClass(), ConvaiCharacters, true, RF_ClassDefaultObject);

	for (int32 CharacterIndex = 0; CharacterIndex < ConvaiCharacters.Num(); ++CharacterIndex)
	{
		UConvaiChatbotComponent* CurrentConvaiCharacter = Cast<UConvaiChatbotComponent>(ConvaiCharacters[CharacterIndex]);
		check(CurrentConvaiCharacter);

		if (!IsValid(CurrentConvaiCharacter))
			continue;


		AActor* Owner = CurrentConvaiCharacter->GetOwner();

		if (Owner == nullptr || CurrentConvaiCharacter->GetWorld() != World)
			continue;

		bool Exclude = false;
		bool Include = false;

		for (UObject* CharacterToExclude : ExcludedCharacters)
		{
			if (!IsValid(CharacterToExclude))
				continue;

			if (CharacterToExclude == CurrentConvaiCharacter || CharacterToExclude == Owner)
			{
				Exclude = true;
				break;
			}
		}

		if (IncludedCharacters.Num())
		{
			for (UObject* CharacterToInclude : IncludedCharacters)
			{
				if (!IsValid(CharacterToInclude))
					continue;

				if (CharacterToInclude == CurrentConvaiCharacter || CharacterToInclude == Owner)
				{
					Include = true;
					break;
				}
			}
		}
		else
		{
			Include = true;
		}

		if (Exclude || !Include)
		{
			continue;
		}
		float DistSquared = 0;
		float DistSquared2D = 0;
		FVector CurrentCharacterLocation = CurrentConvaiCharacter->GetComponentLocation();
		if (PlaneView)
		{
			DistSquared2D = FVector::DistSquared2D(CurrentCharacterLocation, CameraLocation);
			if (Radius > 0 && DistSquared2D > Radius * Radius)
				continue;
		}
		else
		{
			DistSquared = FVector::DistSquared(CurrentCharacterLocation, CameraLocation);
			if (Radius > 0 && DistSquared > Radius * Radius)
				continue;
		}

		FVector DirCameraToCharacter = CurrentCharacterLocation - CameraLocation;
		if (PlaneView)
			DirCameraToCharacter.Z = 0;


		DirCameraToCharacter.Normalize();
		float CurrentFocuseDot = FVector::DotProduct(DirCameraToCharacter, CameraForward);
		float mxnScore = -1;
		float score = 0;

		if (PlaneView) {
			score = CurrentFocuseDot / DistSquared2D;
		}
		else {
			score = CurrentFocuseDot / DistSquared;
		}

		if (score > mxnScore && CurrentFocuseDot >= FocuseDotThresshold)
		{
			mxnScore = score;
			FocuseDotThresshold = CurrentFocuseDot;
			ConvaiCharacter = CurrentConvaiCharacter;
			Found = true;
			//CONVAI_LOG(ConvaiUtilsLog, Log, TEXT("GetLookedAtCharacter: Found! %s = %f"), *CurrentConvaiCharacter->GetFullName(), FocuseDotThresshold);

		}
	}
}

void UConvaiUtils::ConvaiGetLookedAtObjectOrCharacter(UObject* WorldContextObject, APlayerController* PlayerController, float Radius, bool PlaneView, TArray<FConvaiObjectEntry> ListToSearchIn, FConvaiObjectEntry& FoundObjectOrCharacter, bool& Found)
{
	Found = false;
	float FocuseDotThresshold = 0.5;
	FVector CameraLocation, CameraForward;

	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("Could not get a pointer to world!"));
		return;
	}

	if (!PlayerController)
	{
		PlayerController = UGameplayStatics::GetPlayerController(WorldContextObject, 0);
	}

	if (!PlayerController)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("ConvaiGetLookedAtActor: Could not get a pointer to PlayerController"));
		return;
	}

	if (PlayerController->PlayerCameraManager)
	{
		CameraLocation = PlayerController->PlayerCameraManager->GetCameraLocation();
		CameraForward = PlayerController->PlayerCameraManager->GetTransformComponent()->GetForwardVector();
	}
	else if (PlayerController->GetPawn())
	{
		CameraLocation = PlayerController->GetPawn()->GetActorLocation();
		CameraForward = PlayerController->GetPawn()->GetActorForwardVector();
	}
	else
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("ConvaiGetLookedAtActor: Could not get a camera location"));
		return;
	}


	if (PlaneView)
	{
		CameraLocation.Z = 0;
		CameraForward.Z = 0;
		CameraForward.Normalize();
	}


	for (int32 ItemIndex = 0; ItemIndex < ListToSearchIn.Num(); ++ItemIndex)
	{
		FConvaiObjectEntry CurrentItem = ListToSearchIn[ItemIndex];

		TWeakObjectPtr<AActor> CurrentItemRef = CurrentItem.Ref;

		if (!CurrentItemRef.IsValid())
			continue;

		if (CurrentItemRef->GetWorld() != World)
			continue;

		float DistSquared = 0;
		float DistSquared2D = 0;
		FVector CurrentItemLocation = CurrentItemRef->GetActorLocation();
		if (PlaneView)
		{
			DistSquared2D = FVector::DistSquared2D(CurrentItemLocation, CameraLocation);
			if (Radius > 0 && DistSquared2D > Radius * Radius)
				continue;
		}
		else
		{
			DistSquared = FVector::DistSquared(CurrentItemLocation, CameraLocation);
			if (Radius > 0 && DistSquared > Radius * Radius)
				continue;
		}

		FVector DirCameraToItem = CurrentItemLocation - CameraLocation;
		if (PlaneView)
			DirCameraToItem.Z = 0;

		DirCameraToItem.Normalize();
		float CurrentFocuseDot = FVector::DotProduct(DirCameraToItem, CameraForward);
		float mxnScore = -1;
		float score = 0;

		if (PlaneView) {
			score = CurrentFocuseDot / DistSquared2D;
		}
		else {
			score = CurrentFocuseDot / DistSquared;
		}

		if (score > mxnScore && CurrentFocuseDot >= FocuseDotThresshold)
		{
			mxnScore = score;
			FocuseDotThresshold = CurrentFocuseDot;
			FoundObjectOrCharacter = CurrentItem;
			Found = true;
		}
	}
}

void UConvaiUtils::ConvaiGetAllPlayerComponents(UObject* WorldContextObject, bool bOnlyConnected, TArray<class UConvaiPlayerComponent*>& ConvaiPlayerComponents, TArray<AActor*>& OutOwners)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("Could not get a pointer to world!"));
		return;
	}

	ConvaiPlayerComponents.Empty();
	OutOwners.Empty();

	TArray<UObject*> ConvaiPlayerComponentsObjects;
	GetObjectsOfClass(UConvaiPlayerComponent::StaticClass(), ConvaiPlayerComponentsObjects, true, RF_ClassDefaultObject);

	for (int32 Index = 0; Index < ConvaiPlayerComponentsObjects.Num(); ++Index)
	{
		UConvaiPlayerComponent* CurrentConvaiPlayer = Cast<UConvaiPlayerComponent>(ConvaiPlayerComponentsObjects[Index]);

		if (!IsValid(CurrentConvaiPlayer))
			continue;

		AActor* Owner = CurrentConvaiPlayer->GetOwner();

		if (Owner == nullptr || CurrentConvaiPlayer->GetWorld() != World)
			continue;

		if (bOnlyConnected && !CurrentConvaiPlayer->IsPlayerConnected())
			continue;

		ConvaiPlayerComponents.Add(CurrentConvaiPlayer);
		OutOwners.Add(Owner);
	}
}

void UConvaiUtils::ConvaiGetAllChatbotComponents(UObject* WorldContextObject, bool bOnlyConnected, TArray<class UConvaiChatbotComponent*>& ConvaiChatbotComponents, TArray<AActor*>& OutOwners)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("Could not get a pointer to world!"));
		return;
	}

	ConvaiChatbotComponents.Empty();
	OutOwners.Empty();

	TArray<UObject*> ConvaiChatbotComponentsObjects;
	GetObjectsOfClass(UConvaiChatbotComponent::StaticClass(), ConvaiChatbotComponentsObjects, true, RF_ClassDefaultObject);

	for (int32 Index = 0; Index < ConvaiChatbotComponentsObjects.Num(); ++Index)
	{
		UConvaiChatbotComponent* CurrentConvaiChatbot = Cast<UConvaiChatbotComponent>(ConvaiChatbotComponentsObjects[Index]);

		if (!IsValid(CurrentConvaiChatbot))
			continue;

		AActor* Owner = CurrentConvaiChatbot->GetOwner();

		if (Owner == nullptr || CurrentConvaiChatbot->GetWorld() != World)
			continue;

		if (bOnlyConnected && CurrentConvaiChatbot->GetChatbotConnectionState() != EC_ConnectionState::Connected)
			continue;

		ConvaiChatbotComponents.Add(CurrentConvaiChatbot);
		OutOwners.Add(Owner);
	}
}

UConvaiPlayerComponent* UConvaiUtils::GetFirstConvaiPlayerComponent(UObject* WorldContextObject, bool bOnlyConnected, AActor*& OutOwner)
{
	TArray<UConvaiPlayerComponent*> Components;
	TArray<AActor*> Owners;
	ConvaiGetAllPlayerComponents(WorldContextObject, bOnlyConnected, Components, Owners);
	OutOwner = Owners.Num() > 0 ? Owners[0] : nullptr;
	return Components.Num() > 0 ? Components[0] : nullptr;
}

UConvaiChatbotComponent* UConvaiUtils::GetFirstConvaiChatbotComponent(UObject* WorldContextObject, bool bOnlyConnected, AActor*& OutOwner)
{
	TArray<UConvaiChatbotComponent*> Components;
	TArray<AActor*> Owners;
	ConvaiGetAllChatbotComponents(WorldContextObject, bOnlyConnected, Components, Owners);
	OutOwner = Owners.Num() > 0 ? Owners[0] : nullptr;
	return Components.Num() > 0 ? Components[0] : nullptr;
}

void UConvaiUtils::ConvaiGetAllObjectComponents(UObject* WorldContextObject, TArray<class UConvaiObjectComponent*>& ConvaiObjectComponents, TArray<AActor*>& OutOwners)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("Could not get a pointer to world!"));
		return;
	}

	ConvaiObjectComponents.Empty();
	OutOwners.Empty();

	UConvaiSubsystem* Subsystem = GetConvaiSubsystem(WorldContextObject);
	if (!Subsystem)
	{
		return;
	}

	for (UConvaiObjectComponent* Component : Subsystem->GetAllObjectComponents())
	{
		if (!IsValid(Component))
			continue;

		AActor* Owner = Component->GetOwner();
		if (Owner == nullptr || Component->GetWorld() != World)
			continue;

		ConvaiObjectComponents.Add(Component);
		OutOwners.Add(Owner);
	}
}

UConvaiObjectComponent* UConvaiUtils::GetFirstConvaiObjectComponent(UObject* WorldContextObject, AActor*& OutOwner)
{
	TArray<UConvaiObjectComponent*> Components;
	TArray<AActor*> Owners;
	ConvaiGetAllObjectComponents(WorldContextObject, Components, Owners);
	OutOwner = Owners.Num() > 0 ? Owners[0] : nullptr;
	return Components.Num() > 0 ? Components[0] : nullptr;
}

void UConvaiUtils::SetAPI_Key(FString API_Key)
{
	Convai::Get().GetConvaiSettings()->API_Key = API_Key;
}

FString UConvaiUtils::GetAPI_Key()
{
	return Convai::Get().GetConvaiSettings()->API_Key;
}

void UConvaiUtils::SetAuthToken(FString AuthToken)
{
	Convai::Get().GetConvaiSettings()->AuthToken = AuthToken;
}

FString UConvaiUtils::GetAuthToken()
{
	return Convai::Get().GetConvaiSettings()->AuthToken;
}

void UConvaiUtils::SetCustomParam(const FString& Key, const FString& Value)
{
	Convai::Get().GetConvaiSettings()->CustomPrams.Add(Key, Value);
}

FString UConvaiUtils::GetCustomParam(const FString& Key, const FString& DefaultValue)
{
	FString Result = ResolveCustomParam(*Key);
	return Result.IsEmpty() ? DefaultValue : Result;
}

FString UConvaiUtils::GetClientVersion()
{
	// Check if the user has explicitly set a version via CustomPrams or command-line.
	const FString Resolved = ResolveCustomParam(TEXT("ClientVersion"));

	// Map build configuration to a short display string.
	auto GetBuildConfigString = []() -> FString
	{
		switch (FApp::GetBuildConfiguration())
		{
			case EBuildConfiguration::Debug:       return TEXT("Debug");
			case EBuildConfiguration::DebugGame:   return TEXT("DebugGame");
			case EBuildConfiguration::Development: return TEXT("Development");
			case EBuildConfiguration::Shipping:    return TEXT("Shipping");
			case EBuildConfiguration::Test:        return TEXT("Test");
			default:                               return TEXT("Unknown");
		}
	};

#if WITH_EDITOR
	// Running inside the UE editor (PIE). Cache one timestamp per PIE session so
	// that multiple connections within the same session share the same version string.
	// The PIE session ID increments each time a new PIE session starts.
	static int32 CachedPIEID = -2; // sentinel: session ID is -1 when not in PIE
	static FString CachedPIETimestamp;

	// UE 5.5 deprecated GPlayInEditorID in favour of UE::GetPlayInEditorID().
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 5
	const int32 CurrentPIEID = UE::GetPlayInEditorID();
#else
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	const int32 CurrentPIEID = static_cast<int32>(GPlayInEditorID);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif

	if (CurrentPIEID != CachedPIEID)
	{
		CachedPIEID = CurrentPIEID;
		CachedPIETimestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	}

	// Always append the PIE session timestamp so every build is traceable to a specific session.
	// Format: "PIE_<timestamp>"  or  "<resolved>_PIE_<timestamp>"
	return Resolved.IsEmpty()
		? FString::Printf(TEXT("PIE_%s"), *CachedPIETimestamp)
		: FString::Printf(TEXT("%s_PIE_%s"), *Resolved, *CachedPIETimestamp);
#else
	// Non-editor process: either a packaged (Shipping) game or a Standalone game
	// launched from the editor (Development / DebugGame build).
	const bool bIsShipping = (FApp::GetBuildConfiguration() == EBuildConfiguration::Shipping);
	const FString BuildConfig = GetBuildConfigString();

	FString BaseVersion;
	if (!Resolved.IsEmpty())
	{
		BaseVersion = Resolved;
	}
	else
	{
		// Use the compile-time build date as the packaged/standalone timestamp.
		FString BuildDate = FApp::GetBuildDate();
		if (BuildDate.IsEmpty())
		{
			BuildDate = TEXT("Unknown");
		}
		const TCHAR* Prefix = bIsShipping ? TEXT("Packaged") : TEXT("Standalone");
		BaseVersion = FString::Printf(TEXT("%s_%s"), Prefix, *BuildDate);
	}

	// Always append the build configuration for non-editor builds so it's clear
	// whether this is a Shipping, Development, Debug, etc. binary.
	return FString::Printf(TEXT("%s_%s"), *BaseVersion, *BuildConfig);
#endif
}

TPair<FString, FString> UConvaiUtils::GetAuthHeaderAndKey()
{
	FString API_Key = GetAPI_Key();
	FString AuthToken = GetAuthToken();

	FString KeyOrToken;
	FString HeaderString;

	if (!API_Key.IsEmpty())
	{
		KeyOrToken = API_Key;
		HeaderString = ConvaiConstants::API_Key_Header;
	}
	else if (!AuthToken.IsEmpty())
	{
		KeyOrToken = AuthToken;
		HeaderString = ConvaiConstants::Auth_Token_Header;
	}
	else
	{
		// Handle the case where both are empty if necessary
		KeyOrToken = "";
		HeaderString = "";
	}

	return TPair<FString, FString>(HeaderString, KeyOrToken);
}

FString UConvaiUtils::GetTestCharacterID()
{
	FString CharacterID = Convai::Get().GetConvaiSettings()->TestCharacterID;
	CharacterID.TrimEndInline();
	CharacterID.TrimStartInline();

	FString CommandLineCharacterID = UCommandLineUtils::GetCommandLineFlagValueAsString(TEXT("ConvaiTestCharacterID"), TEXT(""));
	if (!CommandLineCharacterID.IsEmpty())
	{
		CharacterID = CommandLineCharacterID;
	}

	return CharacterID;
}

FString UConvaiUtils::GetStreamURL()
{
	FString URL = Convai::Get().GetConvaiSettings()->CustomURL;
	URL.TrimEndInline();
	URL.TrimStartInline();

	FString CommandLineURL = UCommandLineUtils::GetCommandLineFlagValueAsString(TEXT("ConvaiStreamURL"), TEXT(""));
	if (!CommandLineURL.IsEmpty())
	{
		URL = CommandLineURL;
	}
	
	else if (URL.IsEmpty())
	{
		URL = TEXT("https://realtime-api.convai.com/connect");
	}
	
	return URL;
}

FString UConvaiUtils::GetLLMProvider()
{
	return ResolveCustomParam(TEXT("LLMProvider"));
}

FString UConvaiUtils::GetConnectionType()
{
	return ResolveCustomParam(TEXT("ConnectionType"));
}

bool UConvaiUtils::IsAECEnabled()
{
	return ResolveCustomParam(TEXT("AEC")) == TEXT("1");
}

bool UConvaiUtils::IsVADEnabled()
{
	return ResolveCustomParam(TEXT("VAD")) == TEXT("1");
}

FString UConvaiUtils::GetReferenceCaptureTap()
{
	return ResolveCustomParam(TEXT("ReferenceCaptureTap"));
}

FString UConvaiUtils::GetMicCaptureTap()
{
	return ResolveCustomParam(TEXT("MicCaptureTap"));
}

FString UConvaiUtils::GetAECStreamDelayMs()
{
	return ResolveCustomParam(TEXT("AECStreamDelayMs"));
}

FString UConvaiUtils::GetAECType()
{
	return ResolveCustomParam(TEXT("AECType"));
}

bool UConvaiUtils::IsNoiseSuppressionEnabled()
{
	return ResolveCustomParam(TEXT("NoiseSuppression")) == TEXT("1");
}

bool UConvaiUtils::IsGainControlEnabled()
{
	return ResolveCustomParam(TEXT("GainControl")) == TEXT("1");
}

int32 UConvaiUtils::GetVADMode()
{
	return FMath::Clamp(FCString::Atoi(*ResolveCustomParam(TEXT("VADMode"))), 0, 3);
}

bool UConvaiUtils::IsHighPassFilterEnabled()
{
	return ResolveCustomParam(TEXT("HighPassFilter")) == TEXT("1");
}

FConvaiVADSettings UConvaiUtils::GetVADSettings()
{
	if (const UConvaiSettings* Settings = Convai::Get().GetConvaiSettings())
	{
		return Settings->VADSettings;
	}
	return FConvaiVADSettings();
}

void UConvaiUtils::SetVADSettings(const FConvaiVADSettings& InSettings)
{
	if (UConvaiSettings* Settings = Convai::Get().GetConvaiSettings())
	{
		Settings->VADSettings = InSettings;
	}
}

int32 UConvaiUtils::GetChunkSize()
{
	return FCString::Atoi(*ResolveCustomParam(TEXT("ChunkSize")));
}

int32 UConvaiUtils::GetOutputFPS()
{
	return FCString::Atoi(*ResolveCustomParam(TEXT("OutputFPS")));
}

float UConvaiUtils::GetFramesBufferDuration()
{
	return FCString::Atof(*ResolveCustomParam(TEXT("FramesBufferDuration")));
}

bool UConvaiUtils::IsFaceSyncSimulateFreeze()
{
	return ResolveCustomParam(TEXT("ConvaiFaceSyncSimulateFreeze")) == TEXT("1");
}

FString UConvaiUtils::GetLipSyncAnimParamOverride(const FString& Key)
{
	return ResolveCustomParam(*Key);
}

EC_LipSyncMode UConvaiUtils::GetLipSyncMode()
{
	EC_LipSyncMode LipSyncMode = Convai::Get().GetConvaiSettings()->LipSyncMode;
	
	const UEnum* EnumPtr = StaticEnum<EC_LipSyncMode>();
	const FString ModeName = EnumPtr->GetNameStringByValue(static_cast<int64>(LipSyncMode));
	
	// Override with command line flag if present
	const FString LipSyncModeStr = UCommandLineUtils::GetCommandLineFlagValueAsString(TEXT("LipSyncMode"), ModeName);

	if (LipSyncModeStr.Equals(TEXT("Off"), ESearchCase::IgnoreCase))
	{
		return EC_LipSyncMode::Off;
	}
	if (LipSyncModeStr.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))
	{
		return EC_LipSyncMode::Auto;
	}
	if (LipSyncModeStr.Equals(TEXT("VisemeBased"), ESearchCase::IgnoreCase))
	{
		return EC_LipSyncMode::VisemeBased;
	}
	if (LipSyncModeStr.Equals(TEXT("BS_MHA"), ESearchCase::IgnoreCase) ||
	         LipSyncModeStr.Equals(TEXT("MetaHuman"), ESearchCase::IgnoreCase))
	{
		return EC_LipSyncMode::BS_MHA;
	}
	if (LipSyncModeStr.Equals(TEXT("BS_ARKit"), ESearchCase::IgnoreCase) ||
	         LipSyncModeStr.Equals(TEXT("ARKit"), ESearchCase::IgnoreCase))
	{
		return EC_LipSyncMode::BS_ARKit;
	}
	
	return LipSyncMode;
}

double UConvaiUtils::GetLipSyncTimeOffset()
{
	return FCString::Atod(*ResolveCustomParam(TEXT("LipSyncTimeOffset")));
}

bool UConvaiUtils::IsAlwaysAllowVisionEnabled()
{
	return Convai::Get().GetConvaiSettings()->AlwaysAllowVision;
}

FString UConvaiUtils::GetEmotionsProvider()
{
	return ResolveCustomParam(TEXT("EmotionsProvider"));
}

float UConvaiUtils::GetConnectionProxyTTL()
{
	return FCString::Atof(*ResolveCustomParam(TEXT("ConnectionProxyTTL")));
}

float UConvaiUtils::GetPrepConnectionTTL()
{
	return FCString::Atof(*ResolveCustomParam(TEXT("PrepConnectionTTL")));
}

float UConvaiUtils::GetLipSyncStarvationFallback()
{
	return FCString::Atof(*ResolveCustomParam(TEXT("LipSyncStarvationFallback")));
}

float UConvaiUtils::GetObjectPollIntervalSeconds()
{
	return FCString::Atof(*ResolveCustomParam(TEXT("ObjectPollIntervalSeconds")));
}

float UConvaiUtils::GetClientReadyRetrySecs()
{
	return FCString::Atof(*ResolveCustomParam(TEXT("ClientReadyRetrySecs")));
}

float UConvaiUtils::GetClientReadyTimeoutSecs()
{
	return FCString::Atof(*ResolveCustomParam(TEXT("ClientReadyTimeoutSecs")));
}

float UConvaiUtils::GetContextDebounceWindowDefault()
{
	return FCString::Atof(*ResolveCustomParam(TEXT("ContextDebounceWindow")));
}

float UConvaiUtils::GetContextMaxDebounceWindowDefault()
{
	return FCString::Atof(*ResolveCustomParam(TEXT("ContextMaxDebounceWindow")));
}

void UConvaiUtils::GetPluginInfo(const FString& PluginName, bool& Found, FString& VersionName, FString& EngineVersion, FString& FriendlyName)
{
	IPluginManager& PluginManager = IPluginManager::Get();
	TSharedPtr<IPlugin> Plugin = PluginManager.FindPlugin(PluginName);
	Found = false;

	if (Plugin.IsValid())
	{
		const FPluginDescriptor& PluginDescriptor = Plugin->GetDescriptor();
		VersionName = PluginDescriptor.VersionName;
		EngineVersion = PluginDescriptor.EngineVersion;
		FriendlyName = PluginDescriptor.FriendlyName;
		Found = true;
	}
}

void UConvaiUtils::GetPlatformInfo(FString& EngineVersion, FString& PlatformName)
{
	// Get the global engine version
	EngineVersion = FEngineVersion::Current().ToString();

	// Get the platform name
#if PLATFORM_WINDOWS
	PlatformName = TEXT("Windows");
#elif PLATFORM_MAC
	PlatformName = TEXT("Mac");
#elif PLATFORM_LINUX
	PlatformName = TEXT("Linux");
#elif PLATFORM_ANDROID
	PlatformName = TEXT("Android");
#else
	PlatformName = TEXT("Unknown");
#endif
}

FString UConvaiUtils::GetDeviceUniqueIdentifier()
{
	// Try GetDeviceId first - provides unique device identifier on most platforms
	FString DeviceId = FPlatformMisc::GetDeviceId();
	
	// Fall back to GetOperatingSystemId - unique ID for the OS installation
	if (DeviceId.IsEmpty())
	{
		DeviceId = FPlatformMisc::GetOperatingSystemId();
	}
	
	// Fall back to GetLoginId - unique per user account on the machine
	if (DeviceId.IsEmpty())
	{
		DeviceId = FPlatformMisc::GetLoginId();
	}
	
	return DeviceId;
}

namespace
{
	// This struct contains information about the sound buffer.
	struct SongBufferInfo
	{
		int32 RawPCMDataSize;
		int32 NumChannels;
		float Duration;
		int32 SampleRate;

		SongBufferInfo() : RawPCMDataSize(0), NumChannels(0), Duration(0), SampleRate(0) {}

		SongBufferInfo(int32 PCMDataSize, int32 numChannels, float duration, int32 sampleRate)
			: RawPCMDataSize(PCMDataSize), NumChannels(numChannels), Duration(duration), SampleRate(sampleRate)
		{
		}
	};

	// this struct contains the sound buffer + information about it.
	struct SongBufferData
	{
		TArray<uint8> RawPCMData;
		SongBufferInfo BufferInfo;

		// default to nothing.
		SongBufferData() : SongBufferData(0, 0, 0, 0) {}

		// allocate memory as we populate the structure.
		SongBufferData(int32 PCMDataSize, int32 numChannels, float duration, int32 sampleRate)
			: BufferInfo(PCMDataSize, numChannels, duration, sampleRate)
		{
			RawPCMData.SetNumZeroed(PCMDataSize);
		}
	};

	USoundWave* WavDataToSoundwave(TArray<uint8> Data)
	{
		FWaveModInfo WaveInfo;
		FString ErrorReason;
		if (WaveInfo.ReadWaveInfo(Data.GetData(), Data.Num(), &ErrorReason))
		{
			USoundWave* SoundWave = NewObject<USoundWave>();

			//From FSoundWavePCMWriter::ApplyBufferToSoundWave() UE4.24
			SoundWave->SetSampleRate(*WaveInfo.pSamplesPerSec);
			SoundWave->NumChannels = *WaveInfo.pChannels;

			const int32 BytesDataPerSecond = *WaveInfo.pChannels * (*WaveInfo.pBitsPerSample / 8.f) * *WaveInfo.pSamplesPerSec;
			if (BytesDataPerSecond)
			{
				SoundWave->Duration = float(WaveInfo.SampleDataSize) / float(BytesDataPerSecond);
			}

			SoundWave->RawPCMDataSize = WaveInfo.SampleDataSize;

			SoundWave->RawPCMData = static_cast<uint8*>(FMemory::Malloc(WaveInfo.SampleDataSize));
			FMemory::Memcpy(SoundWave->RawPCMData, WaveInfo.SampleDataStart, WaveInfo.SampleDataSize);

			const int32 BytesPerSample = FMath::Max(1, *WaveInfo.pBitsPerSample / 8);
			SoundWave->TotalSamples = WaveInfo.SampleDataSize / BytesPerSample;

#if WITH_EDITORONLY_DATA
			// RawPCMData alone is enough in a cooked build, but in the editor the audio engine
			// builds the platform format (BINKA) through the DDC, and that build reads the RawData
			// bulk payload and parses it as a RIFF/WAVE. With RawData left empty you get
			// "Sound wave payload in asset ... failed to parse as a wave!", then "there is no
			// source LPCM data", and the wave is flagged with errors so it never plays - even
			// though RawPCMData is perfectly good. Data is already a complete .wav, so hand it
			// over unchanged. Matches FSoundWavePCMWriter::SerializeSoundWaveToAsset.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			SoundWave->RawData.UpdatePayload(FSharedBuffer::Clone(Data.GetData(), Data.Num()));
#else
			SoundWave->RawData.Lock(LOCK_READ_WRITE);
			void* LockedRawData = SoundWave->RawData.Realloc(Data.Num());
			FMemory::Memcpy(LockedRawData, Data.GetData(), Data.Num());
			SoundWave->RawData.Unlock();
#endif
			SoundWave->SetImportedSampleRate(*WaveInfo.pSamplesPerSec);
#endif

			return SoundWave;
		}
		else
		{
			//CONVAI_LOG(ConvaiT2SHttpLog, Warning, TEXT("%s"), *ErrorReason);
			return nullptr;
		}
	}

	bool DecompressUSoundWave(USoundWave* soundWave, TSharedPtr<SongBufferData>& Out_SongBufferData)
	{
		FAudioDevice* audioDevice = GEngine ? GEngine->GetMainAudioDeviceRaw() : nullptr;

		if (!audioDevice || !soundWave || soundWave->GetName() == TEXT("None"))
			return false;

		bool breturn = false;

#if ENGINE_MAJOR_VERSION == 4 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 2)

		// Ensure we have the sound data. Compressed format is fine.
		soundWave->InitAudioResource(audioDevice->GetRuntimeFormat(soundWave));

		// Create a decoder for this audio. We want the PCM data.
		ICompressedAudioInfo* AudioInfo = audioDevice->CreateCompressedAudioInfo(soundWave);

#else
		// Ensure we have the sound data. Compressed format is fine.
		soundWave->InitAudioResource(soundWave->GetRuntimeFormat());
		
		// Create a decoder for this audio. We want the PCM data.
		ICompressedAudioInfo* AudioInfo = IAudioInfoFactoryRegistry::Get().Create(soundWave->GetRuntimeFormat());
#endif

		// Decompress complete audio to this buffer
		FSoundQualityInfo QualityInfo = { 0 };
#if ENGINE_MAJOR_VERSION == 4
		if (AudioInfo->ReadCompressedInfo(soundWave->ResourceData, soundWave->ResourceSize, &QualityInfo))
		{
			Out_SongBufferData = TSharedPtr<SongBufferData>(new SongBufferData(
				QualityInfo.SampleDataSize, QualityInfo.NumChannels, QualityInfo.Duration, QualityInfo.SampleRate));

			// Decompress all the sample data into preallocated memory now
			AudioInfo->ExpandFile(Out_SongBufferData->RawPCMData.GetData(), &QualityInfo);

			breturn = true;
		}
#else
		FAudioDevice* AudioDevice = GEngine->GetMainAudioDeviceRaw();
		if (AudioDevice)
		{
			#if (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 2)
			FName format = AudioDevice->GetRuntimeFormat(soundWave);
			#else
			FName format = soundWave->GetRuntimeFormat();
			#endif
			soundWave->InitAudioResource(format);
		}

		const uint8* ResourceData = soundWave->GetResourceData();
		uint32 ResourceSize = soundWave->GetResourceSize();

		if (!ResourceData || ResourceSize <= 0)
		{
			return breturn;
		}

		if (AudioInfo->ReadCompressedInfo(ResourceData, ResourceSize, &QualityInfo))
		{
			Out_SongBufferData = TSharedPtr<SongBufferData>(new SongBufferData(
				QualityInfo.SampleDataSize, QualityInfo.NumChannels, QualityInfo.Duration, QualityInfo.SampleRate));

			// Decompress all the sample data into preallocated memory now
			AudioInfo->ExpandFile(Out_SongBufferData->RawPCMData.GetData(), &QualityInfo);

			breturn = true;
		}
#endif
		// Clean up.
		delete AudioInfo;

		return breturn;
	}
};

TArray<uint8> UConvaiUtils::ExtractPCMDataFromSoundWave(USoundWave* SoundWave, int32& OutSampleRate, int32& OutNumChannels)
{
	TArray<uint8> PCMData;

	if (!SoundWave)
	{
		CONVAI_LOG(LogTemp, Warning, TEXT("SoundWave is null!"));
		return PCMData;
	}

	if (SoundWave->RawPCMDataSize > 0)
	{
		PCMData.Append(SoundWave->RawPCMData, SoundWave->RawPCMDataSize);
		OutSampleRate = SoundWave->GetSampleRateForCurrentPlatform();
		OutNumChannels = SoundWave->NumChannels;
	}
	else
	{
		TSharedPtr<SongBufferData> SongBuffer;
		if (DecompressUSoundWave(SoundWave, SongBuffer) && SongBuffer.IsValid())
		{
			PCMData = SongBuffer->RawPCMData;
			OutSampleRate = SongBuffer->BufferInfo.SampleRate;
			OutNumChannels = SongBuffer->BufferInfo.NumChannels;
		}
	}
	return PCMData;
}

void UConvaiUtils::PCMDataToWav(TArray<uint8> InPCMBytes, TArray<uint8>& OutWaveFileData, int NumChannels, int SampleRate)
{
	// SerializeWaveFile sizes its output at 44 + NumBytes and then indexes [44]
	// to memcpy the payload, so an empty buffer indexes one past the end of a
	// 44-element array and asserts. Guarded here rather than at the call site
	// because FinishRecording reaches this with an empty VoiceCaptureBuffer
	// whenever the microphone produced nothing, including from EndPlay, where
	// nobody chose to call it.
	if (InPCMBytes.Num() == 0)
	{
		OutWaveFileData.Reset();
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("PCMDataToWav: no PCM data, no wav written"));
		return;
	}

	SerializeWaveFile(OutWaveFileData, InPCMBytes.GetData(), InPCMBytes.Num(), NumChannels, SampleRate);
}

USoundWave* UConvaiUtils::PCMDataToSoundWav(TArray<uint8> InPCMBytes, int NumChannels, int SampleRate)
{
	if (InPCMBytes.Num() <= 44)
		return nullptr;

	TArray<uint8> OutSerializeWave;
	// insert the .wav format headers at the beggining of the PCM data
	SerializeWaveFile(OutSerializeWave, InPCMBytes.GetData(), InPCMBytes.Num(), NumChannels, SampleRate);

	// Save the wav file to disk for debug
	//FString SaveDir = "C:\\Users\\pc\\Videos\\MetahumansConvaiTutorial\\outtest.wav";
	//CONVAI_LOG(ConvaiUtilsLog, Log, TEXT("OutSerializeWave.Num() final: %d bytes "), OutSerializeWave.Num());
	//FFileHelper::SaveArrayToFile(OutSerializeWave, *SaveDir);

	return UConvaiUtils::WavDataToSoundWave(OutSerializeWave);
}

USoundWave* UConvaiUtils::WavDataToSoundWave(TArray<uint8> InWavData)
{
	return WavDataToSoundwave(InWavData);
}

void UConvaiUtils::ResampleAudio(float currentSampleRate, float targetSampleRate, int numChannels, bool reduceToMono, int16* currentPcmData, int numSamplesToConvert, TArray<int16>& outResampledPcmData)
{
	// Calculate the ratio of input to output sample rates
	float sampleRateRatio = currentSampleRate / targetSampleRate;

	// Determine the number of output channels
	int outNumChannels = reduceToMono ? 1 : numChannels;

	// Determine the number of frames to iterate over
	int32 numFramesToConvert = FMath::CeilToInt((float)numSamplesToConvert / (float)numChannels);

	// Calculate the number output frames
	int32 numOutputFrames = FMath::CeilToInt(numFramesToConvert * targetSampleRate / currentSampleRate);

	// Resize the output array to the expected size
	outResampledPcmData.Reset(numOutputFrames * outNumChannels);

	// Initialize variables for tracking the current and next frame indices
	float currentFrameIndex = 0.0f;
	float nextFrameIndex = 0.0f;

	// Iterate over the frames and resample the audio
	for (;;)
	{
		// Calculate the next frame index
		nextFrameIndex += sampleRateRatio;

		if (currentFrameIndex >= numFramesToConvert || nextFrameIndex > numFramesToConvert)
		{
			break;
		}

		// Calculate the number of input samples to average over
		int32 numInputSamplesToAverage = FMath::CeilToInt(nextFrameIndex - currentFrameIndex);

		if (reduceToMono)
		{
			// Average every channel, not channel 0. Taking the first channel
			// alone under-represents anything panned away from it, and when
			// this feeds an echo canceller the far end is exactly the signal
			// that has to be matched.
			int32 sumOfInputSamples = 0;
			for (int inputSampleIndex = 0; inputSampleIndex < numInputSamplesToAverage; ++inputSampleIndex)
			{
				const int32 frameIndex = FMath::FloorToInt(currentFrameIndex + inputSampleIndex);
				for (int channel = 0; channel < numChannels; ++channel)
				{
					sumOfInputSamples += *(currentPcmData + frameIndex * numChannels + channel);
				}
			}
			outResampledPcmData.Add(
				(int16)(sumOfInputSamples / (numInputSamplesToAverage * FMath::Max(1, numChannels))));
		}
		else
		{
			for (int channel = 0; channel < outNumChannels; ++channel)
			{
				// Per channel: shared across channels, the sum carried channel
				// n-1 into channel n.
				int32 sumOfInputSamples = 0;
				for (int inputSampleIndex = 0; inputSampleIndex < numInputSamplesToAverage; ++inputSampleIndex)
				{
					int32 currentSampleIndex = FMath::FloorToInt(currentFrameIndex + inputSampleIndex) * numChannels + channel;
					int16 currentSampleValue = *(currentPcmData + currentSampleIndex);
					sumOfInputSamples += currentSampleValue;
				}

				// Calculate the average of the input samples
				int16 averageSampleValue = (int16)(sumOfInputSamples / numInputSamplesToAverage);

				// Add the resampled sample to the output array
				outResampledPcmData.Add(averageSampleValue);
			}
		}

		// Update the current frame index
		currentFrameIndex = nextFrameIndex;
	}
}

void UConvaiUtils::ResampleAudio(float currentSampleRate, float targetSampleRate, int numChannels, bool reduceToMono, const TArray<int16>& currentPcmData, int numSamplesToConvert, TArray<int16>& outResampledPcmData)
{
	// Call the other function using this instance
	ResampleAudio(currentSampleRate, targetSampleRate, numChannels, reduceToMono, (int16*)currentPcmData.GetData(), numSamplesToConvert, outResampledPcmData);
}

FString UConvaiUtils::FUTF8ToFString(const char* StringToConvert)
{
	// Create a TCHAR (wide string) from the UTF-8 string using Unreal's FUTF8ToTCHAR class
	FUTF8ToTCHAR Converter(StringToConvert);

	// Create an FString from the converted wide string
	FString text_string(Converter.Get());

	return text_string;
}

int UConvaiUtils::LevenshteinDistance(const FString& s, const FString& t)
{
	// Degenerate cases
	if (s == t) return 0;
	if (s.Len() == 0) return t.Len();
	if (t.Len() == 0) return s.Len();

	// Create two work vectors of integer distances
	TArray<int32> v0;
	v0.Init(0, t.Len() + 1);
	TArray<int32> v1;
	v1.Init(0, t.Len() + 1);

	// Initialize v0 (the previous row of distances)
	// This row is A[0][i]: edit distance for an empty s
	// The distance is just the number of characters to delete from t
	for (int32 i = 0; i < v0.Num(); i++)
	{
		v0[i] = i;
	}

	for (int32 i = 0; i < s.Len(); i++)
	{
		// Calculate v1 (current row distances) from the previous row v0

		// First element of v1 is A[i+1][0]
		// Edit distance is delete (i+1) characters from s to match an empty t
		v1[0] = i + 1;

		// Use formula to fill in the rest of the row
		for (int32 j = 0; j < t.Len(); j++)
		{
			int32 cost = (s[i] == t[j]) ? 0 : 2; // Here, we change the cost of substitution to 2
			v1[j + 1] = FMath::Min3(v1[j] + 1, v0[j + 1] + 1, v0[j] + cost);
		}

		// Copy v1 (current row) to v0 (previous row) for next iteration
		for (int32 j = 0; j < v0.Num(); j++)
		{
			v0[j] = v1[j];
		}
	}

	return v1[t.Len()];
}

TArray<FAnimationFrame> UConvaiUtils::ParseJsonToBlendShapeData(const FString& JsonString)
{
	TArray<FAnimationFrame> AnimationFrames;

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	TSharedPtr<FJsonValue> JsonParsed;
	if (FJsonSerializer::Deserialize(Reader, JsonParsed) && JsonParsed.IsValid() && JsonParsed->Type == EJson::Array)
	{
		TArray<TSharedPtr<FJsonValue>> FrameArray = JsonParsed->AsArray();
		for (auto FrameVal : FrameArray)
		{
			TSharedPtr<FJsonObject> FrameObj = FrameVal->AsObject();
		FAnimationFrame NewFrame;

		NewFrame.FrameIndex = FrameObj->GetIntegerField(JSON_FIELD("FrameIndex"));

		TArray<TSharedPtr<FJsonValue>> BlendShapeArray = FrameObj->GetArrayField(JSON_FIELD("BlendShapes"));
		for (auto BlendShapeVal : BlendShapeArray)
		{
			TSharedPtr<FJsonObject> BlendShapeObj = BlendShapeVal->AsObject();
			FName name = FName(BlendShapeObj->GetStringField(JSON_FIELD("name")));
			double score;
			bool Success = BlendShapeObj->TryGetNumberField(JSON_FIELD("score"), score);
				if (!Success)
					score = 0;

				NewFrame.BlendShapes.Add(name, score);
			}

			AnimationFrames.Add(NewFrame);
		}
	}

	return AnimationFrames;
}

bool UConvaiUtils::ParseVisemeValuesToAnimationFrame(const FString& VisemeValuesString, FAnimationFrame& AnimationFrame)
{
	// Split the input string by ','
	TArray<FString> StringValues;
	VisemeValuesString.ParseIntoArray(StringValues, TEXT(","));

	// Ensure that the number of parsed values is the same as the number of viseme names
	if (StringValues.Num() != ConvaiConstants::VisemeNames.Num())
	{
		// Log an error message and return the uninitialized FAnimationFrame object
		//CONVAI_LOG(LogTemp, Error, TEXT("Number of values does not match the number of viseme names."));
		return false;
	}

	float ValuesSum = 0.0f; // Used to check if all blendshapes are zeros

	bool ignore = true;
	// Loop over the parsed string values and the viseme names simultaneously
	for (int32 Index = 0; Index < StringValues.Num(); ++Index)
	{
		// Convert each string value to a float and add it to the TMap in AnimationFrame
		float Value;
		if (StringValues[Index].TrimStartAndEnd().IsNumeric())
		{
			Value = FCString::Atof(*StringValues[Index]);
			AnimationFrame.BlendShapes.Add(*ConvaiConstants::VisemeNames[Index], Value);
			if (Value > 0.03)
				ignore = false;
			ValuesSum += Value; // Add the value to the sum
		}
		else
		{
			// Log a warning message if a string value is not numeric
			//CONVAI_LOG(LogTemp, Warning, TEXT("Invalid numeric value: %s"), *StringValues[Index]);
			Value = 0;
			AnimationFrame.BlendShapes.Add(*ConvaiConstants::VisemeNames[Index], Value);
		}
	}

	// If the sum of all parsed values is close to zero, set the first blendshape to 1
	if (ValuesSum < 0.1 || ignore)
	{
		if (ConvaiConstants::VisemeNames.Num() > 0)
		{
			AnimationFrame.BlendShapes[*ConvaiConstants::VisemeNames[0]] = 1.0f;
			return false;
		}
	}

	return true;
}

AActor* UConvaiUtils::ConvaiCloneActor(AActor* InputActor)
{
	UWorld* World = InputActor->GetWorld();
	FActorSpawnParameters params;
	params.Template = InputActor;

	UClass* ItemClass = InputActor->GetClass();
	AActor* const SpawnedActor = World->SpawnActor<AActor>(ItemClass, params);
	return SpawnedActor;
}

FString UConvaiUtils::ConvaiAnimationSequenceToJson(const FAnimationSequenceBP& AnimationSequenceBP)
{
	return AnimationSequenceBP.AnimationSequence.ToJson();
}

void UConvaiUtils::ConvaiAnimationSequenceFromJson(const FString& JsonString, FAnimationSequenceBP& AnimationSequenceBP)
{
	AnimationSequenceBP.AnimationSequence.FromJson(JsonString);
}

bool UConvaiUtils::ContainsAudioContent(const int16_t* AudioData, const size_t NumFrames, const uint32_t NumChannels, const int16_t AudioContentThreshold)
{
	if (!AudioData || NumFrames == 0)
		return false;

	// Check if any sample exceeds our threshold for audio content
	const size_t TotalSamples = NumFrames * NumChannels;
	for (size_t i = 0; i < TotalSamples; ++i)
	{
		// Check absolute value of sample against threshold
		if (FMath::Abs(AudioData[i]) > AudioContentThreshold)
		{
			return true; // Found audio content
		}
	}

	return false; // No significant audio content detected
}

bool UConvaiUtils::ContainsActualAudio(
	const int16_t* AudioData,
	const size_t NumFrames,
	const uint32_t NumChannels,
	const FConvaiAudioFrameStats& PrevStats,
	FConvaiAudioFrameStats& OutCurrentStats,
	const int16_t AmplitudeThreshold,
	const float VarianceThreshold)
{
	OutCurrentStats = FConvaiAudioFrameStats{};

	if (!AudioData || NumFrames == 0 || NumChannels == 0)
	{
		return false;
	}

	const size_t TotalSamples = NumFrames * NumChannels;

	// Single-pass online stats: peak, running mean, running M2 (sum of squared
	// deviations from the running mean). Welford's algorithm — numerically
	// stable without needing a prior mean pass. M2 / N is the population
	// variance at the end.
	int32 Peak = 0;
	double RunningMean = 0.0;
	double M2 = 0.0;
	for (size_t i = 0; i < TotalSamples; ++i)
	{
		const int32 S = AudioData[i];
		const int32 AbsS = FMath::Abs(S);
		if (AbsS > Peak)
		{
			Peak = AbsS;
		}

		const double X = static_cast<double>(S);
		const double Delta = X - RunningMean;
		RunningMean += Delta / static_cast<double>(i + 1);
		M2 += Delta * (X - RunningMean);
	}

	const float Variance = TotalSamples > 0
		? static_cast<float>(M2 / static_cast<double>(TotalSamples))
		: 0.0f;
	const float RMS = FMath::Sqrt(Variance + static_cast<float>(RunningMean * RunningMean));

	OutCurrentStats.PeakAmplitude = Peak;
	OutCurrentStats.RMS           = RMS;
	OutCurrentStats.Variance      = Variance;
	OutCurrentStats.SampleCount   = static_cast<int32>(TotalSamples);

	// Primary gate: must clear BOTH peak and variance. Variance catches steady
	// background hum / DC offset that would otherwise sneak past a peak-only
	// check, while peak rules out the symmetric case (a single loud click with
	// near-zero variance around it isn't speech either).
	const bool bClearsThresholds =
		(Peak > AmplitudeThreshold) &&
		(Variance > VarianceThreshold);

	// Cross-frame smoothing: if the prior frame had real audio but this one
	// collapsed to <10 % of its RMS AND fell below the variance bar, treat it
	// as the tail end of an utterance regardless of any spike that briefly
	// cleared Peak. Prevents single-sample clicks during silence from re-
	// extending the grace window after speech has actually ended.
	bool bIsActualAudio = bClearsThresholds;
	if (bClearsThresholds && PrevStats.SampleCount > 0 && PrevStats.bHasContent)
	{
		const bool bRMSCollapsed = (RMS < 0.1f * PrevStats.RMS);
		const bool bVarianceBelow = (Variance <= VarianceThreshold * 2.0f);
		if (bRMSCollapsed && bVarianceBelow)
		{
			bIsActualAudio = false;
		}
	}

	OutCurrentStats.bHasContent = bIsActualAudio;
	return bIsActualAudio;
}

TMap<FName, float> UConvaiUtils::MapBlendshapes(const TMap<FName, float>& InputBlendshapes, const TMap<FName, FConvaiBlendshapeParameters>& BlendshapeMap, float GlobalMultiplier, float GlobalOffset)
{
	TMap<FName, float> OutputMap;

	// Generate arrays for original blendshape names and values
	TArray<FName> OriginalNames;
	TArray<float> OriginalValues;
	InputBlendshapes.GenerateKeyArray(OriginalNames);
	InputBlendshapes.GenerateValueArray(OriginalValues);

	// Loop through each original blendshape
	for (int i = 0; i < OriginalNames.Num(); i++)
	{
		FName OriginalName = OriginalNames[i];
		float OriginalValue = OriginalValues[i];

		// Check if the original name has a mapped parameter
		const FConvaiBlendshapeParameters* MappedParameter = BlendshapeMap.Find(OriginalName);

		if (MappedParameter)
		{
			float Multiplier = MappedParameter->Multiplyer;
			float Offset = MappedParameter->Offset;
			bool UseOverrideValue = MappedParameter->UseOverrideValue;
			float OverrideValue = MappedParameter->OverrideValue;
			float ClampMinValue = MappedParameter->ClampMinValue;
			float ClampMaxValue = MappedParameter->ClampMaxValue;
			bool IgnoreGlobalModifiers = MappedParameter->IgnoreGlobalModifiers;

			// Loop through each target name specified in the mapped parameter
			for (FName TargetName : MappedParameter->TargetNames)
			{
				if (UseOverrideValue)
				{
					// Use the override value if specified
					OutputMap.Add(TargetName, OverrideValue);
				}
				else
				{
					// Calculate the final blendshape value using the multiplier and offset
					float CalculatedValue;
					if (IgnoreGlobalModifiers)
					{
						CalculatedValue = Multiplier * OriginalValue + Offset;
					}
					else
					{
						CalculatedValue = Multiplier * OriginalValue * GlobalMultiplier + Offset + GlobalOffset;
					}

					CalculatedValue = CalculatedValue > ClampMaxValue ? ClampMaxValue : CalculatedValue;
					CalculatedValue = CalculatedValue < ClampMinValue ? ClampMinValue : CalculatedValue;

					// If this curve appeared before then choose the higher value
					if (float* PreviousValue = OutputMap.Find(TargetName))
					{
						if (CalculatedValue <= *PreviousValue)
						{
							continue;
						}
					}

					OutputMap.Add(TargetName, CalculatedValue);
				}
			}
		}
		else
		{
			// If no mapped parameter exists for the original name, keep the original value
			OutputMap.Add(OriginalName, OriginalValue);
		}
	}

	return OutputMap;
}

void UConvaiUtils::SplitBlendshapeMapByKeys(
	TMap<FName, float>& InOutOriginalMap,
	const TArray<FName>& SplitKeys,
	TMap<FName, float>& OutSplitMap
)
{
	// Pre-reserve memory for the split map
	OutSplitMap.Empty(SplitKeys.Num());

	for (const FName& Key : SplitKeys)
	{
		float Value;
		if (InOutOriginalMap.RemoveAndCopyValue(Key, Value))
		{
			OutSplitMap.Add(Key, Value);
		}
	}
}

TMap<FName, float> UConvaiUtils::MergeBlendshapeMaps(
	const TMap<FName, float>& BaseMap,
	const TMap<FName, float>& OverrideMap
)
{
	// Start with a copy of the base map
	TMap<FName, float> Result = BaseMap;

	// Reserve additional space for potential new keys from override map
	Result.Reserve(BaseMap.Num() + OverrideMap.Num());

	// Add/override with values from the override map
	for (const auto& Pair : OverrideMap)
	{
		Result.Add(Pair.Key, Pair.Value);
	}

	return Result;
}

bool UConvaiSettingsUtils::GetParamValueAsString(const FString& paramName, FString& outValue) {
    // First check command line parameters
    FString CommandLineValue = UCommandLineUtils::GetCommandLineFlagValueAsString(paramName, TEXT(""));
    if (!CommandLineValue.IsEmpty())
    {
        outValue = CommandLineValue;
        return true;
    }

    // Fall back to ExtraParams setting
    FString input = Convai::Get().GetConvaiSettings()->ExtraParams;
    FString result;
    FString trimmedInput = input.Replace(TEXT(" "), TEXT("")); // Remove all spaces
    if (trimmedInput.Split(paramName + TEXT("="), nullptr, &result)) {
        result.Split(TEXT(","), &result, nullptr);
        outValue = result.TrimStartAndEnd().Replace(TEXT("\""), TEXT("")).TrimStartAndEnd();
        return true;
    }
    outValue = FString();
    return false;
}

bool UConvaiSettingsUtils::GetParamValueAsFloat(const FString& paramName, float& outValue) {
    // First check command line parameters
    FString CommandLineValue = UCommandLineUtils::GetCommandLineFlagValueAsString(paramName, TEXT(""));
    if (!CommandLineValue.IsEmpty())
    {
        if (FDefaultValueHelper::ParseFloat(CommandLineValue, outValue))
        {
            return true;
        }
    }

    // Fall back to ExtraParams setting
    FString stringValue;
    if (GetParamValueAsString(paramName, stringValue)) {
        if (FDefaultValueHelper::ParseFloat(stringValue, outValue))
        {
            return true;
        }
    }
    outValue = 0.0f;
    return false;
}

bool UConvaiSettingsUtils::GetParamValueAsInt(const FString& paramName, int32& outValue) {
    // First check command line parameters
    FString CommandLineValue = UCommandLineUtils::GetCommandLineFlagValueAsString(paramName, TEXT(""));
    if (!CommandLineValue.IsEmpty())
    {
        if (FDefaultValueHelper::ParseInt(CommandLineValue, outValue))
        {
            return true;
        }
    }

    // Fall back to ExtraParams setting
    FString stringValue;
    if (GetParamValueAsString(paramName, stringValue)) {
        if (FDefaultValueHelper::ParseInt(stringValue, outValue))
        {
            return true;
        }
    }
    outValue = 0;
    return false;
}

bool UConvaiUtils::WriteSoundWaveToWavFile(USoundWave* SoundWave, const FString& FilePath)
{
	// Access the raw PCM data from the USoundWave
	int32 OutSampleRate;
	int32 OutNumChannels;
	TArray<uint8> RawData = ExtractPCMDataFromSoundWave(SoundWave, OutSampleRate, OutNumChannels);

#if ENGINE_MAJOR_VERSION < 5
	if (RawData.Num() == 0)
		return false;
#else
	if (RawData.IsEmpty())
		return false;
#endif

	TArray<uint8> OutWaveFileData;

	PCMDataToWav(RawData, OutWaveFileData, OutNumChannels, OutSampleRate);

	// Use the helper function to save the raw PCM data as a .wav file
	return SaveByteArrayAsFile(FilePath, OutWaveFileData);
}

USoundWave* UConvaiUtils::ReadWavFileAsSoundWave(const FString & FilePath)
{
	FString ProcessedFilePath = GetAbsolutePathFromFilePath(FilePath);

	TArray<uint8> RawData;
	if (!ReadFileAsByteArray(ProcessedFilePath, RawData))
	{
		return nullptr;
	}

	// Create a new USoundWave object
	USoundWave* NewSoundWave = WavDataToSoundWave(RawData);

	return NewSoundWave;
}

bool UCommandLineUtils::IsCommandLineFlagPresent(const FString& Flag)
{
    // Check if the flag is present as a standalone flag (e.g., -fullscreen)
    if (FParse::Param(FCommandLine::Get(), *Flag))
    {
        return true;
    }

    // Check if the flag is present as a flag with a value (e.g., -port=12345)
    FString DummyString;
    if (FParse::Value(FCommandLine::Get(), *(Flag + "="), DummyString))
    {
        return true;
    }

    return false;
}

int32 UCommandLineUtils::GetCommandLineFlagValueAsInt(const FString& Flag, int32 DefaultValue)
{
	int32 Value = DefaultValue;

	// Check for the "-Flag=" and get its value as an integer
	if (FParse::Value(FCommandLine::Get(), *(Flag + "="), Value))
	{
		return Value;
	}

	return DefaultValue; // Return default if not found
}

FString UCommandLineUtils::GetCommandLineFlagValueAsString(const FString& Flag, const FString& DefaultValue)
{
	FString Value;

	// Check for the "-Flag=" and get its value as a string
	if (FParse::Value(FCommandLine::Get(), *(Flag + "="), Value))
	{
		Value.TrimStartInline();
		Value.TrimEndInline();
		if (!Value.IsEmpty())
		{
			return Value;
		}
	}

	return DefaultValue; // Return default if not found or empty
}

FString UCommandLineUtils::GetCommandLineFlagValueAsStringNoDefault(const FString& Flag)
{
	FString BaseUrl = FString();
	FParse::Value(FCommandLine::Get(), *Flag, BaseUrl);    
	return BaseUrl;
}

double UCommandLineUtils::GetCommandLineFlagValueAsDouble(const FString& Flag, const double DefaultValue)
{
	double Value;
	if (FParse::Value(FCommandLine::Get(), *(Flag + "="), Value))
	{
		return Value;
	}

	return DefaultValue;
}

void UConvaiUtils::GetBodyAndFaceSkeletalMeshComponents(AActor* Actor, USkeletalMeshComponent*& BodyComponent, USkeletalMeshComponent*& FaceComponent, bool bRequireAnimInstance)
{
	BodyComponent = nullptr;
	FaceComponent = nullptr;

	if (!Actor)
		return;

	TInlineComponentArray<USkeletalMeshComponent*> SkelMeshes(Actor);

	if (SkelMeshes.Num() == 0)
		return;

	// Only meshes with a running anim instance (an AnimBP) can be driven for body/face
	// animation — filter the rest (hair, cloth, static-pose meshes) out of the candidate
	// set. If nothing qualifies, keep the full list so callers still get a body fallback.
	if (bRequireAnimInstance)
	{
		TInlineComponentArray<USkeletalMeshComponent*> Animated;
		for (USkeletalMeshComponent* Comp : SkelMeshes)
		{
			if (Comp && Comp->GetAnimInstance())
				Animated.Add(Comp);
		}
		if (Animated.Num() > 0)
			SkelMeshes = MoveTemp(Animated);
	}

	if (SkelMeshes.Num() == 1)
	{
		BodyComponent = SkelMeshes[0];
		return;
	}

	// "head" is intentionally excluded — it appears on both body and face rigs.
	static const TCHAR* const BodyTokens[] = {
		TEXT("pelvis"), TEXT("spine"),    TEXT("neck"),     TEXT("clavicle"),
		TEXT("shoulder"), TEXT("upperarm"), TEXT("lowerarm"), TEXT("hand"),
		TEXT("thigh"),  TEXT("calf"),     TEXT("foot"),     TEXT("knee"),
		TEXT("elbow"),  TEXT("wrist"),    TEXT("ankle"),    TEXT("hip")
	};
	static const TCHAR* const FaceTokens[] = {
		TEXT("eye"),     TEXT("nose"),  TEXT("jaw"),    TEXT("lip"),
		TEXT("brow"),    TEXT("cheek"), TEXT("tongue"), TEXT("teeth"),
		TEXT("chin"),    TEXT("mouth"), TEXT("forehead"),
		TEXT("lash"),    TEXT("facial")
	};
	constexpr int32 NumBodyTokens = UE_ARRAY_COUNT(BodyTokens);
	constexpr int32 NumFaceTokens = UE_ARRAY_COUNT(FaceTokens);

	// Depth defaults to MAX_int32 so null/invalid slots lose every tiebreak.
	struct FScore { int32 BodyHits = 0; int32 FaceHits = 0; int32 Depth = MAX_int32; };
	TArray<FScore, TInlineAllocator<8>> Scores;
	Scores.AddDefaulted(SkelMeshes.Num());

	constexpr int32 EarlyExitFaceHits = 3;

	for (int32 i = 0; i < SkelMeshes.Num(); ++i)
	{
		USkeletalMeshComponent* Comp = SkelMeshes[i];
		if (!Comp)
			continue;

		// Attachment depth within this actor (0 = root component, 1 = child of root, ...).
		// Used purely as a tiebreaker among equal token scores: MetaHuman hangs Face/Hair/
		// Eyebrows/Eyelashes off the Body component, so the parent mesh should outrank
		// its children when token signal alone can't separate them.
		int32 Depth = 0;
		for (USceneComponent* Parent = Comp->GetAttachParent();
		     Parent && Parent->GetOwner() == Actor;
		     Parent = Parent->GetAttachParent())
		{
			++Depth;
		}
		Scores[i].Depth = Depth;

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 4
		USkeletalMesh* Mesh = Comp->GetSkeletalMeshAsset();
#else
		USkeletalMesh* Mesh = Comp->SkeletalMesh;
#endif
		if (!Mesh)
			continue;

		const TArray<FMeshBoneInfo>& BoneInfos = Mesh->GetRefSkeleton().GetRefBoneInfo();

		int32 BodyHits = 0;
		int32 FaceHits = 0;
		FString BoneName;
		for (const FMeshBoneInfo& Info : BoneInfos)
		{
			BoneName.Reset();
			Info.Name.AppendString(BoneName);
			for (int32 t = 0; t < NumFaceTokens; ++t)
			{
				if (BoneName.Contains(FaceTokens[t], ESearchCase::IgnoreCase))
				{
					++FaceHits;
					break;
				}
			}
			for (int32 t = 0; t < NumBodyTokens; ++t)
			{
				if (BoneName.Contains(BodyTokens[t], ESearchCase::IgnoreCase))
				{
					++BodyHits;
					break;
				}
			}
			if (FaceHits >= EarlyExitFaceHits && BodyHits == 0)
				break;
		}
		Scores[i].BodyHits = BodyHits;
		Scores[i].FaceHits = FaceHits;
	}

	// Lexicographic ranking: hits dominate, depth only resolves ties (shallower wins).
	auto Beats = [](int32 NewHits, int32 NewDepth, int32 BestHits, int32 BestDepth)
	{
		if (NewHits != BestHits) return NewHits > BestHits;
		return NewDepth < BestDepth;
	};

	int32 BestBodyIdx = INDEX_NONE; int32 BestBodyScore = -1; int32 BestBodyDepth = MAX_int32;
	int32 BestFaceIdx = INDEX_NONE; int32 BestFaceScore = -1; int32 BestFaceDepth = MAX_int32;
	for (int32 i = 0; i < SkelMeshes.Num(); ++i)
	{
		if (Beats(Scores[i].BodyHits, Scores[i].Depth, BestBodyScore, BestBodyDepth))
		{
			BestBodyScore = Scores[i].BodyHits; BestBodyDepth = Scores[i].Depth; BestBodyIdx = i;
		}
		if (Beats(Scores[i].FaceHits, Scores[i].Depth, BestFaceScore, BestFaceDepth))
		{
			BestFaceScore = Scores[i].FaceHits; BestFaceDepth = Scores[i].Depth; BestFaceIdx = i;
		}
	}

	// If a single mesh tops both categories (fully-rigged body among hair/cloth meshes),
	// keep it as body and look for a distinct face mesh among the remaining candidates.
	if (BestBodyIdx == BestFaceIdx)
	{
		int32 FallbackFaceIdx = INDEX_NONE; int32 FallbackFaceScore = 0; int32 FallbackFaceDepth = MAX_int32;
		for (int32 i = 0; i < SkelMeshes.Num(); ++i)
		{
			if (i == BestBodyIdx)
				continue;
			if (Beats(Scores[i].FaceHits, Scores[i].Depth, FallbackFaceScore, FallbackFaceDepth))
			{
				FallbackFaceScore = Scores[i].FaceHits;
				FallbackFaceDepth = Scores[i].Depth;
				FallbackFaceIdx = i;
			}
		}
		BestFaceIdx = (FallbackFaceScore > 0) ? FallbackFaceIdx : INDEX_NONE;
		BestFaceScore = FallbackFaceScore;
	}

	// Body falls through even at 0 hits — the shallowest mesh wins, which keeps a
	// MetaHuman parent body over its 0-hit children. Face stays null without signal.
	BodyComponent = (BestBodyIdx != INDEX_NONE) ? SkelMeshes[BestBodyIdx] : SkelMeshes[0];
	FaceComponent = (BestFaceScore > 0 && BestFaceIdx != INDEX_NONE) ? SkelMeshes[BestFaceIdx] : nullptr;
}

bool UConvaiUtils::FixCC5LipsyncPostProcessBlendshapes(UAnimInstance* AnimInstance)
{
	if (!AnimInstance)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("FixCC5LipsyncPostProcessBlendshapes: AnimInstance is null"));
		return false;
	}

	// Get the owning skeletal mesh component
	USkeletalMeshComponent* SkelMeshComp = AnimInstance->GetOwningComponent();
	if (!SkelMeshComp)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("FixCC5LipsyncPostProcessBlendshapes: Could not get owning skeletal mesh component"));
		return false;
	}

	CONVAI_LOG(ConvaiUtilsLog, Log, TEXT("FixCC5LipsyncPostProcessBlendshapes: Found skeletal mesh component: %s"), *SkelMeshComp->GetName());

	// Get the post process anim instance
	UAnimInstance* PostProcessAnimInstance = SkelMeshComp->GetPostProcessInstance();
	if (!PostProcessAnimInstance)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("FixCC5LipsyncPostProcessBlendshapes: No post process anim instance found"));
		return false;
	}

	CONVAI_LOG(ConvaiUtilsLog, Log, TEXT("FixCC5LipsyncPostProcessBlendshapes: Found post process anim instance: %s"), *PostProcessAnimInstance->GetClass()->GetName());

	// Get the class of the post process anim instance to find the property
	UClass* AnimClass = PostProcessAnimInstance->GetClass();
	if (!AnimClass)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("FixCC5LipsyncPostProcessBlendshapes: Could not get class of post process anim instance"));
		return false;
	}

	// Find the ExpBoneData property
	FProperty* ExpBoneDataProp = AnimClass->FindPropertyByName(FName(TEXT("ExpBoneData")));
	if (!ExpBoneDataProp)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("FixCC5LipsyncPostProcessBlendshapes: Could not find ExpBoneData property"));
		return false;
	}

	CONVAI_LOG(ConvaiUtilsLog, Log, TEXT("FixCC5LipsyncPostProcessBlendshapes: Found ExpBoneData property"));

	// Cast to array property
	FArrayProperty* ExpBoneDataArrayProp = CastField<FArrayProperty>(ExpBoneDataProp);
	if (!ExpBoneDataArrayProp)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("FixCC5LipsyncPostProcessBlendshapes: ExpBoneData is not an array property"));
		return false;
	}

	// Get the inner struct property (RLExpStruct)
	FStructProperty* InnerStructProp = CastField<FStructProperty>(ExpBoneDataArrayProp->Inner);
	if (!InnerStructProp)
	{
		CONVAI_LOG(ConvaiUtilsLog, Warning, TEXT("FixCC5LipsyncPostProcessBlendshapes: ExpBoneData inner type is not a struct"));
		return false;
	}

	UScriptStruct* RLExpStruct = InnerStructProp->Struct;
	CONVAI_LOG(ConvaiUtilsLog, Log, TEXT("FixCC5LipsyncPostProcessBlendshapes: Inner struct type: %s"), *RLExpStruct->GetName());

	// Get pointer to the array data
	void* ArrayPtr = ExpBoneDataArrayProp->ContainerPtrToValuePtr<void>(PostProcessAnimInstance);
	FScriptArrayHelper ArrayHelper(ExpBoneDataArrayProp, ArrayPtr);

	int32 NumExpEntries = ArrayHelper.Num();
	CONVAI_LOG(ConvaiUtilsLog, Log, TEXT("FixCC5LipsyncPostProcessBlendshapes: ExpBoneData has %d entries"), NumExpEntries);

	int32 TotalModifications = 0;

	// Find property offsets in RLExpStruct - iterate through properties to find them
	FProperty* ExpNameProp = nullptr;
	FProperty* BonesProp = nullptr;

	for (TFieldIterator<FProperty> PropIt(RLExpStruct); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		FString PropName = Prop->GetName();

		if (PropName.Contains(TEXT("ExpName")))
		{
			ExpNameProp = Prop;
		}
		else if (PropName.Contains(TEXT("Bones")))
		{
			BonesProp = Prop;
		}
	}

	if (!ExpNameProp)
	{
		return false;
	}

	if (!BonesProp)
	{
		return false;
	}

	// Cast Bones to array property
	FArrayProperty* BonesArrayProp = CastField<FArrayProperty>(BonesProp);
	if (!BonesArrayProp)
	{
		return false;
	}

	// Get the inner struct of Bones array (RLBoneStruct)
	FStructProperty* BoneStructProp = CastField<FStructProperty>(BonesArrayProp->Inner);
	if (!BoneStructProp)
	{
		return false;
	}

	UScriptStruct* RLBoneStruct = BoneStructProp->Struct;

	// Find BoneName property in RLBoneStruct
	FProperty* BoneNameProp = nullptr;
	for (TFieldIterator<FProperty> PropIt(RLBoneStruct); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		FString PropName = Prop->GetName();

		if (PropName.Contains(TEXT("BoneName")))
		{
			BoneNameProp = Prop;
			break;
		}
	}

	if (!BoneNameProp)
	{
		return false;
	}

	// Cast properties to FNameProperty
	FNameProperty* ExpNameNameProp = CastField<FNameProperty>(ExpNameProp);
	FNameProperty* BoneNameNameProp = CastField<FNameProperty>(BoneNameProp);

	if (!ExpNameNameProp)
	{
		return false;
	}

	if (!BoneNameNameProp)
	{
		return false;
	}

	// Iterate over each entry in ExpBoneData
	for (int32 ExpIndex = 0; ExpIndex < NumExpEntries; ++ExpIndex)
	{
		void* ExpStructPtr = ArrayHelper.GetRawPtr(ExpIndex);

		// Get and modify ExpName
		FName* ExpNamePtr = ExpNameNameProp->ContainerPtrToValuePtr<FName>(ExpStructPtr);
		if (ExpNamePtr)
		{
			FString ExpNameStr = ExpNamePtr->ToString();
			if (ExpNameStr.Contains(TEXT("jaw"), ESearchCase::IgnoreCase))
			{
				FString NewExpNameStr = ExpNameStr.Replace(TEXT("jaw"), TEXT(""), ESearchCase::IgnoreCase);
				*ExpNamePtr = FName(*NewExpNameStr);
				TotalModifications++;
			}
		}

		// Get the Bones array and iterate over it
		void* BonesArrayPtr = BonesArrayProp->ContainerPtrToValuePtr<void>(ExpStructPtr);
		FScriptArrayHelper BonesArrayHelper(BonesArrayProp, BonesArrayPtr);

		int32 NumBones = BonesArrayHelper.Num();
		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			void* BoneStructPtr = BonesArrayHelper.GetRawPtr(BoneIndex);

			// Get and modify BoneName
			FName* BoneNamePtr = BoneNameNameProp->ContainerPtrToValuePtr<FName>(BoneStructPtr);
			if (BoneNamePtr)
			{
				FString BoneNameStr = BoneNamePtr->ToString();
				if (BoneNameStr.Contains(TEXT("jaw"), ESearchCase::IgnoreCase))
				{
					FString NewBoneNameStr = BoneNameStr.Replace(TEXT("jaw"), TEXT(""), ESearchCase::IgnoreCase);
					*BoneNamePtr = FName(*NewBoneNameStr);
					TotalModifications++;
				}
			}
		}
	}

	CONVAI_LOG(ConvaiUtilsLog, Log, TEXT("FixCC5LipsyncPostProcessBlendshapes: Completed with %d modifications"), TotalModifications);
	return TotalModifications > 0;
}

