// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiConversationComponent.h"
#include "ConvaiDefinitions.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiChatbotComponent.generated.h"

// Forward declarations
class UConvaiPlayerComponent;
class USoundWaveProcedural;
class UConvaiGRPCGetResponseProxy;
class UConvaiConnectionSessionProxy;
class UConvaiChatBotGetDetailsProxy;
class UConvaiEnvironment;

DECLARE_LOG_CATEGORY_EXTERN(ConvaiChatbotComponentLog, Log, All);

// New V2 delegates with component references
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_ThreeParams(FOnActionReceivedSignature_V2, UConvaiChatbotComponent, OnActionReceivedEvent_V2, UConvaiChatbotComponent*, ChatbotComponent, UConvaiPlayerComponent*, InteractingPlayerComponent, const TArray<FConvaiResultAction>&, SequenceOfActions);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_TwoParams(FOnCharacterDataLoadSignature_V2, UConvaiChatbotComponent, OnCharacterDataLoadEvent_V2, UConvaiChatbotComponent*, ChatbotComponent, bool, Success); 
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_TwoParams(FOnNarrativeSectionReceivedSignature, UConvaiChatbotComponent, OnNarrativeSectionReceivedEvent, UConvaiChatbotComponent*, ChatbotComponent, FString, NarrativeSectionID);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_TwoParams(FOnEmotionReceivedSignature, UConvaiChatbotComponent, OnEmotionStateChangedEvent, UConvaiChatbotComponent*, ChatbotComponent, UConvaiPlayerComponent*, InteractingPlayerComponent);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_ThreeParams(FOnInteractionIDReceivedSignature, UConvaiChatbotComponent, OnInteractionIDReceivedEvent, UConvaiChatbotComponent*, ChatbotComponent, UConvaiPlayerComponent*, InteractingPlayerComponent, FString, InteractionID);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE(FOnFailureSignature, UConvaiChatbotComponent, OnFailureEvent);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_TwoParams(FOnInterruptedSignature, UConvaiChatbotComponent, OnInterruptedEvent, UConvaiChatbotComponent*, ChatbotComponent, UConvaiPlayerComponent*, InteractingPlayerComponent);

UCLASS(Blueprintable, BlueprintType, meta = (BlueprintSpawnableComponent), DisplayName = "Convai Chatbot")
class CONVAI_API UConvaiChatbotComponent : public UConvaiConversationComponent, public IConvaiConnectionInterface
{
	GENERATED_BODY()
public:
	UConvaiChatbotComponent();

	void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;


	// Override from UConvaiConversationComponent
	virtual bool IsPlayer() const override { return false; }
	virtual FString GetConversationalName() const override { return CharacterName; }

	/**
	* Returns true, if the character is being talked to, is talking, or is processing the response.
	*/
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai")
	bool IsInConversation();

	/**
	* Returns true, if the character is still processing and has not received the full response yet.
	*/
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai", meta = (DisplayName = "Is Thinking"))
	bool IsProcessing();

	/**
	* Returns true, if the character is currently listening to a player.
	*/
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai")
	bool IsListening();

	/**
	* Returns true, if the character is currently talking.
	*/
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai", meta = (DisplayName = "Is Talking"))
	bool GetIsTalking();

	/** Returns time elapsed since the character started talking */
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Voice")
	float GetTalkingTimeElapsed();

	/** Returns time remaining audio time for the character to speak */
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Voice")
	float GetTalkingTimeRemaining();

	UPROPERTY(EditAnywhere, Category = "Convai", Replicated, BlueprintSetter = LoadCharacter)
	FString CharacterID;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString CharacterName;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString VoiceType;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString Backstory;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString LanguageCode;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString ReadyPlayerMeLink;

	UPROPERTY(BlueprintReadOnly, Category = "Convai", Replicated)
	FString AvatarImageLink;

	UPROPERTY(BlueprintReadOnly, Category = "Convai|Actions", Replicated)
	TArray<FConvaiResultAction> ActionsQueue;

	UPROPERTY(Replicated)
	FConvaiEmotionState EmotionState;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai|Emotion", Replicated)
	bool LockEmotionState = false;

