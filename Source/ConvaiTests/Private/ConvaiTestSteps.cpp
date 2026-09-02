// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiTestSteps.h"

#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiTestEventSink.h"
#include "ConvaiTests.h"
#include "ConvaiUtils.h"
#include "ConvaiVirtualMicComponent.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    // Ported from convai-livekit-cpp-p/tests/harness/wav_loader.cpp rather than
    // rewritten, so the two repos disagree about a fixture only if the fixture
    // changed. Reads the whole file first because these are test inputs of a
    // megabyte or so and a chunk walk over memory is simpler than one over a
    // stream.
    uint16 ReadU16(const uint8* P) { return static_cast<uint16>(P[0] | (P[1] << 8)); }

    uint32 ReadU32(const uint8* P)
    {
        return static_cast<uint32>(P[0]) | (static_cast<uint32>(P[1]) << 8) |
               (static_cast<uint32>(P[2]) << 16) | (static_cast<uint32>(P[3]) << 24);
    }

    bool LoadPcmWav(const FString& Path, TArray<int16>& OutSamples, int32& OutSampleRate,
                    int32& OutChannels, FString& OutError)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *Path))
        {
            OutError = FString::Printf(TEXT("cannot open wav file: %s"), *Path);
            return false;
        }
        if (Bytes.Num() < 12 || FMemory::Memcmp(Bytes.GetData(), "RIFF", 4) != 0 ||
            FMemory::Memcmp(Bytes.GetData() + 8, "WAVE", 4) != 0)
        {
            OutError = FString::Printf(TEXT("not a RIFF/WAVE file: %s"), *Path);
            return false;
        }

        bool bGotFmt = false;
        bool bGotData = false;
        int32 Cursor = 12;
        while (Cursor + 8 <= Bytes.Num() && !(bGotFmt && bGotData))
        {
            const uint8* Header = Bytes.GetData() + Cursor;
            const int64 ChunkSize = ReadU32(Header + 4);
            const int32 Body = Cursor + 8;
            if (Body + ChunkSize > Bytes.Num())
            {
                OutError = TEXT("truncated chunk");
                return false;
            }

            if (FMemory::Memcmp(Header, "fmt ", 4) == 0)
            {
                if (ChunkSize < 16)
                {
                    OutError = TEXT("fmt chunk too small");
                    return false;
                }
                const uint8* Fmt = Bytes.GetData() + Body;
                const uint16 AudioFormat = ReadU16(Fmt + 0);
                OutChannels = ReadU16(Fmt + 2);
                OutSampleRate = ReadU32(Fmt + 4);
                const uint16 BitsPerSample = ReadU16(Fmt + 14);
                if (AudioFormat != 1 || BitsPerSample != 16)
                {
                    OutError = FString::Printf(
                        TEXT("only 16-bit PCM wav supported (format %u, %u bits)"), AudioFormat,
                        BitsPerSample);
                    return false;
                }
                bGotFmt = true;
            }
            else if (FMemory::Memcmp(Header, "data", 4) == 0)
            {
                if (!bGotFmt)
                {
                    OutError = TEXT("data chunk before fmt");
                    return false;
                }
                OutSamples.SetNumUninitialized(static_cast<int32>(ChunkSize / sizeof(int16)));
                FMemory::Memcpy(OutSamples.GetData(), Bytes.GetData() + Body, ChunkSize);
                bGotData = true;
            }

            // Chunks are word-aligned, so an odd size carries a pad byte.
            Cursor = Body + static_cast<int32>(ChunkSize + (ChunkSize & 1));
        }

        if (!bGotFmt || !bGotData)
        {
            OutError = TEXT("wav has no fmt or no data chunk");
            return false;
        }
        return true;
    }

    TArray<FString> NormalisedWords(const FString& Text)
    {
        FString Cleaned;
        Cleaned.Reserve(Text.Len());
        for (const TCHAR Char : Text)
        {
            // Punctuation is folded to a separator rather than dropped, so
            // "don't" and "dont" differ by one word rather than compare equal
            // to something else entirely.
            Cleaned.AppendChar(FChar::IsAlnum(Char) ? FChar::ToLower(Char) : TEXT(' '));
        }
        TArray<FString> Words;
        Cleaned.ParseIntoArrayWS(Words);
        return Words;
    }
}

namespace ConvaiTestSteps
{
    FString DataDir()
    {
        if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ConvAI")))
        {
            return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/ConvaiTests/Data"));
        }
        return FString();
    }

