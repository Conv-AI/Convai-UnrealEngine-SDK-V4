// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiTestEventSink.h"

#include "ConvaiChatbotComponent.h"
#include "ConvaiConversationComponent.h"
#include "ConvaiTests.h"

#include "HAL/PlatformTime.h"

void UConvaiTestEventSink::HandleTranscription(UConvaiConversationComponent* Speaker,
                                               UConvaiConversationComponent* Listener,
                                               FString Transcription, bool IsTranscriptionReady,
                                               bool IsFinal)
{
    FTranscript Entry;
    // Names rather than pointers: the components outlive nothing in particular
    // and a scenario reading this after teardown would be reading freed memory.
    Entry.Speaker = Speaker ? Speaker->GetName() : TEXT("<none>");
    Entry.Listener = Listener ? Listener->GetName() : TEXT("<none>");
    Entry.Text = MoveTemp(Transcription);
    Entry.bReady = IsTranscriptionReady;
    Entry.bFinal = IsFinal;
    Entry.AtSeconds = FPlatformTime::Seconds();

    UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS transcript speaker=%s final=%d text=%s"),
           *Entry.Speaker, IsFinal ? 1 : 0, *Entry.Text);

    FScopeLock ScopeLock(&Lock);
    Recorded.Add(MoveTemp(Entry));
}

void UConvaiTestEventSink::HandleFailure()
{
    UE_LOG(LogConvaiTests, Error, TEXT("CONVAI_TESTS OnFailureEvent"));
    FScopeLock ScopeLock(&Lock);
    ++Failures;
}

void UConvaiTestEventSink::HandleCharacterDataLoaded(UConvaiChatbotComponent* /*ChatbotComponent*/,
                                                     bool Success)
{
    UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS character_data success=%d"), Success ? 1 : 0);
    if (Success)
    {
        return;
    }
    FScopeLock ScopeLock(&Lock);
    ++CharacterDataFailures;
}

int32 UConvaiTestEventSink::CharacterDataLoadFailures() const
{
    FScopeLock ScopeLock(&Lock);
    return CharacterDataFailures;
}

void UConvaiTestEventSink::HandleBotTurnCompleted(UConvaiChatbotComponent* /*ChatbotComponent*/,
                                                  FString ResponseId, bool bWasInterrupted,
                                                  bool bWasAborted, FString ErrorReason)
{
    FBotTurn Turn;
    Turn.ResponseId = MoveTemp(ResponseId);
    Turn.bInterrupted = bWasInterrupted;
    Turn.bAborted = bWasAborted;
    Turn.ErrorReason = MoveTemp(ErrorReason);
    Turn.AtSeconds = FPlatformTime::Seconds();

    UE_LOG(LogConvaiTests, Display,
           TEXT("CONVAI_TESTS bot_turn id=%s interrupted=%d aborted=%d error=%s"),
           *Turn.ResponseId, bWasInterrupted ? 1 : 0, bWasAborted ? 1 : 0, *Turn.ErrorReason);

    FScopeLock ScopeLock(&Lock);
    Turns.Add(MoveTemp(Turn));
}

void UConvaiTestEventSink::HandleInterrupted(UConvaiChatbotComponent* /*ChatbotComponent*/,
                                             UConvaiPlayerComponent* /*InteractingPlayerComponent*/)
{
    UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS OnInterruptedEvent"));
    FScopeLock ScopeLock(&Lock);
    ++Interrupts;
}

TArray<UConvaiTestEventSink::FTranscript> UConvaiTestEventSink::Transcripts() const
{
    FScopeLock ScopeLock(&Lock);
    return Recorded;
}

FString UConvaiTestEventSink::LatestFinalText() const
{
    FScopeLock ScopeLock(&Lock);
    for (int32 i = Recorded.Num() - 1; i >= 0; --i)
    {
        if (Recorded[i].bFinal && !Recorded[i].Text.IsEmpty())
        {
            return Recorded[i].Text;
        }
    }
    return FString();
}

TSet<FString> UConvaiTestEventSink::Speakers() const
{
    FScopeLock ScopeLock(&Lock);
    TSet<FString> Result;
    for (const FTranscript& Entry : Recorded)
    {
        Result.Add(Entry.Speaker);
    }
    return Result;
}

int32 UConvaiTestEventSink::FailureCount() const
{
    FScopeLock ScopeLock(&Lock);
    return Failures;
}