	/**
	 *    Used to track memory of a previous conversation, set to -1 means no previous conversation,
	 *	  this property will change as you talk to the character, you can save the session ID for a
	 *    conversation and then set it back later on to resume a conversation
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Convai", Replicated)
	FString SessionID = "-1";

	/**
	 *    Contains all relevant objects and characters in the scene including the (Player), and also all the actions doable by the character
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Convai", BlueprintSetter = LoadEnvironment)
	UConvaiEnvironment* Environment;

	/**
	 *    Time in seconds, for the character's voice audio to gradually degrade until it is completely turned off when interrupted.
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
	float InterruptVoiceFadeOutDuration;

	/**
	 *    Value between -1 and 1, a value greater than zero over extends the emotions strength and a value less than zero dimishes it.
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai")
	float EmotionOffset = 0;

	/**
	 *    Contains key value pairs used for Narrative Design.
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai|NarrativeDesign", BlueprintSetter = UpdateNarrativeTemplateKeys)
	TMap<FString, FString> NarrativeTemplateKeys;

	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void UpdateNarrativeTemplateKeys(TMap<FString, FString> InNarrativeTemplateKeys);
	
	/**
	 *   Extra information that can be passed to the character, can contain any important data that the chracter needs to know about without the 
	 *   need of player interaction or narrative triggers, e.g. Inventory items, Player health, key information to solve a puzzle, time of day, etc...
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Convai", BlueprintSetter = UpdateDynamicEnvironmentInfo)
	FString DynamicEnvironmentInfo;

	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void UpdateDynamicEnvironmentInfo(FString InDynamicEnvironmentInfo);
	
	/**
	 *   Speaker ID used for long term memory (LTM)
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Convai")
	FString SpeakerID;

	/**
	 *    Reset the conversation with the character and remove previous memory, this is the same as setting the session ID property to -1.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai")
	void ResetConversation();	
	
	/**
	 *    Loads a new character using its ID
	 */
	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void LoadCharacter(FString NewCharacterID);

	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly, Category = "Convai")
	void LoadEnvironment(UConvaiEnvironment* NewConvaiEnvironment);

	/**
	 * Appends a TArray of FConvaiResultAction items to the existing ActionsQueue.
	 * If ActionsQueue is not empty, it takes the first element, appends the new array to it,
	 * and then reassigns it back to ActionsQueue. If ActionsQueue is empty, it simply sets ActionsQueue to the new array.
	 *
	 * @param NewActions Array of FConvaiResultAction items to be appended.
	 *
	 * @category Convai
	 */
	 void AppendActionsToQueue(TArray<FConvaiResultAction> NewActions);

	/**
	 * Marks the current action as completed and handles post-execution logic.
	 *
	 * @param IsSuccessful A boolean flag indicating whether the executed action was successful or not. If true, the next action in the queue will be processed. If false, the current action will be retried.
	 * @param Delay A float value representing the time in seconds to wait before attempting either the next action or retrying the current action, depending on the value of IsSuccessful.
	 *
	 * @note This function should be invoked after each action execution to manage the action queue.
	 *
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions")
	void HandleActionCompletion(bool IsSuccessful, float Delay);

	/**
	 * Checks if the ActionsQueue managed by the Convai chatbot component is empty.
	 *
	 * @return Returns true if the ActionsQueue is empty; otherwise, returns false.
	 *
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Actions")
	bool IsActionsQueueEmpty();

	/**
	 * Clears the ActionsQueue managed by the Convai chatbot component.
	 *
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Actions")
	void ClearActionQueue();

	/**
	 * Fetches the first action from the ActionsQueue managed by the Convai chatbot component.
	 *
	 * @param ConvaiResultAction Reference to a struct that will be populated with the details of the first action in the queue.
	 *
	 * @return Returns true if there is at least one action in the ActionsQueue and the struct has been successfully populated; otherwise, returns false.
	 *
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Actions")
	bool FetchFirstAction(FConvaiResultAction& ConvaiResultAction);

	/**
	 * Removes the first action from the ActionsQueue managed by the Convai chatbot component.
	 *
	 * @return Returns true if an action was successfully removed; otherwise, returns false.
	 *
	 */
	bool DequeueAction();

	/**
	 * Starts executing the first action in the ActionsQueue by calling TriggerNamedBlueprintAction.
	 *
	 * @return Returns true if the first action was successfully started; otherwise, returns false.
	 *
	 */
	UFUNCTION()
	bool StartFirstAction();
	
	/**
	 * Triggers a specified Blueprint event or function on the owning actor based on the given action name and parameters.
	 *
	 * @param ActionName The name of the Blueprint event or function to trigger. This event or function should exist in the Blueprint that owns this component.
	 * @param ConvaiActionStruct A struct containing additional data or parameters to pass to the Blueprint event or function.
	 *
	 * @note The function attempts to dynamically find and call a Blueprint event or function in the owning actor's class. If the Blueprint event or function does not exist or if the signature doesn't match, the function will log a warning.
	 *
	 */
	bool TriggerNamedBlueprintAction(const FString& ActionName, FConvaiResultAction ConvaiActionStruct);
	bool TryCallFunction(UObject* Object, const FString& FunctionName, FConvaiResultAction& ConvaiResultAction) const;
	
	UFUNCTION(BlueprintCallable, Category = "Convai|Emotion")
	void ForceSetEmotion(EBasicEmotions BasicEmotion, EEmotionIntensity Intensity, bool ResetOtherEmotions = false);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Emotion")
	float GetEmotionScore(EBasicEmotions Emotion);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Emotion")
	TMap<FName, float> GetEmotionBlendshapes();

	UFUNCTION(BlueprintCallable, Category = "Convai|Emotion")
	void ResetEmotionState();

