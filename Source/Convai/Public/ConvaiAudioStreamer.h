// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once
//#include "CoreMinimal.h"
// #undef UpdateResource
#include "Components/AudioComponent.h"
#include "ConvaiDefinitions.h"
#include "ConvaiThreadSafeBuffers.h"
#include "Misc/ScopeLock.h"
#include "Interfaces/VoiceCodec.h"

#include "CoreTypes.h"
#include "Templates/UnrealTemplate.h"
#include "HAL/PlatformAtomics.h"
#include "HAL/PlatformMisc.h"

#include "ConvaiAudioStreamer.generated.h"

#define NUM_ENTROPY_VALUES 5

DECLARE_LOG_CATEGORY_EXTERN(ConvaiAudioStreamerLog, Log, All);

class USoundWaveProcedural;
class IConvaiLipSyncInterface;
class IConvaiLipSyncExtendedInterface;
class IConvaiVisionInterface;


/**
 * Template for queues.
 *
 * This template implements an unbounded non-intrusive queue using a lock-free linked
 * list that stores copies of the queued items. The template can operate in two modes:
 * Multiple-producers single-consumer (MPSC) and Single-producer single-consumer (SPSC).
 *
 * The queue is thread-safe in both modes. The Dequeue() method ensures thread-safety by
 * writing it in a way that does not depend on possible instruction reordering on the CPU.
 * The Enqueue() method uses an atomic compare-and-swap in multiple-producers scenarios.
 *
 * @param T The type of items stored in the queue.
 * @param Mode The queue mode (single-producer, single-consumer by default).
 * @todo gmp: Implement node pooling.
 */
template<typename T, EQueueMode Mode = EQueueMode::Spsc>
class TConvaiQueue
{
public:
	using FElementType = T;

	/** Default constructor. */
	TConvaiQueue()
	{
		Head = Tail = new TNode();
	}

	/** Destructor. */
	~TConvaiQueue()
	{
		while (Tail != nullptr)
		{
			TNode* Node = Tail;
			Tail = Tail->NextNode;

			delete Node;
		}
	}

	/**
	 * Removes and returns the item from the tail of the queue.
	 *
	 * @param OutValue Will hold the returned value.
	 * @return true if a value was returned, false if the queue was empty.
	 * @note To be called only from consumer thread.
	 * @see Empty, Enqueue, IsEmpty, Peek, Pop
	 */
	bool Dequeue(FElementType& OutItem)
	{
		TNode* Popped = Tail->NextNode;

		if (Popped == nullptr)
		{
			return false;
		}

		TSAN_AFTER(&Tail->NextNode);
		OutItem = MoveTemp(Popped->Item);

		TNode* OldTail = Tail;
		Tail = Popped;
		Tail->Item = FElementType();
		delete OldTail;

		return true;
	}

	/**
	 * Empty the queue, discarding all items.
	 *
	 * @note To be called only from consumer thread.
	 * @see Dequeue, IsEmpty, Peek, Pop
	 */
	void Empty()
	{
		while (Pop());
	}

	/**
	 * Adds an item to the head of the queue.
	 *
	 * @param Item The item to add.
	 * @return true if the item was added, false otherwise.
	 * @note To be called only from producer thread(s).
	 * @see Dequeue, Pop
	 */
	bool Enqueue(const FElementType& Item)
	{
		TNode* NewNode = new TNode(Item);

		if (NewNode == nullptr)
		{
			return false;
		}

		TNode* OldHead;

		if (Mode == EQueueMode::Mpsc)
		{
			OldHead = (TNode*)FPlatformAtomics::InterlockedExchangePtr((void**)&Head, NewNode);
			TSAN_BEFORE(&OldHead->NextNode);
			FPlatformAtomics::InterlockedExchangePtr((void**)&OldHead->NextNode, NewNode);
		}
		else
		{
			OldHead = Head;
			Head = NewNode;
			TSAN_BEFORE(&OldHead->NextNode);
			FPlatformMisc::MemoryBarrier();
			OldHead->NextNode = NewNode;
		}

		return true;
	}

	/**
	 * Adds an item to the head of the queue.
	 *
	 * @param Item The item to add.
	 * @return true if the item was added, false otherwise.
	 * @note To be called only from producer thread(s).
	 * @see Dequeue, Pop
	 */
	bool Enqueue(FElementType&& Item)
	{
		TNode* NewNode = new TNode(MoveTemp(Item));

		if (NewNode == nullptr)
		{
			return false;
		}

		TNode* OldHead;

		if (Mode == EQueueMode::Mpsc)
		{
			OldHead = (TNode*)FPlatformAtomics::InterlockedExchangePtr((void**)&Head, NewNode);
			TSAN_BEFORE(&OldHead->NextNode);
			FPlatformAtomics::InterlockedExchangePtr((void**)&OldHead->NextNode, NewNode);
		}
		else
		{
			OldHead = Head;
			Head = NewNode;
			TSAN_BEFORE(&OldHead->NextNode);
			FPlatformMisc::MemoryBarrier();
			OldHead->NextNode = NewNode;
		}

		return true;
	}