    bool SpeakWav(UConvaiVirtualMicComponent* Mic, const FString& FixtureName, FString& OutError)
    {
        if (!Mic)
        {
            OutError = TEXT("no Virtual Mic");
            return false;
        }

        const FString Path = FPaths::Combine(DataDir(), FixtureName + TEXT(".wav"));
        TArray<int16> Samples;
        int32 SampleRate = 0;
        int32 Channels = 0;
        if (!LoadPcmWav(Path, Samples, SampleRate, Channels, OutError))
        {
            return false;
        }
        if (Samples.Num() == 0)
        {
            OutError = FString::Printf(TEXT("fixture %s carries no samples"), *FixtureName);
            return false;
        }

        const int32 TargetRate = UConvaiVirtualMicComponent::CaptureSampleRate();
        if (SampleRate == TargetRate && Channels == 1)
        {
            Mic->EnqueueSamples(Samples);
        }
        else
        {
            TArray<int16> Resampled;
            UConvaiUtils::ResampleAudio(static_cast<float>(SampleRate),
                                        static_cast<float>(TargetRate), Channels,
                                        /*reduceToMono=*/true, Samples, Samples.Num(), Resampled);
            if (Resampled.Num() == 0)
            {
                OutError = FString::Printf(
                    TEXT("resampling %s from %d Hz / %d ch to %d Hz mono produced nothing"),
                    *FixtureName, SampleRate, Channels, TargetRate);
                return false;
            }
            Mic->EnqueueSamples(Resampled);
        }

        UE_LOG(LogConvaiTests, Display, TEXT("CONVAI_TESTS speak_wav=%s src=%dHz/%dch"),
               *FixtureName, SampleRate, Channels);
        return true;
    }

    void Silence(UConvaiVirtualMicComponent* Mic, float Seconds)
    {
        if (Mic)
        {
            Mic->EnqueueSilence(Seconds);
        }
    }

    void SetTalkTargets(UConvaiPlayerComponent* Player,
                        const TArray<UConvaiChatbotComponent*>& Targets)
    {
        // There is one Connection per process here, so the microphone is not
        // addressed at a target set -- but it does not flow until the player
        // holds a session of its own: UnmuteStreamingAudio returns false with
        // "No valid session" while SessionProxyInstance is null, and every
        // scenario that speaks goes through it. So the single-connection
        // equivalent of naming a target is opening the player's own session.
        //
        // Which targets were named is genuinely unused; the step keeps the
        // argument so scenarios read the same wherever Talk Targets exist, and
        // so this is the one function that changes when they arrive.
        // Called unconditionally rather than behind IsPlayerConnected(), which
        // is false for the whole handshake and would mistake a connecting
        // session for a missing one; StartSession stops an existing session
        // itself.
        (void)Targets;
        if (Player)
        {
            Player->StartSession();
        }
    }

    void SayText(UConvaiPlayerComponent* Player, UConvaiConversationComponent* Chatbot,
                 const FString& Text)
    {
        if (Player)
        {
            Player->SendText(Chatbot, Text);
        }
    }