public:
	// UFUNCTION(BlueprintCallable, Category = "Convai|Voice")
	void StartRecordingVoice();

	// UFUNCTION(BlueprintCallable, Category = "Convai|Voice")
	USoundWave* FinishRecordingVoice();

	// UFUNCTION(BlueprintCallable, Category = "Convai|Voice")
	bool PlayRecordedVoice(USoundWave* RecordedVoice);

public:
	/** Called when a new action is received from the API */
	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Actions Received"))
	FOnActionReceivedSignature_V2 OnActionReceivedEvent_V2;
	
	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Emotion State Changed"))
	FOnEmotionReceivedSignature OnEmotionStateChangedEvent;


	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Character Data Loaded"))
	FOnCharacterDataLoadSignature_V2 OnCharacterDataLoadEvent_V2;

	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Narrative Section Received"))
	FOnNarrativeSectionReceivedSignature OnNarrativeSectionReceivedEvent;

	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Interaction ID Received"))
	FOnInteractionIDReceivedSignature OnInteractionIDReceivedEvent;

	/** Called when the character is interrupted */
	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Interrupted"))
	FOnInterruptedSignature OnInterruptedEvent;

	/** Called when there is an error */
	UPROPERTY(BlueprintAssignable, Category = "Convai", meta = (DisplayName = "On Failure"))
	FOnFailureSignature OnFailureEvent;	

public:
	UFUNCTION(BlueprintCallable, Category = "Convai", meta = (DisplayName = "Invoke Speech"))
	void ExecuteNarrativeTrigger(FString TriggerMessage, UConvaiEnvironment* InEnvironment, bool InGenerateActions, bool InVoiceResponse, bool InReplicateOnNetwork);
	
	UFUNCTION(BlueprintCallable, Category = "Convai", meta = (DisplayName = "Invoke Narrative Design Trigger"))
	void InvokeNarrativeDesignTrigger(FString TriggerName, UConvaiEnvironment* InEnvironment, bool InGenerateActions, bool InVoiceResponse, bool InReplicateOnNetwork);

	void InvokeTrigger_Internal(const FString& TriggerName, const FString& TriggerMessage, UConvaiEnvironment* InEnvironment, bool InGenerateActions, bool InVoiceResponse, bool InReplicateOnNetwork);

	// Interrupts the current speech with a provided fade-out duration. 
	// The fade-out duration is controlled by the parameter 'InVoiceFadeOutDuration'.
	UFUNCTION(BlueprintCallable, Category = "Convai")
	void InterruptSpeech(float InVoiceFadeOutDuration);

	// Broadcasts an interruption of the current speech across a network, with a provided fade-out duration.
	// This function ensures that the interruption is communicated reliably to all connected clients.
	// The fade-out duration is controlled by the parameter 'InVoiceFadeOutDuration'.
	UFUNCTION(NetMulticast, Reliable, Category = "VoiceNetworking")
	void Broadcast_InterruptSpeech(float InVoiceFadeOutDuration);