	/**
	 * Checks whether the queue is empty.
	 *
	 * @return true if the queue is empty, false otherwise.
	 * @note To be called only from consumer thread.
	 * @see Dequeue, Empty, Peek, Pop
	 */
	bool IsEmpty() const
	{
		return (Tail->NextNode == nullptr);
	}

	/**
	 * Peeks at the queue's tail item without removing it.
	 *
	 * @param OutItem Will hold the peeked at item.
	 * @return true if an item was returned, false if the queue was empty.
	 * @note To be called only from consumer thread.
	 * @see Dequeue, Empty, IsEmpty, Pop
	 */
	bool Peek(FElementType& OutItem) const
	{
		if (Tail->NextNode == nullptr)
		{
			return false;
		}

		OutItem = Tail->NextNode->Item;

		return true;
	}

	/**
	 * Peek at the queue's tail item without removing it.
	 *
	 * This version of Peek allows peeking at a queue of items that do not allow
	 * copying, such as TUniquePtr.
	 *
	 * @return Pointer to the item, or nullptr if queue is empty
	 */
	FElementType* Peek()
	{
		if (Tail->NextNode == nullptr)
		{
			return nullptr;
		}

		return &Tail->NextNode->Item;
	}

	/**
	 * Peek at the queue's head item
	 *
	 *
	 * @return Pointer to the item, or nullptr if queue is empty
	 */
	FElementType* PeekHead()
	{
		if (Tail->NextNode == nullptr)
		{
			return nullptr;
		}

		return &Head->Item;
	}


	FORCEINLINE const FElementType* Peek() const
	{
		return const_cast<TConvaiQueue*>(this)->Peek();
	}

	/**
	 * Removes the item from the tail of the queue.
	 *
	 * @return true if a value was removed, false if the queue was empty.
	 * @note To be called only from consumer thread.
	 * @see Dequeue, Empty, Enqueue, IsEmpty, Peek
	 */
	bool Pop()
	{
		TNode* Popped = Tail->NextNode;

		if (Popped == nullptr)
		{
			return false;
		}

		TSAN_AFTER(&Tail->NextNode);

		TNode* OldTail = Tail;
		Tail = Popped;
		Tail->Item = FElementType();
		delete OldTail;

		return true;
	}

private:

	/** Structure for the internal linked list. */
	struct TNode
	{
		/** Holds a pointer to the next node in the list. */
		TNode* volatile NextNode;

		/** Holds the node's item. */
		FElementType Item;

		/** Default constructor. */
		TNode()
			: NextNode(nullptr)
		{ }

		/** Creates and initializes a new node. */
		explicit TNode(const FElementType& InItem)
			: NextNode(nullptr)
			, Item(InItem)
		{ }

		/** Creates and initializes a new node. */
		explicit TNode(FElementType&& InItem)
			: NextNode(nullptr)
			, Item(MoveTemp(InItem))
		{ }
	};

	/** Holds a pointer to the head of the list. */
	MS_ALIGN(16) TNode* volatile Head GCC_ALIGN(16);

	/** Holds a pointer to the tail of the list. */
	TNode* Tail;

private:

	/** Hidden copy constructor. */
	TConvaiQueue(const TConvaiQueue&) = delete;

	/** Hidden assignment operator. */
	TConvaiQueue& operator=(const TConvaiQueue&) = delete;
};

UCLASS()
class UConvaiAudioStreamer : public UAudioComponent
{
	GENERATED_UCLASS_BODY()

	DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE(FOnStartedTalkingSignature, UConvaiAudioStreamer, OnStartedTalkingDelegate);
	DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE(FOnFinishedTalkingSignature, UConvaiAudioStreamer, OnFinishedTalkingDelegate);
	DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE(FOnFacialDataReadySignature, UConvaiAudioStreamer, OnFacialDataReadyDelegate);

public:
	void PlayVoiceData(uint8* VoiceData, uint32 VoiceDataSize, bool ContainsHeaderData=true, uint32 SampleRate=21000, uint32 NumChannels=1);

	void PlayVoiceData(uint8* VoiceData, uint32 VoiceDataSize, bool ContainsHeaderData, FAnimationSequence FaceSequence, uint32 SampleRate = 21000, uint32 NumChannels = 1);

	void ForcePlayVoice(USoundWave* VoiceToPlay);

	void StopVoice();

	void PauseVoice();

	void ResumeVoice();

	void StopVoiceWithFade(float InRemainingVoiceFadeOutTime);

	void ResetVoiceFade();

	void UpdateVoiceFade(float DeltaTime);

	bool IsVoiceCurrentlyFading();

	void ClearAudioFinishedTimer();

	bool IsLocal();

	/** Called when starts to talk */
	UPROPERTY(BlueprintAssignable, Category = "Convai")
	FOnStartedTalkingSignature OnStartedTalkingDelegate;

	/** Called when stops to talk */
	UPROPERTY(BlueprintAssignable, Category = "Convai")
	FOnFinishedTalkingSignature OnFinishedTalkingDelegate;

	/** Called when there are LipSync facial data available */
	UPROPERTY(BlueprintAssignable, Category = "Convai|LipSync")
	FOnFacialDataReadySignature OnFacialDataReadyDelegate;

