// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ConvaiDefinitions.h"
#include "Utility/Log/ConvaiLogger.h"
#include "ConvaiUtils.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiUtilsLog, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(ConvaiFormValidationLog, Log, All);

class USoundWave;
class APlayerController;
class UObject;
class UConvaiSubsystem;
class UConvaiObjectComponent;
struct FAnimationFrame;
// FConvaiAudioFrameStats comes in via ConvaiDefinitions.h above — no forward decl needed.

UCLASS()
class CONVAI_API UConvaiUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	static UConvaiSubsystem* GetConvaiSubsystem(const UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "Convai|Utilities")
	static void StereoToMono(TArray<uint8> stereoWavBytes, TArray<uint8>& monoWavBytes);

	UFUNCTION(BlueprintCallable, Category = "Convai|Utilities")
	static bool ReadFileAsByteArray(const FString FilePath, TArray<uint8>& Bytes);

	UFUNCTION(BlueprintCallable, Category = "Convai|Utilities")
	static bool SaveByteArrayAsFile(FString FilePath, TArray<uint8> Bytes);

	UFUNCTION(BlueprintCallable, Category = "Convai|Utilities")
	static FString ByteArrayToString(TArray<uint8> Bytes);

	// Writes a string to a file
	UFUNCTION(BlueprintCallable, Category = "Convai|Utilities")
	static bool WriteStringToFile(const FString& StringToWrite, const FString& FilePath);

	// Reads a string from a file
	UFUNCTION(BlueprintCallable, Category = "Convai|Utilities")
	static bool ReadStringFromFile(FString& OutString, const FString& FilePath);

	static double CalculateAudioDuration(uint32 AudioSize, uint8 Channels, uint32 SampleRate, uint8 SampleSize = 2);

	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Utilities", meta = (WorldContext = "WorldContextObject", AutoCreateRefTerm = "IncludedCharacters, ExcludedCharacters"))
	static void ConvaiGetLookedAtCharacter(UObject* WorldContextObject, APlayerController* PlayerController, float Radius, bool PlaneView, TArray<UObject*> IncludedCharacters, TArray<UObject*> ExcludedCharacters, UConvaiChatbotComponent*& ConvaiCharacter, bool& Found);
	
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Utilities", meta = (WorldContext = "WorldContextObject"))
	static void ConvaiGetLookedAtObjectOrCharacter(UObject* WorldContextObject, APlayerController* PlayerController, float Radius, bool PlaneView, TArray<FConvaiObjectEntry> ListToSearchIn, FConvaiObjectEntry& FoundObjectOrCharacter, bool& Found);

	/**
	 * Get all ConvaiPlayerComponents currently registered in the world.
	 * @param bOnlyConnected  If true, only returns components that have a started/active session.
	 * @param OutOwners       Parallel array of owning Actors (same length and order as the returned components).
	 */
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Utilities", meta = (WorldContext = "WorldContextObject"))
	static void ConvaiGetAllPlayerComponents(UObject* WorldContextObject, bool bOnlyConnected, TArray<class UConvaiPlayerComponent*>& ConvaiPlayerComponents, TArray<AActor*>& OutOwners);

	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Utilities", meta = (WorldContext = "WorldContextObject"))
	static class UConvaiPlayerComponent* GetFirstConvaiPlayerComponent(UObject* WorldContextObject, bool bOnlyConnected, AActor*& OutOwner);

	/**
	 * Get all ConvaiChatbotComponents currently registered in the world.
	 * @param bOnlyConnected  If true, only returns chatbots whose session is in the Connected state (see UConvaiChatbotComponent::IsChatbotConnected).
	 * @param OutOwners       Parallel array of owning Actors (same length and order as the returned components).
	 */
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Utilities", meta = (WorldContext = "WorldContextObject"))
	static void ConvaiGetAllChatbotComponents(UObject* WorldContextObject, bool bOnlyConnected, TArray<class UConvaiChatbotComponent*>& ConvaiChatbotComponents, TArray<AActor*>& OutOwners);

	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Utilities", meta = (WorldContext = "WorldContextObject"))
	static class UConvaiChatbotComponent* GetFirstConvaiChatbotComponent(UObject* WorldContextObject, bool bOnlyConnected, AActor*& OutOwner);

	/**
	 * Get all ConvaiObjectComponents currently registered in the world.
	 * @param OutOwners  Parallel array of owning Actors (same length and order as the returned components).
	 */
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Utilities", meta = (WorldContext = "WorldContextObject"))
	static void ConvaiGetAllObjectComponents(UObject* WorldContextObject, TArray<class UConvaiObjectComponent*>& ConvaiObjectComponents, TArray<AActor*>& OutOwners);

	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Utilities", meta = (WorldContext = "WorldContextObject"))
	static class UConvaiObjectComponent* GetFirstConvaiObjectComponent(UObject* WorldContextObject, AActor*& OutOwner);

	UFUNCTION(BlueprintCallable, Category = "Convai|Settings")
	static void SetAPI_Key(FString API_Key);

	UFUNCTION(BlueprintPure, Category = "Convai|Settings")
	static FString GetAPI_Key();

	/**
	 * Sets a custom parameter in the Convai settings CustomPrams map.
	 * These parameters are picked up by the resolve function (ResolveCustomParam)
	 * which checks defaults, then CustomPrams, then command-line overrides.
	 * Use this to set keys such as "client_version" at runtime from Blueprint.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Settings")
	static void SetCustomParam(const FString& Key, const FString& Value);

	/**
	 * Gets a custom parameter value using the full resolve chain:
	 * built-in defaults → CustomPrams settings → command-line override.
	 * Returns DefaultValue if the resolved result is empty.
	 */
	UFUNCTION(BlueprintPure, Category = "Convai|Settings")
	static FString GetCustomParam(const FString& Key, const FString& DefaultValue = TEXT(""));

	/** Returns the client_version resolved via the CustomPrams / command-line resolve chain. */
	UFUNCTION(BlueprintPure, Category = "Convai|Settings")
	static FString GetClientVersion();

	UFUNCTION(BlueprintCallable, Category = "Convai|Settings")
	static void SetAuthToken(FString AuthToken);

	UFUNCTION(BlueprintPure, Category = "Convai|Settings")
	static FString GetAuthToken();

	static TPair<FString, FString> GetAuthHeaderAndKey();

	UFUNCTION(BlueprintPure, Category = "Convai|Settings")
	static FString GetTestCharacterID();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static FString GetStreamURL();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static FString GetLLMProvider();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static FString GetConnectionType();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static bool IsAECEnabled();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static bool IsVADEnabled();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static FString GetAECType();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static bool IsNoiseSuppressionEnabled();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static bool IsGainControlEnabled();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static int32 GetVADMode();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static bool IsHighPassFilterEnabled();

	/** Returns the current VAD settings from project settings (mutable snapshot). */
	UFUNCTION(BlueprintPure, Category = "Convai|Settings")
	static FConvaiVADSettings GetVADSettings();

	/** Overwrites the VAD settings in the active UConvaiSettings instance. Persisted to .ini on save. */
	UFUNCTION(BlueprintCallable, Category = "Convai|Settings")
	static void SetVADSettings(const FConvaiVADSettings& InSettings);

	static int32 GetChunkSize();

	static int32 GetOutputFPS();

	static float GetFramesBufferDuration();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static bool IsFaceSyncSimulateFreeze();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static FString GetLipSyncAnimParamOverride(const FString& Key);

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static EC_LipSyncMode GetLipSyncMode();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static double GetLipSyncTimeOffset();

	UFUNCTION(BlueprintPure, Category = "Convai|Settings")
	static bool IsAlwaysAllowVisionEnabled();
	
	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static FString GetEmotionsProvider();
	
	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static float GetConnectionProxyTTL();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static float GetPrepConnectionTTL();
	
	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static float GetLipSyncStarvationFallback();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static float GetObjectPollIntervalSeconds();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static float GetClientReadyRetrySecs();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static float GetClientReadyTimeoutSecs();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static float GetContextDebounceWindowDefault();

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static float GetContextMaxDebounceWindowDefault();
	
	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static void GetPluginInfo(const FString& PluginName, bool& Found, FString& VersionName, FString& EngineVersion, FString& FriendlyName);

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static void GetPlatformInfo(FString& EngineVersion, FString& PlatformName);

	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static FString GetDeviceUniqueIdentifier();

	/**
	 * Maps and transforms blendshapes from one naming convention/rig to another.
	 * Supports per-blendshape multipliers, offsets, clamping, and override values.
	 *
	 * This is useful for:
	 * - Converting ARKit blendshapes to MetaHuman blendshapes
	 * - Remapping custom character rigs
	 * - Applying global scaling and offset to all blendshapes
	 * - Overriding specific blendshape values
	 * - Clamping blendshape values to valid ranges
	 *
	 * @param InputBlendshapes The source blendshape map (e.g., from ARKit or AI system)
	 * @param BlendshapeMap Mapping configuration for each blendshape (multipliers, offsets, target names, etc.)
	 * @param GlobalMultiplier Global multiplier applied to all blendshapes (unless IgnoreGlobalModifiers is set)
	 * @param GlobalOffset Global offset added to all blendshapes (unless IgnoreGlobalModifiers is set)
	 * @return Transformed blendshape map ready for the target character rig
	 *
	 * Example:
	 * - Input: {"jawOpen": 0.5}
	 * - BlendshapeMap: {"jawOpen" -> TargetNames: ["CTRL_expressions_mouthOpen"], Multiplier: 2.0, Offset: 0.1}
	 * - GlobalMultiplier: 1.0, GlobalOffset: 0.0
	 * - Output: {"CTRL_expressions_mouthOpen": 1.1}  // (0.5 * 2.0 * 1.0) + 0.1 + 0.0
	 */
	UFUNCTION(BlueprintPure, Category = "Convai|Blendshapes")
	static TMap<FName, float> MapBlendshapes(const TMap<FName,float>& InputBlendshapes, const TMap<FName, FConvaiBlendshapeParameters>& BlendshapeMap, float GlobalMultiplier, float GlobalOffset);


	/**
	 * Splits a blendshape map into two maps based on a list of keys.
	 * Keys found in SplitKeys are moved to OutSplitMap and removed from InOutOriginalMap.
	 *
	 * This is useful for:
	 * - Separating blendshapes that need different blend modes (additive vs replace)
	 * - Isolating specific facial regions (eyes, mouth, etc.) for independent control
	 * - Creating layered animation systems
	 *
	 * Performance: O(n) where n is the number of blendshapes in the original map
	 * Uses TSet internally for O(1) key lookups
	 *
	 * @param InOutOriginalMap The original map. Keys matching SplitKeys will be removed from this map.
	 * @param SplitKeys Array of blendshape names to extract from the original map.
	 * @param OutSplitMap Output map containing only the key-value pairs whose keys are in SplitKeys.
	 *
	 * Example:
	 * - InOutOriginalMap: {"eyeBlinkL": 0.5, "jawOpen": 0.3, "eyeBlinkR": 0.5}
	 * - SplitKeys: ["eyeBlinkL", "eyeBlinkR"]
	 * - After execution:
	 *   - InOutOriginalMap: {"jawOpen": 0.3}
	 *   - OutSplitMap: {"eyeBlinkL": 0.5, "eyeBlinkR": 0.5}
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Blendshapes", meta = (DisplayName = "Split Blendshape Map by Keys"))
	static void SplitBlendshapeMapByKeys(
		UPARAM(ref) TMap<FName, float>& InOutOriginalMap,
		const TArray<FName>& SplitKeys,
		TMap<FName, float>& OutSplitMap
	);

	/**
	 * Merges two blendshape maps together.
	 * If a key exists in both maps, the value from OverrideMap takes precedence.
	 *
	 * This is useful for:
	 * - Combining blendshapes from multiple sources (AI + manual animation)
	 * - Applying corrective blendshapes on top of base animation
	 * - Layering different animation systems
	 *
	 * @param BaseMap The base map to merge into.
	 * @param OverrideMap The map whose values will override the base map.
	 * @return A new map containing all key-value pairs from both maps.
	 *
	 * Example:
	 * - BaseMap: {"eyeBlinkL": 0.3, "jawOpen": 0.5}
	 * - OverrideMap: {"eyeBlinkL": 0.8, "browUp": 0.2}
	 * - Result: {"eyeBlinkL": 0.8, "jawOpen": 0.5, "browUp": 0.2}
	 */
	UFUNCTION(BlueprintPure, Category = "Convai|Blendshapes", meta = (DisplayName = "Merge Blendshape Maps"))
	static TMap<FName, float> MergeBlendshapeMaps(
		const TMap<FName, float>& BaseMap,
		const TMap<FName, float>& OverrideMap
	);

	static TArray<uint8> ExtractPCMDataFromSoundWave(USoundWave* SoundWave, int32& OutSampleRate, int32& OutNumChannels);

	static void PCMDataToWav(TArray<uint8> InPCMBytes, TArray<uint8>& OutWaveFileData, int NumChannels, int SampleRate);

	static USoundWave* PCMDataToSoundWav(TArray<uint8> InPCMBytes, int NumChannels, int SampleRate);

	static USoundWave* WavDataToSoundWave(TArray<uint8> InWavData);

	// Writes a USoundWave to a .wav file on disk
	UFUNCTION(BlueprintCallable, Category = "Convai|Utilities")
	static bool WriteSoundWaveToWavFile(USoundWave* SoundWave, const FString& FilePath);

	// Reads a .wav file from disk and creates a USoundWave
	UFUNCTION(BlueprintPure, Category = "Convai|Utilities")
	static USoundWave* ReadWavFileAsSoundWave(const FString& FilePath);

	static void ResampleAudio(float currentSampleRate, float targetSampleRate, int numChannels, bool reduceToMono, int16* currentPcmData, int numSamplesToConvert, TArray<int16>& outResampledPcmData);

	static void ResampleAudio(float currentSampleRate, float targetSampleRate, int numChannels, bool reduceToMono, const TArray<int16>& currentPcmData, int numSamplesToConvert, TArray<int16>& outResampledPcmData);

	static FString FUTF8ToFString(const char* StringToConvert);

	static int LevenshteinDistance(const FString& s, const FString& t);

	static TArray<FAnimationFrame> ParseJsonToBlendShapeData(const FString& JsonString);

	static bool ParseVisemeValuesToAnimationFrame(const FString& VisemeValuesString, FAnimationFrame& AnimationFrame);

	// UFUNCTION(BlueprintCallable, Category = "ActorFuncions", meta = (WorldContext = WorldContextObject))
	static AActor* ConvaiCloneActor(AActor* InputActor);

	// UFUNCTION(BlueprintPure, Category = "Convai|Utilities|AnimationSequence")
	static FString ConvaiAnimationSequenceToJson(const FAnimationSequenceBP& AnimationSequenceBP);

	// UFUNCTION(BlueprintPure, Category = "Convai|Utilities|AnimationSequence")
	static void ConvaiAnimationSequenceFromJson(const FString& JsonString, FAnimationSequenceBP& AnimationSequenceBP);
	
	/** Helper function to detect if audio data contains actual content vs silence.
	 *  Peak-only check — fast, but treats steady background noise / DC offset as
	 *  audio. For tail-of-speech detection where ruling out steady hum matters,
	 *  prefer ContainsActualAudio below. */
	static bool ContainsAudioContent(const int16_t* AudioData, size_t NumFrames, uint32_t NumChannels, int16_t AudioContentThreshold = 50);

	/**
	 * Decide whether a PCM16 frame contains actual audio (speech / impulses) as
	 * opposed to true silence or steady background noise.
	 *
	 * Two combined signals separate the cases:
	 *   - PeakAmplitude > AmplitudeThreshold  → some signal is present.
	 *   - Variance      > VarianceThreshold   → the signal is modulated (speech
	 *                                           varies sample-to-sample; hum / DC
	 *                                           offset / capture-line bias does not).
	 *
	 * Cross-frame smoothing: if PrevStats is non-zero, a current-frame RMS that
	 * collapsed to <10 % of PrevStats.RMS *and* failed the variance threshold is
	 * treated as the tail end of speech regardless of peak — handles the
	 * "loud sample, then sudden quiet" case at the end of an utterance.
	 *
	 * OutCurrentStats is always populated so callers can feed it back in as
	 * PrevStats on the next call. Pass a default-constructed FConvaiAudioFrameStats
	 * for the first frame.
	 *
	 * Thresholds (`AmplitudeThreshold` = 50, `VarianceThreshold` = 1000.0f) are
	 * calibrated against int16 PCM at typical mic gains: 50 sits well above the
	 * quietest hardware noise floor, 1000.0 corresponds to an RMS jitter of ~32
	 * which steady hum doesn't reach but normal speech easily clears.
	 */
	static bool ContainsActualAudio(
		const int16_t* AudioData,
		size_t NumFrames,
		uint32_t NumChannels,
		const FConvaiAudioFrameStats& PrevStats,
		FConvaiAudioFrameStats& OutCurrentStats,
		int16_t AmplitudeThreshold = 50,
		float   VarianceThreshold  = 1000.0f);

	/**
	 * Fixes lipsync blendshapes in the post process animation blueprint by removing "jaw" from bone/expression names.
	 * This is useful for fixing conflicts between post-process facial animation and lipsync.
	 *
	 * The function accesses the skeletal mesh's post process anim instance and looks for a blueprint-defined
	 * property named "ExpBoneData" (array of RLExpStruct), then removes the word "jaw" from ExpName and BoneName fields.
	 *
	 * @param AnimInstance The animation instance to get the owning skeletal mesh component from
	 * @return True if the fix was successfully applied, false otherwise
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|LipSync")
	static bool FixCC5LipsyncPostProcessBlendshapes(UAnimInstance* AnimInstance);

	/**
	 * Splits an actor's skeletal mesh components into a body and (optionally) a face mesh,
	 * using bone-name heuristics. Useful for MetaHuman-style rigs that separate the face,
	 * but also handles single-mesh and unrecognized rigs.
	 *
	 * Other skeletal mesh components on the actor (hair, weapons, clothing, etc.) are
	 * ignored as long as their bones don't substring-match body or face tokens.
	 *
	 *  - 0 skeletal mesh components on the actor: both outputs are null.
	 *  - Exactly 1: that mesh becomes the body, face is null. No bone inspection.
	 *  - 2+: each candidate is scored by substring-matching bones against body
	 *        tokens ("pelvis", "hand", "neck", ...) and face tokens ("eye", "jaw",
	 *        "brow", ...). Highest body score wins body; highest face score wins
	 *        face. If no mesh has any body bones, falls back to the first
	 *        component as body. If no mesh has any face bones, face is null.
	 *
	 * @param Actor          The actor to inspect.
	 * @param BodyComponent  Out: the body skeletal mesh, or null if Actor has none.
	 * @param FaceComponent  Out: the face skeletal mesh, or null if no face was identified.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Utilities")
	static void GetBodyAndFaceSkeletalMeshComponents(AActor* Actor, class USkeletalMeshComponent*& BodyComponent, class USkeletalMeshComponent*& FaceComponent);
};

UCLASS()
class CONVAI_API UConvaiSettingsUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	static bool GetParamValueAsString(const FString& paramName, FString& outValue);

	static bool GetParamValueAsFloat(const FString& paramName, float& outValue);

	static bool GetParamValueAsInt(const FString & paramName, int32 & outValue);
};

UCLASS()
class CONVAI_API UConvaiFormValidation : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	static bool ValidateAuthKey(FString API_Key)
	{
		if ((API_Key.Len()))
		{
			return true;
		}
		else
		{
			CONVAI_LOG(LogTemp, Warning, TEXT("Empty API Key, please add it in Edit->Project Settings->Convai"));
			return false;
		}
	}

	static bool ValidateSessionID(FString SessionID)
	{
		if ((SessionID.Len()))
		{
			return true;
		}
		else
		{
			CONVAI_LOG(ConvaiFormValidationLog, Warning, TEXT("Empty Session ID"));
			return false;
		}
	}

	static bool ValidateCharacterID(FString CharacterID)
	{
		if ((CharacterID.Len()))
		{
			return true;
		}
		else
		{
			CONVAI_LOG(ConvaiFormValidationLog, Warning, TEXT("Empty Character ID"));
			return false;
		}
	}

	static bool ValidateInputText(FString InputText)
	{
		if ((InputText.Len()))
		{
			return true;
		}
		else
		{
			CONVAI_LOG(LogTemp, Warning, TEXT("Empty Input Text"));
			return false;
		}
	}

	static bool ValidateVoiceType(FString VoiceType)
	{
		if ((VoiceType.Len()))
		{
			return true;
		}
		else
		{
			CONVAI_LOG(ConvaiFormValidationLog, Warning, TEXT("Invalid Voice Type"));
			return false;
		}
	}

	static bool ValidateBackstory(FString Backstory)
	{
		if ((Backstory.Len()))
		{
			return true;
		}
		else
		{
			CONVAI_LOG(ConvaiFormValidationLog, Warning, TEXT("Empty Backstory"));
			return false;
		}
	}

	static bool ValidateCharacterName(FString CharacterName)
	{
		if ((CharacterName.Len()))
		{
			return true;
		}
		else
		{
			CONVAI_LOG(ConvaiFormValidationLog, Warning, TEXT("Empty Character Name"));
			return false;
		}
	}

	static bool ValidateInputVoice(TArray<uint8> InputVoiceData)
	{
		if ((InputVoiceData.Num() > 44))
		{
			return true;
		}
		else
		{
			CONVAI_LOG(ConvaiFormValidationLog, Warning, TEXT("Input Voice is too short (less than 44 bytes)"));
			return false;
		}
	}
};

UCLASS()
class CONVAI_API UCommandLineUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	// Function to check if a flag is present in the command line - Has issue it always returns false
	//UFUNCTION(BlueprintCallable, Category = "CommandLine")
	static bool IsCommandLineFlagPresent(const FString& Flag);

	// Function to get the value of a command line flag as an integer
	UFUNCTION(BlueprintCallable, Category = "CommandLine")
	static int32 GetCommandLineFlagValueAsInt(const FString& Flag, int32 DefaultValue = 0);

	// Function to get the value of a command line flag as a string
	UFUNCTION(BlueprintCallable, Category = "CommandLine")
	static FString GetCommandLineFlagValueAsString(const FString& Flag, const FString& DefaultValue = TEXT(""));

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CommandLine")
	static FString GetCommandLineFlagValueAsStringNoDefault(const FString& Flag);

	// Function to get the value of a command line flag as an integer
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CommandLine")
	static double GetCommandLineFlagValueAsDouble(const FString& Flag, double DefaultValue = 0.0f);
};