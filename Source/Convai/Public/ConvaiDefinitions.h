// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Engine/GameEngine.h"
#include "Runtime/Launch/Resources/Version.h"
#include "RestAPI/ConvaiURL.h"
#include "ConvaiDefinitions.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiDefinitionsLog, Log, All);

USTRUCT(BlueprintType)
struct CONVAI_API FNarrativeDecision
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString criteria;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString next_section_id;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	int32 priority;

    FNarrativeDecision()
        : criteria(TEXT("")), // Initialize with default empty string
		next_section_id(TEXT("")), // Initialize with default empty string
		priority(0) // Initialize with default priority
    {
    }
};

USTRUCT(BlueprintType)
struct CONVAI_API FNarrativeTrigger
{
	GENERATED_BODY()

	UPROPERTY()
	FString character_id;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString destination_section;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString trigger_id;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString trigger_message;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString trigger_name;

	FNarrativeTrigger()
		: destination_section(TEXT("")),
		trigger_id(TEXT("")),
		trigger_message(TEXT("")),
		trigger_name(TEXT(""))
	{
	}
};

USTRUCT(BlueprintType)
struct CONVAI_API FNarrativeSection
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString behavior_tree_code;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString bt_constants;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString character_id;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	TArray<FNarrativeDecision> decisions;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString objective;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	TArray<FString> parents;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString section_id;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	FString section_name;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Narrative Design")
	TMap<FString, FString> updated_character_data;

    FNarrativeSection()
        : behavior_tree_code(TEXT("")),
		bt_constants(TEXT("")),
		character_id(TEXT("")),
		objective(TEXT("")),
		section_id(TEXT("")),
		section_name(TEXT(""))
    {
    }
};

UENUM(BlueprintType)
enum class ETTS_Voice_Type : uint8
{
	MALE,
	FEMALE,
	WUKMale_1,
	WUKFemale_1,
	SUKMale_1,
	WAFemale_1,
	WAMale_1,
	SIFemale_1,
	SIMale_1,
	SUFemale_1,
	SUMale_1,
	WUFemale_1,
	WUMale_1,
	Trixie,
	Twilight_Sparkle,
	Celestia,
	Spike,
	Applejack,
};

UENUM(BlueprintType)
enum class EEmotionIntensity : uint8
{
	Basic        UMETA(DisplayName = "Basic"),
	LessIntense  UMETA(DisplayName = "Less Intense"),
	MoreIntense  UMETA(DisplayName = "More Intense"),
	None         UMETA(DisplayName = "None", BlueprintHidden) // To handle cases when the emotion is not found
};

UENUM(BlueprintType)
enum class EBasicEmotions : uint8
{
	Joy          UMETA(DisplayName = "Happy"),
	Trust        UMETA(DisplayName = "Calm"),
	Fear         UMETA(DisplayName = "Afraid"),
	Surprise     UMETA(DisplayName = "Surprise"),
	Sadness      UMETA(DisplayName = "Sad"),
	Disgust      UMETA(DisplayName = "Bored"),
	Anger        UMETA(DisplayName = "Angry"),
	Anticipation UMETA(DisplayName = "Anticipation", Hidden), // No longer used
	None         UMETA(DisplayName = "None", Hidden) // To handle cases when the emotion is not found
};

USTRUCT(BlueprintType)
struct FConvaiObjectEntry
{
	GENERATED_USTRUCT_BODY()

public:
	/** A refrence of a character or object*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		TWeakObjectPtr<AActor> Ref;

	/** A related position vector*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FVector OptionalPositionVector;

	/** The Name of the character or object*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FString Name;


	/** The bio/description for the chracter/object*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FString Description;

	friend bool operator==(const FConvaiObjectEntry& lhs, const FConvaiObjectEntry& rhs)
	{
		return lhs.Name == rhs.Name;
	}

	FConvaiObjectEntry()
		: Ref(nullptr)
		, OptionalPositionVector(FVector(0, 0, 0))
		, Name(FString(""))
		, Description(FString(""))
	{
	}
};

USTRUCT(BlueprintType)
struct FConvaiExtraParams
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		float Number;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FString Text;

	FConvaiExtraParams()
		:Number(0),
		Text("")
	{

	}
};

USTRUCT(BlueprintType)
struct FConvaiResultAction
{
	GENERATED_BODY()

		/** The action to be made*/
		UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FString Action;

	/** The object or character whom the action is to be made on*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FConvaiObjectEntry RelatedObjectOrCharacter;

	/** The actual string of the action without any preprocessing*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FString ActionString;

	/** Has extra parameters*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Action API")
		FConvaiExtraParams ConvaiExtraParams;
};

USTRUCT(BlueprintType)
struct FConvaiBlendshapeParameters
{
	GENERATED_BODY()

		UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		TArray<FName> TargetNames;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		float Multiplyer = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		float Offset = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		bool UseOverrideValue = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		bool IgnoreGlobalModifiers = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		float OverrideValue = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		float ClampMinValue = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|LipSync")
		float ClampMaxValue = 1;
};

USTRUCT()
struct FAnimationFrame
{
	GENERATED_BODY()

	UPROPERTY()
	int32 FrameIndex = 0;

	UPROPERTY()
	TMap<FName, float> BlendShapes;

	FString ToString()
	{
		FString Result;

		// iterate over all elements in the TMap
		for (const auto& Elem : BlendShapes)
		{
			// Append the key-value pair to the result string
			Result += Elem.Key.ToString() + TEXT(": ") + FString::SanitizeFloat(Elem.Value) + TEXT(", ");
		}

		// Remove the trailing comma and space for cleanliness, if present
		if (Result.Len() > 0)
		{
			Result.RemoveAt(Result.Len() - 2);
		}

		return Result;
	}
};

USTRUCT()
struct FAnimationSequence
{
	GENERATED_BODY()
public:
	UPROPERTY()
	TArray<FAnimationFrame> AnimationFrames;

	UPROPERTY()
	double Duration = 0;

	UPROPERTY()
	int32 FrameRate = 0;

	// Serialize this struct to a JSON string
	FString ToJson() const
	{
		TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);

		// Convert AnimationFrames to a JSON array
		TArray<TSharedPtr<FJsonValue>> JsonFrames;
		for (const FAnimationFrame& Frame : AnimationFrames)
		{
			TSharedPtr<FJsonObject> JsonFrameObject = MakeShareable(new FJsonObject);
			JsonFrameObject->SetNumberField(TEXT("FrameIndex"), Frame.FrameIndex);

			// Convert BlendShapes to a JSON object
			TSharedPtr<FJsonObject> JsonBlendShapes = MakeShareable(new FJsonObject);
			for (const auto& Elem : Frame.BlendShapes)
			{
				JsonBlendShapes->SetNumberField(Elem.Key.ToString(), Elem.Value);
			}
			JsonFrameObject->SetObjectField(TEXT("BlendShapes"), JsonBlendShapes);

			JsonFrames.Add(MakeShareable(new FJsonValueObject(JsonFrameObject)));
		}
		JsonObject->SetArrayField(TEXT("AnimationFrames"), JsonFrames);

		// Set the rest of the properties
		JsonObject->SetNumberField(TEXT("Duration"), Duration);
		JsonObject->SetNumberField(TEXT("FrameRate"), FrameRate);

		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

		return OutputString;
	}

	// Deserialize this struct from a JSON string
	bool FromJson(const FString& JsonString)
	{
		AnimationFrames.Empty();
		Duration = 0;
		FrameRate = 0;
		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
		if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonFrames;
			if (JsonObject->TryGetArrayField(TEXT("AnimationFrames"), JsonFrames))
			{
				for (const TSharedPtr<FJsonValue>& JsonValue : *JsonFrames)
				{
					TSharedPtr<FJsonObject> JsonFrameObject = JsonValue->AsObject();
					if (JsonFrameObject.IsValid())
					{
						FAnimationFrame Frame;
						Frame.FrameIndex = JsonFrameObject->GetIntegerField(TEXT("FrameIndex"));

						TSharedPtr<FJsonObject> JsonBlendShapes = JsonFrameObject->GetObjectField(TEXT("BlendShapes"));
						for (const auto& Elem : JsonBlendShapes->Values)
						{
							Frame.BlendShapes.Add(FName(*Elem.Key), Elem.Value->AsNumber());
						}

						AnimationFrames.Add(Frame);
					}
				}
			}

#if ENGINE_MAJOR_VERSION < 5
			double tempDuration;
			JsonObject->TryGetNumberField(TEXT("Duration"), tempDuration);
			Duration = (float)tempDuration;
#else
			JsonObject->TryGetNumberField(TEXT("Duration"), Duration);
#endif
			JsonObject->TryGetNumberField(TEXT("FrameRate"), FrameRate);

			return true;
		}
		return false;
	}
};

USTRUCT()
struct FAnimationSequenceBP
{
	GENERATED_BODY()
public:
	UPROPERTY()
	FAnimationSequence AnimationSequence = FAnimationSequence();
};

/** Result of frame selection calculation for interpolation */
struct FFrameSelectionResult
{
	TMap<FName, float> StartFrame;
	TMap<FName, float> EndFrame;
	float Alpha = 0.0f;
	int32 FrameIndex = 0;
	int32 BufferIndex = 0;
	bool bValid = false;
};

USTRUCT()
struct FConvaiEmotionState
{
	GENERATED_BODY()

public:
	
