// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#if WITH_TESTS

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ConvaiConversationStateTestProbe.generated.h"

class UConvaiConversationComponent;

/** Event receiver used only by the native conversation-state automation tests. */
UCLASS()
class UConvaiConversationStateTestProbe : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleStartedTalking()
	{
		++StartedCount;
		EventOrder.Add(TEXT("Started"));
	}

	UFUNCTION()
	void HandleFinishedTalking()
	{
		++FinishedCount;
		EventOrder.Add(TEXT("Finished"));
	}

	UFUNCTION()
	void HandleTranscription(
		UConvaiConversationComponent* Speaker,
		UConvaiConversationComponent* Listener,
		FString Transcription,
		bool bIsTranscriptionReady,
		bool bIsFinal)
	{
		(void)Speaker;
		(void)Listener;
		(void)bIsTranscriptionReady;
		Transcription.TrimStartAndEndInline();
		if (bIsFinal && !Transcription.IsEmpty())
		{
			EventOrder.Add(TEXT("FinalTranscript"));
		}
	}

	int32 StartedCount = 0;
	int32 FinishedCount = 0;
	TArray<FName> EventOrder;
};
#endif // WITH_TESTS