public:
	// AActorComponent interface
	virtual void BeginPlay() override;
	//virtual void OnRegister() override;
	//virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	// End AActorComponent interface

	//~ Begin UObject Interface.
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
	//~ End UObject Interface.

	//~ Begin UConvaiAudioStreamer Interface.
	virtual bool CanUseLipSync() override;
	virtual bool CanUseVision() override;
	virtual void onAudioFinished() override;
	//~ End UConvaiAudioStreamer Interface.

	/**
	 * Force play any buffered audio immediately.
	 * Used when no more audio is expected but we have buffered data.
	 */
	void ForcePlayBufferedAudio();

private:
	UConvaiChatBotGetDetailsProxy* ConvaiGetDetails();

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
	FScriptDelegate ConvaiChatBotGetDetailsDelegate;
#else
	TScriptDelegate<FWeakObjectPtr> ConvaiChatBotGetDetailsDelegate;
#endif

	UFUNCTION()
	void OnConvaiGetDetailsCompleted(FString ReceivedCharacterName, FString ReceivedVoiceType, FString ReceivedBackstory, FString ReceivedLanguageCode, bool HasReadyPlayerMeLink, FString ReceivedReadyPlayerMeLink, FString ReceivedAvatarImageLink);

private:
	UPROPERTY(Replicated, ReplicatedUsing = OnRep_EnvironmentData)
	FConvaiEnvironmentDetails ConvaiEnvironmentDetails;

	UFUNCTION()
	void OnRep_EnvironmentData();

	void UpdateEnvironmentData();

private:
	UPROPERTY()
	UConvaiChatBotGetDetailsProxy* ConvaiChatBotGetDetailsProxy;

	TMap<FName, float> EmotionBlendshapes;
	TArray<uint8> RecordedAudio;
	uint32 RecordedAudioSampleRate;
	bool IsRecordingAudio;

	/** The session proxy instance */
	UPROPERTY()
	UConvaiConnectionSessionProxy* SessionProxyInstance;

public:
	/** Whether to automatically initialize the session in BeginPlay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Session")
	bool bAutoInitializeSession;

	/**
	 * Initializes the session for this chatbot component
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Session")
	void StartSession();

	/**
	 * Shuts down the session for this chatbot component
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Session")
	void StopSession();

private:	
	// IConvaiConnectionInterface implementation
	virtual UConvaiEnvironment* GetConvaiEnvironment() override{ return Environment; }
	virtual bool IsVisionSupported() override { return SupportsVision(); }
	virtual EC_LipSyncMode GetLipSyncMode() override;
	virtual void OnConnectedToServer() override;
	virtual void OnDisconnectedFromServer() override;
	virtual void OnAttendeeConnected(FString AttendeeId) override;
	virtual void OnAttendeeDisconnected(FString AttendeeId) override;
	virtual void OnTranscriptionReceived(FString Transcription, bool IsTranscriptionReady, bool IsFinal) override;
	virtual void OnAudioDataReceived(const int16_t* AudioData, size_t NumFrames, uint32_t SampleRate, uint32_t BitsPerSample, uint32_t NumChannels) override;
	virtual void OnStartedTalking() override;
	virtual void OnFinishedTalking() override;
	virtual void OnFaceDataReceived(FAnimationSequence FaceDataAnimation) override;
	virtual void OnSessionIDReceived(FString ReceivedSessionID) override;
	virtual void OnInteractionIDReceived(FString ReceivedInteractionID) override;
	virtual void OnActionSequenceReceived(const TArray<FConvaiResultAction>& ReceivedSequenceOfActions) override;
	virtual void OnEmotionReceived(FString ReceivedEmotionResponse, FAnimationFrame EmotionBlendshapesFrame, bool MultipleEmotions) override;
	virtual void OnNarrativeSectionReceived(FString BT_Code, FString BT_Constants, FString ReceivedNarrativeSectionID) override;
	virtual void OnFailure(FString Message) override;

	FThreadSafeBool IsConnectionTalking = false;

	void SendImage(const float& DeltaTime);
	
private:	
	/** Timestamp when IsConnectionTalking last became false */
	double FinishedTalkingTimestamp = -1.0;
		
	/** Delay in seconds before checking audio content after talking stops */
	static constexpr double AudioContentCheckDelay = 0.900f;

	// Accumulates DeltaTime so we can throttle to ConvaiVision's FPS.
	float TimeSinceLastVideoSend = 0.f;

	// Cached target interval between frames (in seconds). Recomputed if FPS changes.
	float TargetFrameInterval = 1.f / 15.f; // sensible default 15 FPS

	// Cache the last seen FPS to avoid recomputing every tick
	int32 CachedVisionFPS = 15;
};