	// Deprecated
	void GetEmotionDetails(const FString& Emotion, EEmotionIntensity& Intensity, EBasicEmotions& BasicEmotion)
	{
		// Static dictionaries of emotions
		static const TMap<FString, EBasicEmotions> BasicEmotions = {
			{"Joy", EBasicEmotions::Joy},
			{"Trust", EBasicEmotions::Trust},
			{"Fear", EBasicEmotions::Fear},
			{"Surprise", EBasicEmotions::Surprise},
			{"Sadness", EBasicEmotions::Sadness},
			{"Disgust", EBasicEmotions::Disgust},
			{"Anger", EBasicEmotions::Anger},
			{"Anticipation", EBasicEmotions::Anticipation}
		};

		static const TMap<FString, EBasicEmotions> LessIntenseEmotions = {
			{"Serenity", EBasicEmotions::Joy},
			{"Acceptance", EBasicEmotions::Trust},
			{"Apprehension", EBasicEmotions::Fear},
			{"Distraction", EBasicEmotions::Surprise},
			{"Pensiveness", EBasicEmotions::Sadness},
			{"Boredom", EBasicEmotions::Disgust},
			{"Annoyance", EBasicEmotions::Anger},
			{"Interest", EBasicEmotions::Anticipation}
		};

		static const TMap<FString, EBasicEmotions> MoreIntenseEmotions = {
			{"Ecstasy", EBasicEmotions::Joy},
			{"Admiration", EBasicEmotions::Trust},
			{"Terror", EBasicEmotions::Fear},
			{"Amazement", EBasicEmotions::Surprise},
			{"Grief", EBasicEmotions::Sadness},
			{"Loathing", EBasicEmotions::Disgust},
			{"Rage", EBasicEmotions::Anger},
			{"Vigilance", EBasicEmotions::Anticipation}
		};

		// Initialize the output parameters
		Intensity = EEmotionIntensity::None;
		BasicEmotion = EBasicEmotions::None;

		// Look up the emotion
		if (BasicEmotions.Contains(Emotion))
		{
			Intensity = EEmotionIntensity::Basic;
			BasicEmotion = BasicEmotions[Emotion];
		}
		else if (LessIntenseEmotions.Contains(Emotion))
		{
			Intensity = EEmotionIntensity::LessIntense;
			BasicEmotion = LessIntenseEmotions[Emotion];
		}
		else if (MoreIntenseEmotions.Contains(Emotion))
		{
			Intensity = EEmotionIntensity::MoreIntense;
			BasicEmotion = MoreIntenseEmotions[Emotion];
		}
	}

	// Deprecated
	void SetEmotionData(const FString& EmotionRespponse, float EmotionOffset)
	{
		TArray<FString> OutputEmotionsArray;
		// Separate the string into an array based on the space delimiter
		EmotionRespponse.ParseIntoArray(OutputEmotionsArray, TEXT(" "), true);
		SetEmotionData(OutputEmotionsArray, EmotionOffset);
	}

	// Deprecated
	void SetEmotionData(const TArray<FString>& EmotionArray, float EmotionOffset)
	{
		ResetEmotionScores();
		EEmotionIntensity Intensity = EEmotionIntensity::None;
		EBasicEmotions BasicEmotion = EBasicEmotions::None;
		float Score = 0;

		int i = 0;
		for (FString Emotion : EmotionArray)
		{
			GetEmotionDetails(Emotion, Intensity, BasicEmotion);
			if (Intensity == EEmotionIntensity::None || BasicEmotion == EBasicEmotions::None)
				continue;

			if (const float* ScoreMultiplier = ScoreMultipliers.Find(Intensity))
			{
				Score = *ScoreMultiplier * (FMath::Exp(float(-i) / float(EmotionArray.Num())) + EmotionOffset);
				Score = Score > 1 ? 1 : Score;
				Score = Score < 0 ? 0 : Score;
			}
			else
			{
				Score = 0;
			}

			EmotionsScore.Add(BasicEmotion, Score);
			i++;
		}

	}


	void ForceSetEmotion(const EBasicEmotions& BasicEmotion, const EEmotionIntensity& Intensity, const bool& ResetOtherEmotions)
	{
		if (ResetOtherEmotions)
		{
			ResetEmotionScores();
		}

		float Score = 0;
		if (const float* ScoreMultiplier = ScoreMultipliers.Find(Intensity))
		{
			Score = *ScoreMultiplier;
		}
		else
		{
			Score = 0;
		}

		EmotionsScore.Add(BasicEmotion, Score);
	}

	void GetTTSEmotion(const FString& Emotion, EBasicEmotions& BasicEmotion)
	{
		// Static dictionaries of emotions
		static const TMap<FString, EBasicEmotions> BasicEmotions = {
			{"Joy", EBasicEmotions::Joy},
			{"Calm", EBasicEmotions::Trust},
			{"Fear", EBasicEmotions::Fear},
			{"Surprise", EBasicEmotions::Surprise},
			{"Sadness", EBasicEmotions::Sadness},
			{"Bored", EBasicEmotions::Disgust},
			{"Anger", EBasicEmotions::Anger},
			{"Neutral", EBasicEmotions::None}
		};

		// Initialize the output parameters
		BasicEmotion = EBasicEmotions::None;

		// Look up the emotion
		if (BasicEmotions.Contains(Emotion))
		{
			BasicEmotion = BasicEmotions[Emotion];
		}
	}