	IConvaiLipSyncInterface* FindFirstLipSyncComponent();

	UFUNCTION(BlueprintCallable, Category = "Convai|LipSync")
	bool SetLipSyncComponent(UActorComponent* LipSyncComponent);

	/** Returns true, if an LipSync Component was available and attached to the character */
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|LipSync")
	bool SupportsLipSync();

	IConvaiVisionInterface* FindFirstVisionComponent();

	UFUNCTION(BlueprintCallable, Category = "Convai|Vision")
	bool SetVisionComponent(UActorComponent* VisionComponent);

	/** Returns true, if an Vision Component was available and attached to the character */
	UFUNCTION(BlueprintPure, BlueprintCallable, Category = "Convai|Vision")
	bool SupportsVision();

public:
	// UActorComponent interface
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// UObject Interface.
	virtual void BeginDestroy() override;

public:

	FTimerHandle AudioFinishedTimerHandle;
	FTimerHandle LypSyncTimeoutTimerHandle;
	double AudioEndTime = 0.0;
	bool IsTalking = false;
	float TotalVoiceFadeOutTime;
	float RemainingVoiceFadeOutTime;

	UPROPERTY()
	USoundWaveProcedural* SoundWaveProcedural;

	IConvaiLipSyncInterface* ConvaiLipSync;
	IConvaiVisionInterface* ConvaiVision;

	void PlayLipSyncWithPrecomputedFacialAnimationSynced(FAnimationSequence& FaceSequence);

	void PlayLipSyncWithPrecomputedFacialAnimation(FAnimationSequence FaceSequence);

	void PlayLipSync(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels);

	void StopLipSync();

	void PauseLipSync();

	void ResumeLipSync();

	virtual bool CanUseLipSync();

	virtual void ForceRecalculateLipsyncStartTime();

	virtual bool CanUseVision();

	void OnFacialDataReadyCallback();

	void OnLipSyncTimeOut();

	UFUNCTION(BlueprintPure, Category = "Convai|LipSync", Meta = (Tooltip = "Returns last predicted facial data scores"))
	const TArray<float> GetFacialData() const;

	UFUNCTION(BlueprintPure, Category = "Convai|LipSync", Meta = (Tooltip = "Returns list of facial data names"))
	const TArray<FString> GetFacialDataNames() const;

	UFUNCTION(BlueprintPure, Category = "Convai|LipSync", Meta = (Tooltip = "Returns map of blendshapes"))
	const TMap<FName, float> ConvaiGetFaceBlendshapes() const;

	UFUNCTION(BlueprintPure, Category = "Convai|LipSync", Meta = (Tooltip = "True if the output facial data is in Blendshape format"))
	bool GeneratesFacialDataAsBlendshapes();


	virtual void onAudioStarted();
	virtual void onAudioFinished();

// Track if audio is currently playing
bool bIsPlayingAudio;

// Configuration parameters
float MinBufferDuration;

// Thread-safe buffers for transport thread -> game thread communication
FAudioRingBuffer AudioRingBuffer;      // For incoming audio from transport thread
FLipSyncBuffer LipSyncBuffer;          // For accumulating lipsync frames

// Track how many bytes we've sent to lipsync component (for non-precomputed lipsync)
uint32 BytesSentToLipSync;

// Audio handling functions (called from transport thread - lightweight)
void HandleAudioReceived(uint8* AudioData, uint32 AudioDataSize, bool ContainsHeaderData, uint32 SampleRate, uint32 NumChannels);
void HandleLipSyncReceived(FAnimationSequence& FaceSequence);

// Processing functions (called from game thread - heavy logic)
void ProcessIncomingAudio(bool Force = false);
void ProcessIncomingLipSync();
void ForcePlayBufferedAudio();

// Helper function to dequeue audio data if ready, returns number of bytes dequeued (0 if not ready)
uint32 TryDequeueAudioChunk(TArray<uint8>& OutAudioData, uint32& OutSampleRate, uint32& OutNumChannels, bool Force = false);

// Helper function to send new buffered audio to non-precomputed lipsync component
void SendNewAudioToLipSync(uint32 AvailableBytes, uint32 SampleRate, uint32 NumChannels);

/**
 * Returns the duration of content (audio) that is
 * currently playing or buffered and ready to play.
 * @return Duration in seconds of content remaining
 */
UFUNCTION(BlueprintPure, Category = "Convai|Audio")
double GetRemainingContentDuration();


private:

	// Critical section for protecting SoundWaveProcedural operations
	FCriticalSection AudioConfigLock;
	FThreadSafeBool IsAudioConfiguring;

	// Buffer for pending audio data when lock is held
	TArray<uint8> PendingAudioBuffer;

	// Reusable buffer for processing audio chunks (avoids repeated allocations)
	TArray<uint8> AudioChunkBuffer;
	const uint32 MaxChunkSize = 1024 * 400; // 400KB chunks

	// Reusable buffer for peeking audio to send to lipsync (avoids repeated allocations)
	TArray<uint8> LipSyncPeekBuffer;

	// Process any pending audio data
	void ProcessPendingAudio();
};