void UConvaiTestEventSink::HandleServerError(UConvaiChatbotComponent* /*ChatbotComponent*/,
                                             FString Message, bool bFatal)
{
    UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS OnServerErrorEvent fatal=%d %s"),
           bFatal ? 1 : 0, *Message);
    FScopeLock ScopeLock(&Lock);
    FServerError Error;
    Error.Message = MoveTemp(Message);
    Error.bFatal = bFatal;
    Error.AtSeconds = FPlatformTime::Seconds();
    ServerErrorsSeen.Add(MoveTemp(Error));
}

TArray<UConvaiTestEventSink::FServerError> UConvaiTestEventSink::ServerErrors() const
{
    FScopeLock ScopeLock(&Lock);
    return ServerErrorsSeen;
}

TArray<UConvaiTestEventSink::FTranscript> UConvaiTestEventSink::TranscriptsFrom(
    const FString& SpeakerName) const
{
    FScopeLock ScopeLock(&Lock);
    TArray<FTranscript> Result;
    for (const FTranscript& Entry : Recorded)
    {
        if (Entry.Speaker == SpeakerName)
        {
            Result.Add(Entry);
        }
    }
    return Result;
}

FString UConvaiTestEventSink::LatestFinalTextFrom(const FString& SpeakerName) const
{
    FScopeLock ScopeLock(&Lock);
    for (int32 i = Recorded.Num() - 1; i >= 0; --i)
    {
        if (Recorded[i].Speaker == SpeakerName && Recorded[i].bFinal &&
            !Recorded[i].Text.IsEmpty())
        {
            return Recorded[i].Text;
        }
    }
    return FString();
}

TArray<UConvaiTestEventSink::FBotTurn> UConvaiTestEventSink::BotTurns() const
{
    FScopeLock ScopeLock(&Lock);
    return Turns;
}

void UConvaiTestEventSink::HandleEmotionStateChanged(UConvaiChatbotComponent* ChatbotComponent,
                                                     UConvaiPlayerComponent* /*InteractingPlayerComponent*/)
{
    FEmotionSample Sample;
    Sample.AtSeconds = FPlatformTime::Seconds();
    if (ChatbotComponent)
    {
        for (int32 i = 0; i <= static_cast<int32>(EBasicEmotions::Anticipation); ++i)
        {
            const EBasicEmotions Emotion = static_cast<EBasicEmotions>(i);
            const float Score = ChatbotComponent->GetEmotionScore(Emotion);
            if (Score > Sample.Score)
            {
                Sample.Dominant = Emotion;
                Sample.Score = Score;
            }
        }
    }

    UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS emotion dominant=%s score=%.2f"),
           *StaticEnum<EBasicEmotions>()->GetNameStringByValue(static_cast<int64>(Sample.Dominant)),
           Sample.Score);

    FScopeLock ScopeLock(&Lock);
    Emotions.Add(Sample);
}

TArray<UConvaiTestEventSink::FEmotionSample> UConvaiTestEventSink::EmotionSamples() const
{
    FScopeLock ScopeLock(&Lock);
    return Emotions;
}

int32 UConvaiTestEventSink::InterruptCount() const
{
    FScopeLock ScopeLock(&Lock);
    return Interrupts;
}

void UConvaiTestEventSink::HandleActionsReceived(UConvaiChatbotComponent* /*ChatbotComponent*/,
                                                 UConvaiPlayerComponent* /*InteractingPlayerComponent*/,
                                                 const TArray<FConvaiResultAction>& SequenceOfActions)
{
    FActionBatch Batch;
    Batch.Actions = SequenceOfActions;
    Batch.AtSeconds = FPlatformTime::Seconds();

    for (const FConvaiResultAction& Action : SequenceOfActions)
    {
        UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS action name=%s raw=%s params=%d"),
               *Action.Action, *Action.ActionString, Action.Parameters.Num());
    }

    FScopeLock ScopeLock(&Lock);
    Batches.Add(MoveTemp(Batch));
}

TArray<UConvaiTestEventSink::FActionBatch> UConvaiTestEventSink::ActionBatches() const
{
    FScopeLock ScopeLock(&Lock);
    return Batches;
}

void UConvaiTestEventSink::HandleFacialDataReady()
{
    FScopeLock ScopeLock(&Lock);
    if (FacialFrameCount++ == 0)
    {
        UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS first_face_frame"));
    }
}

int32 UConvaiTestEventSink::FacialFrames() const
{
    FScopeLock ScopeLock(&Lock);
    return FacialFrameCount;
}