	void SetEmotionDataSingleEmotion(const FString& EmotionRespponse, float EmotionOffset)
	{
		FString EmotionString;
		float Scale;
		float MaxScale = 3;
		ParseStringToStringAndFloat(EmotionRespponse, EmotionString, Scale);


		Scale /= MaxScale;

		// Increase the scale a bit
		Scale += EmotionOffset;
		Scale = Scale > 1? 1 : Scale;
		Scale = Scale < 0? 0 : Scale;

		EBasicEmotions Emotion;
		GetTTSEmotion(EmotionString, Emotion);

		ResetEmotionScores();
		EmotionsScore.Add(Emotion, Scale);
	}
	float GetEmotionScore(const EBasicEmotions& Emotion)
	{
		float Score = 0;
		if (const float* ScorePointer = EmotionsScore.Find(Emotion))
		{
			Score = *ScorePointer;
		}
		return Score;
	}

	void ResetEmotionScores()
	{
		EmotionsScore.Empty();

		for (int32 i = 0; i <= static_cast<int32>(EBasicEmotions::Anticipation); ++i)
		{
			EBasicEmotions EnumValue = static_cast<EBasicEmotions>(i);
			EmotionsScore.Add(EnumValue, 0);
		}
	}

private:
	TMap<EBasicEmotions, float> EmotionsScore;

	static const TMap<EEmotionIntensity, float> ScoreMultipliers;

	bool ParseStringToStringAndFloat(const FString& Input, FString& OutString, float& OutFloat)
	{
		// Split the input string into two parts based on the space character
		TArray<FString> Parsed;
		Input.ParseIntoArray(Parsed, TEXT(" "), true);

		// Check if the parsing was successful
		if (Parsed.Num() == 2)
		{
			// Assign the first part to OutString
			OutString = Parsed[0];

			// Convert the second part to a float and assign it to OutFloat
			OutFloat = FCString::Atof(*Parsed[1]);
			return true;
		}
		else
		{
			// Handle the error case
			OutString = TEXT("");
			OutFloat = 0.0f;
			return false;
		}
	}
};

USTRUCT()
struct FConvaiEnvironmentDetails
{
	GENERATED_BODY()

public:

	UPROPERTY()
		TArray<FString> Actions;

	UPROPERTY()
		TArray<FConvaiObjectEntry> Objects;

	UPROPERTY()
		TArray<FConvaiObjectEntry> Characters;

	UPROPERTY()
		FConvaiObjectEntry MainCharacter;

	UPROPERTY()
		FConvaiObjectEntry AttentionObject;
};

// TODO: OnEnvironmentChanged event should be called in an optimizied way for any change in the environment

UCLASS(Blueprintable)
class UConvaiEnvironment : public UObject
{
	GENERATED_BODY()
public:

	DECLARE_DELEGATE(FEnvironmentChangedEventSignature);

	FEnvironmentChangedEventSignature OnEnvironmentChanged;

	// Creates a Convai Environment object.
	UFUNCTION(BlueprintCallable, Category = "Convai|Action API")
		static UConvaiEnvironment* CreateConvaiEnvironment()
	{
		UConvaiEnvironment* ContextObject = NewObject<UConvaiEnvironment>();
		return ContextObject;
	}

	void SetFromEnvironment(UConvaiEnvironment* InEnvironment)
	{
		if (IsValid(InEnvironment))
		{
			Objects = InEnvironment->Objects;
			Characters = InEnvironment->Characters;
			Actions = InEnvironment->Actions;
			MainCharacter = InEnvironment->MainCharacter;
			AttentionObject = InEnvironment->AttentionObject;
			OnEnvironmentChanged.ExecuteIfBound();
		}
	}

	void SetFromEnvironment(FConvaiEnvironmentDetails InEnvironment)
	{
		Objects = InEnvironment.Objects;
		Characters = InEnvironment.Characters;
		Actions = InEnvironment.Actions;
		MainCharacter = InEnvironment.MainCharacter;
		AttentionObject = InEnvironment.AttentionObject;
		OnEnvironmentChanged.ExecuteIfBound();
	}