    FString ExpectedTranscript(const FString& FixtureName)
    {
        FString Json;
        if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(DataDir(), TEXT("STT.json"))))
        {
            return FString();
        }
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return FString();
        }
        FString Transcript;
        Root->TryGetStringField(FixtureName, Transcript);
        return Transcript;
    }

    FConvaiScenarioFinding FailureEventFinding(int32 Failures, int32 CharacterDataFailures,
                                               const FString& Exchange,
                                               const TMap<FString, double>& Metrics)
    {
        FConvaiScenarioFinding Finding;
        if (CharacterDataFailures > 0)
        {
            Finding.DedupKey = TEXT("character-details-request-failed");
            Finding.Summary = TEXT("the character-details request failed and OnFailureEvent fired, "
                                   "although the session itself ran");
            Finding.Evidence = FString::Printf(
                TEXT("OnCharacterDataLoadEvent_V2 reported failure %d time(s) and OnFailureEvent fired "
                     "%d time(s) during %s. A game bound to OnFailure shows an error for a character "
                     "that went on to answer."),
                CharacterDataFailures, Failures, *Exchange);
        }
        else
        {
            Finding.DedupKey = TEXT("on-failure-event-during-exchange");
            Finding.Summary = TEXT("OnFailureEvent fired during an exchange, from something other than "
                                   "the character-details request");
            Finding.Evidence = FString::Printf(
                TEXT("OnFailureEvent fired %d time(s) during %s; the character-details request did "
                     "not report failure. The log carries the plugin's warning for it."),
                Failures, *Exchange);
        }
        Finding.Metrics = Metrics;
        return Finding;
    }

    FString AssembledTextFrom(const UConvaiTestEventSink& Sink, const FString& SpeakerName)
    {
        FString Assembled;
        for (const UConvaiTestEventSink::FTranscript& Entry : Sink.TranscriptsFrom(SpeakerName))
        {
            if (!Entry.bFinal)
            {
                Assembled += Entry.Text;
            }
        }
        return Assembled;
    }

    TOptional<FConvaiScenarioFinding> TranscriptShapeFinding(const UConvaiTestEventSink& Sink,
                                                             const FString& SpeakerName,
                                                             ETranscriptShape Shape,
                                                             const TMap<FString, double>& Metrics)
    {
        const TArray<UConvaiTestEventSink::FTranscript> Stream = Sink.TranscriptsFrom(SpeakerName);
        if (Stream.Num() == 0)
        {
            return {};
        }

        const bool bWhole = Shape == ETranscriptShape::WholeUtterance;

        FString SoFar;
        FString Breach;
        int32 WrongFinals = 0;
        int32 WrongReadyFlags = 0;

        for (const UConvaiTestEventSink::FTranscript& Entry : Stream)
        {
            if (Entry.bReady == bWhole)
            {
                // Whole-utterance broadcasts must never set it; increments must
                // always set it. Either way this entry contradicts the shape.
                ++WrongReadyFlags;
            }

            if (Entry.bFinal)
            {
                if (Entry.Text.IsEmpty() == bWhole)
                {
                    ++WrongFinals;
                }
                // The final closes the utterance either way: the next partial
                // starts a new one and neither extends nor appends to this.
                SoFar.Reset();
                continue;
            }

            if (Breach.IsEmpty() && !SoFar.IsEmpty())
            {
                const bool bOk = bWhole ? Entry.Text.StartsWith(SoFar, ESearchCase::CaseSensitive)
                                        : !Entry.Text.Contains(SoFar, ESearchCase::CaseSensitive);
                if (!bOk)
                {
                    Breach = bWhole ? FString::Printf(TEXT("\"%s\" does not extend \"%s\""),
                                                      *Entry.Text, *SoFar)
                                    : FString::Printf(TEXT("\"%s\" re-sends \"%s\""),
                                                      *Entry.Text, *SoFar);
                }
            }
            SoFar = bWhole ? Entry.Text : SoFar + Entry.Text;
        }

        if (Breach.IsEmpty() && WrongFinals == 0 && WrongReadyFlags == 0)
        {
            return {};
        }

        FConvaiScenarioFinding Finding;
        Finding.DedupKey = bWhole ? TEXT("transcript-stream-breaks-whole-utterance-contract")
                                  : TEXT("transcript-stream-breaks-increment-contract");
        Finding.Summary =
            bWhole ? TEXT("the player's transcript broadcasts do not each carry the whole "
                          "utterance so far, so a listener that replaces what it has renders "
                          "only a fragment of what was said")
                   : TEXT("the character's transcript broadcasts are not increments, so a "
                          "listener that appends what it is handed renders the answer once per "
                          "broadcast");
        Finding.Evidence = FString::Printf(
            TEXT("%s broadcast %d transcription(s), graded as %s. Non-final broadcasts breaking "
                 "the shape: %s. Finals of the wrong kind (%s expected): %d. Broadcasts with the "
                 "wrong IsTranscriptionReady (%s expected): %d. The contract is written at "
                 "FOnTranscriptionReceivedSignature."),
            *SpeakerName, Stream.Num(), bWhole ? TEXT("whole-utterance") : TEXT("increment"),
            Breach.IsEmpty() ? TEXT("none") : *Breach,
            bWhole ? TEXT("non-empty") : TEXT("empty"), WrongFinals,
            bWhole ? TEXT("false") : TEXT("true"), WrongReadyFlags);
        Finding.Metrics = Metrics;
        return Finding;
    }

    double WordErrorRate(const FString& Reference, const FString& Hypothesis)
    {
        const TArray<FString> Ref = NormalisedWords(Reference);
        const TArray<FString> Hyp = NormalisedWords(Hypothesis);
        if (Ref.Num() == 0)
        {
            return Hyp.Num() == 0 ? 0.0 : 1.0;
        }

        // One row at a time: the fixtures run to a few hundred words and the
        // full matrix buys nothing without a backtrace, which nothing needs.
        TArray<int32> Previous;
        Previous.SetNumUninitialized(Hyp.Num() + 1);
        for (int32 j = 0; j <= Hyp.Num(); ++j)
        {
            Previous[j] = j;
        }

        TArray<int32> Current;
        Current.SetNumUninitialized(Hyp.Num() + 1);
        for (int32 i = 1; i <= Ref.Num(); ++i)
        {
            Current[0] = i;
            for (int32 j = 1; j <= Hyp.Num(); ++j)
            {
                const int32 Substitution = Previous[j - 1] + (Ref[i - 1] == Hyp[j - 1] ? 0 : 1);
                Current[j] = FMath::Min(Substitution, FMath::Min(Previous[j] + 1, Current[j - 1] + 1));
            }
            Previous = Current;
        }

        return static_cast<double>(Previous[Hyp.Num()]) / Ref.Num();
    }
}