	FConvaiEnvironmentDetails ToEnvironmentStruct()
	{
		FConvaiEnvironmentDetails OutStruct;
		OutStruct.Objects = Objects;
		OutStruct.Characters = Characters;
		OutStruct.Actions = Actions;
		OutStruct.MainCharacter = MainCharacter;
		OutStruct.AttentionObject = AttentionObject;
		return OutStruct;
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void AddAction(FString Action)
	{
		Actions.AddUnique(Action);
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void AddActions(TArray<FString> ActionsToAdd)
	{
		for (auto a : ActionsToAdd)
			Actions.AddUnique(a);
		OnEnvironmentChanged.ExecuteIfBound();
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void RemoveAction(FString Action)
	{
		Actions.Remove(Action);
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void RemoveActions(TArray<FString> ActionsToRemove)
	{
		for (auto a : ActionsToRemove)
			Actions.Remove(a);
		OnEnvironmentChanged.ExecuteIfBound();
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void ClearAllActions()
	{
		Actions.Empty();
		OnEnvironmentChanged.ExecuteIfBound();
	}

	FConvaiObjectEntry* FindObject(FString ObjectName)
	{
		for (FConvaiObjectEntry& o : Objects)
		{
			if (ObjectName == o.Name)
			{
				return &o;
			}
		}
		return nullptr;
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void AddObject(FConvaiObjectEntry Object)
	{
		// Replace old object that has the same name with the new object
		if (FConvaiObjectEntry* ExistingObject = FindObject(Object.Name))
		{
			ExistingObject->Description = Object.Description;
			ExistingObject->OptionalPositionVector = Object.OptionalPositionVector;
			ExistingObject->Ref = Object.Ref;
		}
		else
		{
			Objects.AddUnique(Object);
		}
	}

	/**
	 *    Adds a list of objects to the Environment object
	 *    @param Objects			A map of objects following this format [UObject refrence : Description]
	 */
	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void AddObjects(TArray<FConvaiObjectEntry> ObjectsToAdd)
	{
		for (auto o : ObjectsToAdd)
			AddObject(o);
		OnEnvironmentChanged.ExecuteIfBound();
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void RemoveObject(FString ObjectName)
	{
		for (auto o : Objects)
			if (ObjectName == o.Name)
				Objects.Remove(o);
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void RemoveObjects(TArray<FString> ObjectNamesToRemove)
	{
		for (auto n : ObjectNamesToRemove)
			RemoveObject(n);
		OnEnvironmentChanged.ExecuteIfBound();
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void ClearObjects()
	{
		Objects.Empty();
		OnEnvironmentChanged.ExecuteIfBound();
	}

	FConvaiObjectEntry* FindCharacter(FString CharacterName)
	{
		for (FConvaiObjectEntry& c : Characters)
		{
			if (CharacterName == c.Name)
			{
				return &c;
			}
		}
		return nullptr;
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void AddCharacter(FConvaiObjectEntry Character)
	{
		// Replace old character that has the same name with the new character
		if (FConvaiObjectEntry* ExistingCharacter = FindCharacter(Character.Name))
		{
			ExistingCharacter->Description = Character.Description;
			ExistingCharacter->OptionalPositionVector = Character.OptionalPositionVector;
			ExistingCharacter->Ref = Character.Ref;
		}
		else
		{
			Characters.AddUnique(Character);
		}
	}

	/**
		*    Adds a list of characters to the Environment object
		*    @param Characters			A map of Characters following this format [Name : Bio]
		*/
	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void AddCharacters(TArray<FConvaiObjectEntry> CharactersToAdd)
	{
		for (auto c : CharactersToAdd)
			AddCharacter(c);
		OnEnvironmentChanged.ExecuteIfBound();
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void RemoveCharacter(FString CharacterName)
	{
		for (auto c : Characters)
			if (CharacterName == c.Name)
				Characters.Remove(c);
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void RemoveCharacters(TArray<FString> CharacterNamesToRemove)
	{
		for (auto n : CharacterNamesToRemove)
			RemoveCharacter(n);
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void ClearCharacters()
	{
		Characters.Empty();
		OnEnvironmentChanged.ExecuteIfBound();
	}

	// Assigns the main character initiating the conversation, typically the player character, unless the dialogue involves non-player characters talking to each other.
	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void SetMainCharacter(FConvaiObjectEntry InMainCharacter)
	{
		MainCharacter = InMainCharacter;
		AddCharacter(MainCharacter);
		OnEnvironmentChanged.ExecuteIfBound();
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void SetAttentionObject(FConvaiObjectEntry InAttentionObject)
	{
		AttentionObject = InAttentionObject;
		AddObject(AttentionObject);
		OnEnvironmentChanged.ExecuteIfBound();
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void ClearMainCharacter()
	{
		MainCharacter = FConvaiObjectEntry();
	}

	UFUNCTION(BlueprintCallable, category = "Convai|Action API")
		void ClearAttentionObject()
	{
		AttentionObject = FConvaiObjectEntry();
	}

	UPROPERTY(BlueprintReadOnly, category = "Convai|Action API")
		TArray<FString> Actions;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Action API")
		TArray<FConvaiObjectEntry> Objects;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Action API")
		TArray<FConvaiObjectEntry> Characters;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Action API")
		FConvaiObjectEntry MainCharacter;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Action API")
		FConvaiObjectEntry AttentionObject;
};

UCLASS(Blueprintable)
class UConvaiActionContext : public UConvaiEnvironment
{
	GENERATED_BODY()
public:
	UFUNCTION(Meta = (DeprecatedFunction, DeprecationMessage = "Use \"Create Convai Environment\" Instead"), BlueprintCallable, Category = "Convai|Action API|DEPRECATED")
		static UConvaiActionContext* CreateConvaiActionContext()
	{
		UConvaiActionContext* ContextObject = NewObject<UConvaiActionContext>();
		return ContextObject;
	}
};

namespace ConvaiConstants
{
	enum
	{
		// Buffer sizes
		VoiceCaptureRingBufferCapacity = 1024 * 1024,
		VoiceCaptureBufferSize = 1024 * 1024,
		LipSyncBufferSize = 1024 * 100, // aproximately 1024 seconds for OVR
		VoiceCaptureSampleRate = 48000,
		VoiceCaptureChunk = 2084,
		VoiceStreamMaxChunk = 4096,
		PlayerTimeOut = 2500 /* 2500 ms*/,
		ChatbotTimeOut = 6000 /* 6000 ms*/,
		WebRTCAudioSampleRate = 48000 /* 16 kHz */
	};
	
	// OVR Viseme names (15 visemes) - used for VisemeBased mode
	const TArray<FString> VisemeNames = { "sil", "PP", "FF", "TH", "DD", "kk", "CH", "SS", "nn", "RR", "aa", "E", "ih", "oh", "ou" };
	
	// ARKit / Apple blendshape names (52 blendshapes) - used for BS_ArKIT mode
	const TArray<FString> ARKitBlendShapesNames = { "EyeBlinkLeft", "EyeLookDownLeft", "EyeLookInLeft", "EyeLookOutLeft", "EyeLookUpLeft", "EyeSquintLeft", "EyeWideLeft", "EyeBlinkRight", "EyeLookDownRight", "EyeLookInRight", "EyeLookOutRight", "EyeLookUpRight", "EyeSquintRight", "EyeWideRight", "JawForward", "JawRight", "JawLeft", "JawOpen", "MouthClose", "MouthFunnel", "MouthPucker", "MouthRight", "MouthLeft", "MouthSmileLeft", "MouthSmileRight", "MouthFrownLeft", "MouthFrownRight", "MouthDimpleLeft", "MouthDimpleRight", "MouthStretchLeft", "MouthStretchRight", "MouthRollLower", "MouthRollUpper", "MouthShrugLower", "MouthShrugUpper", "MouthPressLeft", "MouthPressRight", "MouthLowerDownLeft", "MouthLowerDownRight", "MouthUpperUpLeft", "MouthUpperUpRight", "BrowDownLeft", "BrowDownRight", "BrowInnerUp", "BrowOuterUpLeft", "BrowOuterUpRight", "CheekPuff", "CheekSquintLeft", "CheekSquintRight", "NoseSneerLeft", "NoseSneerRight", "TongueOut", "HeadYaw", "HeadPitch", "HeadRoll", "LeftEyeYaw", "LeftEyePitch", "LeftEyeRoll", "RightEyeYaw", "RightEyePitch", "RightEyeRoll" };
	
	// MetaHuman CTRL curve names - used for BS_MHA mode
	const TArray<FString> MetaHumanCtrlNames = { TEXT("CTRL_expressions_browDownL"), TEXT("CTRL_expressions_browDownR"), TEXT("CTRL_expressions_browLateralL"), TEXT("CTRL_expressions_browLateralR"), TEXT("CTRL_expressions_browRaiseInL"), TEXT("CTRL_expressions_browRaiseInR"), TEXT("CTRL_expressions_browRaiseOuterL"), TEXT("CTRL_expressions_browRaiseOuterR"), TEXT("CTRL_expressions_earUpL"), TEXT("CTRL_expressions_earUpR"), TEXT("CTRL_expressions_eyeBlinkL"), TEXT("CTRL_expressions_eyeBlinkR"), TEXT("CTRL_expressions_eyeCheekRaiseL"), TEXT("CTRL_expressions_eyeCheekRaiseR"), TEXT("CTRL_expressions_eyeFaceScrunchL"), TEXT("CTRL_expressions_eyeFaceScrunchR"), TEXT("CTRL_expressions_eyeLidPressL"), TEXT("CTRL_expressions_eyeLidPressR"), TEXT("CTRL_expressions_eyeLookDownL"), TEXT("CTRL_expressions_eyeLookDownR"), TEXT("CTRL_expressions_eyeLookLeftL"), TEXT("CTRL_expressions_eyeLookLeftR"), TEXT("CTRL_expressions_eyeLookRightL"), TEXT("CTRL_expressions_eyeLookRightR"), TEXT("CTRL_expressions_eyeLookUpL"), TEXT("CTRL_expressions_eyeLookUpR"), TEXT("CTRL_expressions_eyeLowerLidDownL"), TEXT("CTRL_expressions_eyeLowerLidDownR"), TEXT("CTRL_expressions_eyeLowerLidUpL"), TEXT("CTRL_expressions_eyeLowerLidUpR"), TEXT("CTRL_expressions_eyeParallelLookDirection"), TEXT("CTRL_expressions_eyePupilNarrowL"), TEXT("CTRL_expressions_eyePupilNarrowR"), TEXT("CTRL_expressions_eyePupilWideL"), TEXT("CTRL_expressions_eyePupilWideR"), TEXT("CTRL_expressions_eyeRelaxL"), TEXT("CTRL_expressions_eyeRelaxR"), TEXT("CTRL_expressions_eyeSquintInnerL"), TEXT("CTRL_expressions_eyeSquintInnerR"), TEXT("CTRL_expressions_eyeUpperLidUpL"), TEXT("CTRL_expressions_eyeUpperLidUpR"), TEXT("CTRL_expressions_eyeWidenL"), TEXT("CTRL_expressions_eyeWidenR"), TEXT("CTRL_expressions_eyelashesDownINL"), TEXT("CTRL_expressions_eyelashesDownINR"), TEXT("CTRL_expressions_eyelashesDownOUTL"), TEXT("CTRL_expressions_eyelashesDownOUTR"), TEXT("CTRL_expressions_eyelashesUpINL"), TEXT("CTRL_expressions_eyelashesUpINR"), TEXT("CTRL_expressions_eyelashesUpOUTL"), TEXT("CTRL_expressions_eyelashesUpOUTR"), TEXT("CTRL_expressions_jawBack"), TEXT("CTRL_expressions_jawChinCompressL"), TEXT("CTRL_expressions_jawChinCompressR"), TEXT("CTRL_expressions_jawChinRaiseDL"), TEXT("CTRL_expressions_jawChinRaiseDR"), TEXT("CTRL_expressions_jawChinRaiseUL"), TEXT("CTRL_expressions_jawChinRaiseUR"), TEXT("CTRL_expressions_jawClenchL"), TEXT("CTRL_expressions_jawClenchR"), TEXT("CTRL_expressions_jawFwd"), TEXT("CTRL_expressions_jawLeft"), TEXT("CTRL_expressions_jawOpen"), TEXT("CTRL_expressions_jawOpenExtreme"), TEXT("CTRL_expressions_jawRight"), TEXT("CTRL_expressions_mouthCheekBlowL"), TEXT("CTRL_expressions_mouthCheekBlowR"), TEXT("CTRL_expressions_mouthCheekSuckL"), TEXT("CTRL_expressions_mouthCheekSuckR"), TEXT("CTRL_expressions_mouthCornerDepressL"), TEXT("CTRL_expressions_mouthCornerDepressR"), TEXT("CTRL_expressions_mouthCornerDownL"), TEXT("CTRL_expressions_mouthCornerDownR"), TEXT("CTRL_expressions_mouthCornerNarrowL"), TEXT("CTRL_expressions_mouthCornerNarrowR"), TEXT("CTRL_expressions_mouthCornerPullL"), TEXT("CTRL_expressions_mouthCornerPullR"), TEXT("CTRL_expressions_mouthCornerRounderDL"), TEXT("CTRL_expressions_mouthCornerRounderDR"), TEXT("CTRL_expressions_mouthCornerRounderUL"), TEXT("CTRL_expressions_mouthCornerRounderUR"), TEXT("CTRL_expressions_mouthCornerSharpenDL"), TEXT("CTRL_expressions_mouthCornerSharpenDR"), TEXT("CTRL_expressions_mouthCornerSharpenUL"), TEXT("CTRL_expressions_mouthCornerSharpenUR"), TEXT("CTRL_expressions_mouthCornerUpL"), TEXT("CTRL_expressions_mouthCornerUpR"), TEXT("CTRL_expressions_mouthCornerWideL"), TEXT("CTRL_expressions_mouthCornerWideR"), TEXT("CTRL_expressions_mouthDimpleL"), TEXT("CTRL_expressions_mouthDimpleR"), TEXT("CTRL_expressions_mouthDown"), TEXT("CTRL_expressions_mouthFunnelDL"), TEXT("CTRL_expressions_mouthFunnelDR"), TEXT("CTRL_expressions_mouthFunnelUL"), TEXT("CTRL_expressions_mouthFunnelUR"), TEXT("CTRL_expressions_mouthLeft"), TEXT("CTRL_expressions_mouthLipsBlowL"), TEXT("CTRL_expressions_mouthLipsBlowR"), TEXT("CTRL_expressions_mouthLipsPressL"), TEXT("CTRL_expressions_mouthLipsPressR"), TEXT("CTRL_expressions_mouthLipsPullDL"), TEXT("CTRL_expressions_mouthLipsPullDR"), TEXT("CTRL_expressions_mouthLipsPullUL"), TEXT("CTRL_expressions_mouthLipsPullUR"), TEXT("CTRL_expressions_mouthLipsPurseDL"), TEXT("CTRL_expressions_mouthLipsPurseDR"), TEXT("CTRL_expressions_mouthLipsPurseUL"), TEXT("CTRL_expressions_mouthLipsPurseUR"), TEXT("CTRL_expressions_mouthLipsPushDL"), TEXT("CTRL_expressions_mouthLipsPushDR"), TEXT("CTRL_expressions_mouthLipsPushUL"), TEXT("CTRL_expressions_mouthLipsPushUR"), TEXT("CTRL_expressions_mouthLipsStickyLPh1"), TEXT("CTRL_expressions_mouthLipsStickyLPh2"), TEXT("CTRL_expressions_mouthLipsStickyLPh3"), TEXT("CTRL_expressions_mouthLipsStickyRPh1"), TEXT("CTRL_expressions_mouthLipsStickyRPh2"), TEXT("CTRL_expressions_mouthLipsStickyRPh3"), TEXT("CTRL_expressions_mouthLipsThickDL"), TEXT("CTRL_expressions_mouthLipsThickDR"), TEXT("CTRL_expressions_mouthLipsThickInwardDL"), TEXT("CTRL_expressions_mouthLipsThickInwardDR"), TEXT("CTRL_expressions_mouthLipsThickInwardUL"), TEXT("CTRL_expressions_mouthLipsThickInwardUR"), TEXT("CTRL_expressions_mouthLipsThickUL"), TEXT("CTRL_expressions_mouthLipsThickUR"), TEXT("CTRL_expressions_mouthLipsThinDL"), TEXT("CTRL_expressions_mouthLipsThinDR"), TEXT("CTRL_expressions_mouthLipsThinInwardDL"), TEXT("CTRL_expressions_mouthLipsThinInwardDR"), TEXT("CTRL_expressions_mouthLipsThinInwardUL"), TEXT("CTRL_expressions_mouthLipsThinInwardUR"), TEXT("CTRL_expressions_mouthLipsThinUL"), TEXT("CTRL_expressions_mouthLipsThinUR"), TEXT("CTRL_expressions_mouthLipsTightenDL"), TEXT("CTRL_expressions_mouthLipsTightenDR"), TEXT("CTRL_expressions_mouthLipsTightenUL"), TEXT("CTRL_expressions_mouthLipsTightenUR"), TEXT("CTRL_expressions_mouthLipsTogetherDL"), TEXT("CTRL_expressions_mouthLipsTogetherDR"), TEXT("CTRL_expressions_mouthLipsTogetherUL"), TEXT("CTRL_expressions_mouthLipsTogetherUR"), TEXT("CTRL_expressions_mouthLipsTowardsDL"), TEXT("CTRL_expressions_mouthLipsTowardsDR"), TEXT("CTRL_expressions_mouthLipsTowardsUL"), TEXT("CTRL_expressions_mouthLipsTowardsUR"), TEXT("CTRL_expressions_mouthLowerLipBiteL"), TEXT("CTRL_expressions_mouthLowerLipBiteR"), TEXT("CTRL_expressions_mouthLowerLipDepressL"), TEXT("CTRL_expressions_mouthLowerLipDepressR"), TEXT("CTRL_expressions_mouthLowerLipRollInL"), TEXT("CTRL_expressions_mouthLowerLipRollInR"), TEXT("CTRL_expressions_mouthLowerLipRollOutL"), TEXT("CTRL_expressions_mouthLowerLipRollOutR"), TEXT("CTRL_expressions_mouthLowerLipShiftLeft"), TEXT("CTRL_expressions_mouthLowerLipShiftRight"), TEXT("CTRL_expressions_mouthLowerLipTowardsTeethL"), TEXT("CTRL_expressions_mouthLowerLipTowardsTeethR"), TEXT("CTRL_expressions_mouthPressDL"), TEXT("CTRL_expressions_mouthPressDR"), TEXT("CTRL_expressions_mouthPressUL"), TEXT("CTRL_expressions_mouthPressUR"), TEXT("CTRL_expressions_mouthRight"), TEXT("CTRL_expressions_mouthSharpCornerPullL"), TEXT("CTRL_expressions_mouthSharpCornerPullR"), TEXT("CTRL_expressions_mouthStickyDC"), TEXT("CTRL_expressions_mouthStickyDINL"), TEXT("CTRL_expressions_mouthStickyDINR"), TEXT("CTRL_expressions_mouthStickyDOUTL"), TEXT("CTRL_expressions_mouthStickyDOUTR"), TEXT("CTRL_expressions_mouthStickyUC"), TEXT("CTRL_expressions_mouthStickyUINL"), TEXT("CTRL_expressions_mouthStickyUINR"), TEXT("CTRL_expressions_mouthStickyUOUTL"), TEXT("CTRL_expressions_mouthStickyUOUTR"), TEXT("CTRL_expressions_mouthStretchL"), TEXT("CTRL_expressions_mouthStretchLipsCloseL"), TEXT("CTRL_expressions_mouthStretchLipsCloseR"), TEXT("CTRL_expressions_mouthStretchR"), TEXT("CTRL_expressions_mouthUp"), TEXT("CTRL_expressions_mouthUpperLipBiteL"), TEXT("CTRL_expressions_mouthUpperLipBiteR"), TEXT("CTRL_expressions_mouthUpperLipRaiseL"), TEXT("CTRL_expressions_mouthUpperLipRaiseR"), TEXT("CTRL_expressions_mouthUpperLipRollInL"), TEXT("CTRL_expressions_mouthUpperLipRollInR"), TEXT("CTRL_expressions_mouthUpperLipRollOutL"), TEXT("CTRL_expressions_mouthUpperLipRollOutR"), TEXT("CTRL_expressions_mouthUpperLipShiftLeft"), TEXT("CTRL_expressions_mouthUpperLipShiftRight"), TEXT("CTRL_expressions_mouthUpperLipTowardsTeethL"), TEXT("CTRL_expressions_mouthUpperLipTowardsTeethR"), TEXT("CTRL_expressions_neckDigastricDown"), TEXT("CTRL_expressions_neckDigastricUp"), TEXT("CTRL_expressions_neckMastoidContractL"), TEXT("CTRL_expressions_neckMastoidContractR"), TEXT("CTRL_expressions_neckStretchL"), TEXT("CTRL_expressions_neckStretchR"), TEXT("CTRL_expressions_neckSwallowPh1"), TEXT("CTRL_expressions_neckSwallowPh2"), TEXT("CTRL_expressions_neckSwallowPh3"), TEXT("CTRL_expressions_neckSwallowPh4"), TEXT("CTRL_expressions_neckThroatDown"), TEXT("CTRL_expressions_neckThroatExhale"), TEXT("CTRL_expressions_neckThroatInhale"), TEXT("CTRL_expressions_neckThroatUp"), TEXT("CTRL_expressions_noseNasolabialDeepenL"), TEXT("CTRL_expressions_noseNasolabialDeepenR"), TEXT("CTRL_expressions_noseNostrilCompressL"), TEXT("CTRL_expressions_noseNostrilCompressR"), TEXT("CTRL_expressions_noseNostrilDepressL"), TEXT("CTRL_expressions_noseNostrilDepressR"), TEXT("CTRL_expressions_noseNostrilDilateL"), TEXT("CTRL_expressions_noseNostrilDilateR"), TEXT("CTRL_expressions_noseWrinkleL"), TEXT("CTRL_expressions_noseWrinkleR"), TEXT("CTRL_expressions_noseWrinkleUpperL"), TEXT("CTRL_expressions_noseWrinkleUpperR"), TEXT("CTRL_expressions_teethBackD"), TEXT("CTRL_expressions_teethBackU"), TEXT("CTRL_expressions_teethDownD"), TEXT("CTRL_expressions_teethDownU"), TEXT("CTRL_expressions_teethFwdD"), TEXT("CTRL_expressions_teethFwdU"), TEXT("CTRL_expressions_teethLeftD"), TEXT("CTRL_expressions_teethLeftU"), TEXT("CTRL_expressions_teethRightD"), TEXT("CTRL_expressions_teethRightU"), TEXT("CTRL_expressions_teethUpD"), TEXT("CTRL_expressions_teethUpU"), TEXT("CTRL_expressions_tongueBendDown"), TEXT("CTRL_expressions_tongueBendUp"), TEXT("CTRL_expressions_tongueDown"), TEXT("CTRL_expressions_tongueIn"), TEXT("CTRL_expressions_tongueLeft"), TEXT("CTRL_expressions_tongueNarrow"), TEXT("CTRL_expressions_tongueOut"), TEXT("CTRL_expressions_tonguePress"), TEXT("CTRL_expressions_tongueRight"), TEXT("CTRL_expressions_tongueRoll"), TEXT("CTRL_expressions_tongueThick"), TEXT("CTRL_expressions_tongueThin"), TEXT("CTRL_expressions_tongueTipDown"), TEXT("CTRL_expressions_tongueTipLeft"), TEXT("CTRL_expressions_tongueTipRight"), TEXT("CTRL_expressions_tongueTipUp"), TEXT("CTRL_expressions_tongueTwistLeft"), TEXT("CTRL_expressions_tongueTwistRight"), TEXT("CTRL_expressions_tongueUp"), TEXT("CTRL_expressions_tongueWide") };
	
	// CC4 Extended curve names - used for BS_CC4_Extended mode
	const TArray<FString> CC4ExtendedNames = { "Mouth_Drop_Lower", "Mouth_Up_Upper_L", "Mouth_Up_Upper_R", "Mouth_Contract", "Tongue_Out", "Tongue_In", "Tongue_Up", "Tongue_Down", "Tongue_Mid_Up", "Tongue_Tip_Up", "Tongue_Tip_Down", "Tongue_Narrow", "Tongue_Wide", "Tongue_Roll", "Tongue_L", "Tongue_R", "Tongue_Tip_L", "Tongue_Tip_R", "Tongue_Twist_L", "Tongue_Twist_R", "Tongue_Bulge_L", "Tongue_Bulge_R", "Tongue_Extend", "Tongue_Enlarge", "Jaw_Forward", "Jaw_L", "Jaw_R", "Jaw_Up", "Jaw_Down", "Head_Turn_Up", "Head_Turn_Down", "Head_Tilt_L", "Head_L", "Head_Backward", "Brow_Raise_Inner_L", "Brow_Raise_Inner_R", "Brow_Raise_Outer_L", "Brow_Raise_Outer_R", "Brow_Drop_L", "Brow_Drop_R", "Brow_Compress_L", "Brow_Compress_R", "Eye_Blink_L", "Eye_Blink_R", "Eye_Squint_L", "Eye_Squint_R", "Eye_Wide_L", "Eye_Wide_R", "Eye_L_Look_L", "Eye_R_Look_L", "Eye_L_Look_R", "Eye_R_Look_R", "Eye_L_Look_Up", "Eye_R_Look_Up", "Eye_L_Look_Down", "Eye_R_Look_Down", "Eyelash_Upper_Up_L", "Eyelash_Upper_Down_L", "Eyelash_Upper_Up_R", "Eyelash_Upper_Down_R", "Eyelash_Lower_Up_L", "Eyelash_Lower_Down_L", "Eyelash_Lower_Up_R", "Eyelash_Lower_Down_R", "Ear_Up_L", "Ear_Up_R", "Ear_Down_L", "Ear_Down_R", "Ear_Out_L", "Ear_Out_R", "Nose_Sneer_L", "Nose_Sneer_R", "Nose_Nostril_Raise_L", "Nose_Nostril_Raise_R", "Nose_Nostril_Dilate_L", "Nose_Nostril_Dilate_R", "Nose_Crease_L", "Nose_Crease_R", "Nose_Nostril_Down_L", "Nose_Nostril_Down_R", "Nose_Nostril_In_L", "Nose_Nostril_In_R", "Nose_Tip_L", "Nose_Tip_R", "Nose_Tip_Up", "Nose_Tip_Down", "Cheek_Raise_L", "Cheek_Raise_R", "Cheek_Suck_L", "Cheek_Suck_R", "Cheek_Puff_L", "Cheek_Puff_R", "Mouth_Smile_L", "Mouth_Smile_R", "Mouth_Smile_Sharp_L", "Mouth_Smile_Sharp_R", "Mouth_Frown_L", "Mouth_Frown_R", "Mouth_Stretch_L", "Mouth_Stretch_R", "Mouth_Dimple_L", "Mouth_Dimple_R", "Mouth_Press_L", "Mouth_Press_R", "Mouth_Tighten_L", "Mouth_Tighten_R", "Mouth_Blow_L", "Mouth_Blow_R", "Mouth_Pucker_Up_L", "Mouth_Pucker_Up_R", "Mouth_Pucker_Down_L", "Mouth_Pucker_Down_R", "Mouth_Funnel_Up_L", "Mouth_Funnel_Up_R", "Mouth_Funnel_Down_L", "Mouth_Funnel_Down_R", "Mouth_Roll_In_Upper_L", "Mouth_Roll_In_Upper_R", "Mouth_Roll_In_Lower_L", "Mouth_Roll_In_Lower_R", "Mouth_Roll_Out_Upper_L", "Mouth_Roll_Out_Upper_R", "Mouth_Roll_Out_Lower_L", "Mouth_Roll_Out_Lower_R", "Mouth_Push_Upper_L", "Mouth_Push_Upper_R", "Mouth_Push_Lower_L", "Mouth_Push_Lower_R", "Mouth_Pull_Upper_L", "Mouth_Pull_Upper_R", "Mouth_Pull_Lower_L", "Mouth_Pull_Lower_R", "Mouth_Up", "Mouth_Down", "Mouth_L", "Mouth_R", "Mouth_Upper_L", "Mouth_Upper_R", "Mouth_Lower_L", "Mouth_Lower_R", "Mouth_Shrug_Upper", "Mouth_Shrug_Lower", "Mouth_Drop_Upper", "Mouth_Down_Lower_L", "Mouth_Down_Lower_R", "Mouth_Chin_Up", "Mouth_Close", "Jaw_Open", "Jaw_Backward", "Neck_Swallow_Up", "Neck_Swallow_Down", "Neck_Tighten_L", "Neck_Tighten_R", "Head_Turn_L", "Head_Turn_R", "Head_Tilt_R", "Head_R", "Head_Forward", "eye_shape_L", "eye_shape_R", "eye_shape_angry_L", "eye_shape_angry_R", "double_eyelid_up_L", "double_eyelid_up_R", "lips_smooth_lower", "lips_curve_shape_lower", "eyes_smile_shape_L", "eyes_smile_shape_R", "smile_coner_shape_L", "smile_coner_shape_R" };
	
	const FString API_Key_Header = "CONVAI-API-KEY";
	const FString Auth_Token_Header = "API-AUTH-TOKEN";
	const FString X_API_KEY_HEADER = "X-API-KEY";
};


template<typename DelegateType>
class FThreadSafeDelegateWrapper
{
public:
	// Bind a delegate
	void Bind(const DelegateType& InDelegate)
	{
		FScopeLock Lock(&Mutex);
		MyDelegate = InDelegate;
	}

	// Mirror of BindUObject function for non-const UserClass
	template <typename UserClass, typename... VarTypes>
	void BindUObject(UserClass* InUserObject, void(UserClass::* InFunc)(VarTypes...))
	{
		FScopeLock Lock(&Mutex);
		MyDelegate.BindUObject(InUserObject, InFunc);
	}

	// Unbind the delegate
	void Unbind()
	{
		FScopeLock Lock(&Mutex);
		MyDelegate.Unbind();
	}

	// Check if the delegate is bound
	bool IsBound() const
	{
		return MyDelegate.IsBound();
	}

	// Execute the delegate if it is bound
	// Use perfect forwarding to forward arguments to the delegate
	template<typename... ArgTypes>
	void ExecuteIfBound(ArgTypes&&... Args) const
	{
		FScopeLock Lock(&Mutex);
		if (MyDelegate.IsBound() && !IsEngineExitRequested())
		{
			MyDelegate.ExecuteIfBound(Forward<ArgTypes>(Args)...);
		}
	}

private:
	mutable FCriticalSection Mutex;
	DelegateType MyDelegate;
};

// Enum for Voice Types
UENUM(BlueprintType)
enum class EVoiceType : uint8
{
	AzureVoices           UMETA(DisplayName = "Azure Voices"),
	ElevenLabsVoices      UMETA(DisplayName = "ElevenLabs Voices"),
	GCPVoices             UMETA(DisplayName = "GCP Voices"),
	ConvaiVoices          UMETA(DisplayName = "Convai Voices"),
	OpenAIVoices          UMETA(DisplayName = "OpenAI Voices"),
	ConvaiVoicesNew       UMETA(DisplayName = "Convai Voices (New)"),
	ConvaiVoicesExperimental UMETA(DisplayName = "Convai Voices (Experimental)")
};

// Enum for Languages
UENUM(BlueprintType)
enum class ELanguageType : uint8
{
	Arabic               UMETA(DisplayName = "Arabic"),
	ChineseCantonese     UMETA(DisplayName = "Chinese (Cantonese)"),
	ChineseMandarin      UMETA(DisplayName = "Chinese (Mandarin)"),
	Dutch                UMETA(DisplayName = "Dutch"),
	DutchBelgium         UMETA(DisplayName = "Dutch (Belgium)"),
	English              UMETA(DisplayName = "English"),
	Finnish              UMETA(DisplayName = "Finnish"),
	French               UMETA(DisplayName = "French"),
	German               UMETA(DisplayName = "German"),
	Hindi                UMETA(DisplayName = "Hindi"),
	Italian              UMETA(DisplayName = "Italian"),
	Japanese             UMETA(DisplayName = "Japanese"),
	Korean               UMETA(DisplayName = "Korean"),
	Polish               UMETA(DisplayName = "Polish"),
	PortugueseBrazil     UMETA(DisplayName = "Portuguese (Brazil)"),
	PortuguesePortugal   UMETA(DisplayName = "Portuguese (Portugal)"),
	Russian              UMETA(DisplayName = "Russian"),
	Spanish              UMETA(DisplayName = "Spanish"),
	SpanishMexico        UMETA(DisplayName = "Spanish (Mexico)"),
	SpanishUS            UMETA(DisplayName = "Spanish (US)"),
	Swedish              UMETA(DisplayName = "Swedish"),
	Turkish              UMETA(DisplayName = "Turkish"),
	Vietnamese           UMETA(DisplayName = "Vietnamese")
};

// Enum for Gender
UENUM(BlueprintType)
enum class EGenderType : uint8
{
	Male    UMETA(DisplayName = "Male"),
	Female  UMETA(DisplayName = "Female")
};


USTRUCT(BlueprintType)
struct FVoiceLanguageStruct
{
	GENERATED_BODY()
	
	//UPROPERTY(BlueprintReadOnly)
	FString VoiceType;

	//UPROPERTY(BlueprintReadOnly)
	FString VoiceName;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Language")
	FString VoiceValue;

	UPROPERTY(BlueprintReadOnly, category = "Convai|Language")
	TArray<FString> LangCodes;

	//UPROPERTY(BlueprintReadOnly)
	FString Gender;

	FVoiceLanguageStruct() {}
};


// LTM
USTRUCT(BlueprintType)
struct FConvaiSpeakerInfo
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speaker Info")
	FString SpeakerID;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speaker Info")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speaker Info")
	FString DeviceID;
	
	FConvaiSpeakerInfo()
		: SpeakerID(TEXT(""))
		, Name(TEXT(""))
		, DeviceID(TEXT(""))
	{
	}
};

UENUM(BlueprintType)
enum class EC_ConnectionState : uint8
{
	Disconnected UMETA(DisplayName = "Disconnected"),
	Connecting   UMETA(DisplayName = "Connecting"),
	Connected    UMETA(DisplayName = "Connected"),
	Reconnecting UMETA(DisplayName = "Reconnecting"),
};

// Forward declaration
namespace convai {
	class ConvaiClient;
}

USTRUCT()
struct CONVAI_API FConvaiConnectionParams
{
	GENERATED_BODY()

public:
	/** ConvaiClient pointer for the connection */
	convai::ConvaiClient* Client;

	/** Character ID for the connection */
	FString CharacterID;

	/** LLM provider for the connection */
	FString LLMProvider;

	/** Connection type for the connection */
	FString ConnectionType;

	/** Blendshape provider (e.g., "neurosync", "ovr", "not_provided") */
	FString BlendshapeProvider;

	/** Blendshape format (e.g., "mha" for MetaHuman, "arkit" for ARKit) */
	FString BlendshapeFormat;

	/** End User ID for long term memory (LTM) */
	FString EndUserID;

	/** End User Metadata as a JSON string for long term memory (LTM) */
	FString EndUserMetadata;

	int32 ChunkSize;

	int32 OutputFPS;

	float FramesBufferDuration;

	FConvaiConnectionParams()
		: Client(nullptr)
		, CharacterID(TEXT(""))
		, LLMProvider(TEXT("dynamic"))
		, ConnectionType(TEXT("audio"))
		, BlendshapeProvider(TEXT("not_provided"))
		, BlendshapeFormat(TEXT(""))
		, EndUserID(TEXT(""))
		, EndUserMetadata(TEXT(""))
		, ChunkSize(10)
		, OutputFPS(90)
		, FramesBufferDuration(0.0f)
	{
	}

	FConvaiConnectionParams(convai::ConvaiClient* InClient, const FString& InCharacterID, 
		const FString& InLLMProvider = TEXT("dynamic"), const FString& InConnectionType = TEXT("audio"),
		const FString& InBlendshapeProvider = TEXT("not_provided"), const FString& InBlendshapeFormat = TEXT(""),
		const FString& InEndUserID = TEXT(""), const FString& InEndUserMetadata = TEXT(""),
		int32 InChunkSize = 10, int32 InOutputFPS = 90, float InFramesBufferDuration = 0.0f)
		: Client(InClient)
		, CharacterID(InCharacterID)
		, LLMProvider(InLLMProvider)
		, ConnectionType(InConnectionType)
		, BlendshapeProvider(InBlendshapeProvider)
		, BlendshapeFormat(InBlendshapeFormat)
		, EndUserID(InEndUserID)
		, EndUserMetadata(InEndUserMetadata)
		, ChunkSize(InChunkSize)
		, OutputFPS(InOutputFPS)
		, FramesBufferDuration(InFramesBufferDuration)
	{
	}

	/**
	 * Creates connection parameters by determining the appropriate settings
	 * @param InClient - The ConvaiClient instance
	 * @param InCharacterID - The character ID to connect to
	 * @param SessionProxy - The session proxy to determine settings from
	 * @return Configured connection parameters
	 */
	static FConvaiConnectionParams Create(convai::ConvaiClient* InClient, const FString& InCharacterID, class UConvaiConnectionSessionProxy* SessionProxy);
};

UENUM(BlueprintType)
enum class EC_LipSyncMode : uint8
{
	Off				UMETA(DisplayName = "Off"),
	Auto            UMETA(DisplayName = "Auto"),
	VisemeBased     UMETA(DisplayName = "Viseme Based"),
	BS_MHA			UMETA(DisplayName = "MetaHuman Blendshapes"),
	BS_ARKit		UMETA(DisplayName = "ARKit Blendshapes"),
	BS_CC4_Extended	UMETA(DisplayName = "CC4 Extended Blendshapes")
};
