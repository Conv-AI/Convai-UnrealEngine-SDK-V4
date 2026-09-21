// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiSubsystem.h"
#include "Core/ConvaiConnectReaper.h"
#include "ConvaiActionUtils.h"
#include "Core/ConvaiConnectionManager.h"
#include "Utility/ConvaiUtf8String.h"
#include "ConvaiUtils.h"
#include "ConvaiAndroid.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiObjectComponent.h"
#include "ConvaiContextSubsystem.h"
#include "Containers/Ticker.h"
#include "ConvaiReferenceAudioThread.h"
#include "HttpModule.h"
#include "convai/convai_client.h"
#include "../Convai.h"
#include "Interfaces/IHttpResponse.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Misc/App.h"
#include <atomic>

// LOCAL BYPASS -- remove once the matching convai_client binaries land.
// convai_client.h declares SetStreamDelay/GetAECStats, but the shipped win64
// import lib (Aug 31 drop) exports neither, so linking the calls fails with
// LNK2019. Both are AEC diagnostics/tuning only; skipping them costs nothing
// outside AEC work. Flip this to 1 when the rebuilt .lib/.dll arrive.
#ifndef CONVAI_HAS_AEC_STATS_API
#define CONVAI_HAS_AEC_STATS_API 0
#endif

DEFINE_LOG_CATEGORY(ConvaiSubsystemLog);
DEFINE_LOG_CATEGORY(ConvaiClientLog);

namespace
{
    enum class EC_PacketType : uint8
    {
        UserStartedSpeaking,
        UserStoppedSpeaking,
        UserTranscription,
        FinalUserTranscription,
        BotLLMStarted,
        BotLLMStopped,
        BotStartedSpeaking,
        BotStoppedSpeaking,
        BotTurnCompleted,
        BotTranscription,
        ServerMessage,
        BotReady,
        BotLLMText,
        UserLLMText,
        BotTTSStarted,
        BotTTSStopped,
        BotTTSText,
        BotOutput,
        LLMFunctionCallLifecycle,
        ErrorResponse,
        Error,
        Unknown
    };

    enum class EC_ServerPacketType : uint8
    {
        BotEmotion,
        ActionResponse,
        BTResponse,
        ModerationResponse,
        Visemes,
        NeurosyncBlendshapes,
        ChunkedNeurosyncBlendshapes,
        BlendshapeTurnStats,
        UserIdleWarning,
        LLMNoResponse,
        InteractionCreated,
        ServerResponse,
        FinalUserTranscription,
        UsageLimitReached,
        SessionEnding,
        Unknown
    };

    // Counter for received blendshape frames (for verification against turn stats)
    int32 ReceivedBlendshapeFrameCount = 0;

    // For delayed frame count verification
    int32 ExpectedBlendshapeFrameCount = 0;
    int32 FramesAtTurnStatsReceived = 0;
    constexpr float BlendshapeVerificationDelaySeconds = 2.0f;
    double LastBlendshapeFrameTime = 0.0;
    std::atomic<bool> bDelayedVerificationPending{false};

    // [MicWatchdog] thresholds, int16 scale. Mic noise floors vary wildly (a
    // Bluetooth headset idles at RMS 1-5, a laptop array with AGC at ~1000), so
    // speech is judged against a tracked floor: well above it, and never below
    // 800. The server's VAD stop is ~2.2 s; 2.5 s of quiet gives its answer
    // time to arrive before the track is judged unheard. A user_text_message
    // makes the server synthesize user-started-speaking and a final
    // user-transcription within about a second, so acknowledgements that close
    // to a text send prove nothing. Chunks are 10 ms.
    constexpr double MicWatchdogSpeechRms = 800.0;
    constexpr double MicWatchdogSpeechOverFloor = 3.0;
    constexpr double MicWatchdogFloorRisePerChunk = 1.001;
    constexpr double MicWatchdogMinSpeechSeconds = 2.0;
    constexpr double MicWatchdogSilenceSeconds = 2.5;
    constexpr double MicWatchdogTextEchoSeconds = 3.0;
    constexpr double MicWatchdogEchoTailSeconds = 1.5;
    constexpr int64 MicWatchdogMinSpeechFrames =
        static_cast<int64>(MicWatchdogMinSpeechSeconds * static_cast<double>(ConvaiConstants::WebRTCAudioSampleRate));

    EC_PacketType ToPacketType(const FString& In) noexcept
    {
        if (In == TEXT("user-started-speaking"))   return EC_PacketType::UserStartedSpeaking;
        if (In == TEXT("user-stopped-speaking"))   return EC_PacketType::UserStoppedSpeaking;
        if (In == TEXT("user-transcription"))      return EC_PacketType::UserTranscription;
        // Sent at the top level as well as inside a server-message, and it
        // repeats the user-transcription that carries final:true. Recognised so
        // it stops being reported as an unknown packet on every utterance.
        if (In == TEXT("final-user-transcription")) return EC_PacketType::FinalUserTranscription;
        if (In == TEXT("bot-llm-started"))         return EC_PacketType::BotLLMStarted;
        if (In == TEXT("bot-llm-stopped"))         return EC_PacketType::BotLLMStopped;
        if (In == TEXT("bot-started-speaking"))    return EC_PacketType::BotStartedSpeaking;
        if (In == TEXT("bot-stopped-speaking"))    return EC_PacketType::BotStoppedSpeaking;
        if (In == TEXT("bot-turn-completed"))      return EC_PacketType::BotTurnCompleted;
        if (In == TEXT("bot-transcription"))       return EC_PacketType::BotTranscription;
        if (In == TEXT("server-message"))          return EC_PacketType::ServerMessage;
        if (In == TEXT("bot-ready"))               return EC_PacketType::BotReady;
        if (In == TEXT("bot-llm-text"))            return EC_PacketType::BotLLMText;
        if (In == TEXT("user-llm-text"))           return EC_PacketType::UserLLMText;
        if (In == TEXT("bot-tts-started"))         return EC_PacketType::BotTTSStarted;
        if (In == TEXT("bot-tts-stopped"))         return EC_PacketType::BotTTSStopped;
        if (In == TEXT("bot-tts-text"))            return EC_PacketType::BotTTSText;
        // Per-word spoken progress, several per sentence. Nothing here consumes it yet.
        if (In == TEXT("bot-output"))              return EC_PacketType::BotOutput;
        if (In == TEXT("llm-function-call-started") ||
            In == TEXT("llm-function-call-in-progress") ||
            In == TEXT("llm-function-call-stopped"))    return EC_PacketType::LLMFunctionCallLifecycle;
        if (In == TEXT("error-response"))           return EC_PacketType::ErrorResponse;
        if (In == TEXT("error"))                    return EC_PacketType::Error;
        return EC_PacketType::Unknown;
    }

    EC_ServerPacketType ToServerPacketType(const FString& In) noexcept
    {
        if (In == TEXT("bot-emotion"))                     return EC_ServerPacketType::BotEmotion;
        if (In == TEXT("action-response"))                 return EC_ServerPacketType::ActionResponse;
        if (In == TEXT("behavior-tree-response"))          return EC_ServerPacketType::BTResponse;
        if (In == TEXT("moderation-response"))             return EC_ServerPacketType::ModerationResponse;
        if (In == TEXT("visemes"))                         return EC_ServerPacketType::Visemes;
        if (In == TEXT("neurosync-blendshapes"))           return EC_ServerPacketType::NeurosyncBlendshapes;
        if (In == TEXT("chunked-neurosync-blendshapes"))   return EC_ServerPacketType::ChunkedNeurosyncBlendshapes;
        if (In == TEXT("blendshape-turn-stats"))           return EC_ServerPacketType::BlendshapeTurnStats;
        if (In == TEXT("user-idle-warning"))               return EC_ServerPacketType::UserIdleWarning;
        if (In == TEXT("llm-no-response"))                 return EC_ServerPacketType::LLMNoResponse;
        if (In == TEXT("interaction-created"))             return EC_ServerPacketType::InteractionCreated;
        if (In == TEXT("server-response"))                 return EC_ServerPacketType::ServerResponse;
        if (In == TEXT("final-user-transcription"))        return EC_ServerPacketType::FinalUserTranscription;
        if (In == TEXT("usage-limit-reached"))             return EC_ServerPacketType::UsageLimitReached;
        if (In == TEXT("session-ending"))                  return EC_ServerPacketType::SessionEnding;
        return EC_ServerPacketType::Unknown;
    }

    // Helper function to convert server viseme data to FAnimationSequence
    void ConvertVisemeDataToAnimationSequence(const TSharedPtr<FJsonObject>& VisemeDataObj, FAnimationSequence& OutAnimationSequence) noexcept
    {
        // Clear any existing data
        OutAnimationSequence.AnimationFrames.Empty();
        OutAnimationSequence.Duration = 0.0f;
        OutAnimationSequence.FrameRate = 0;
        
        if (!VisemeDataObj.IsValid())
        {
            return;
        }
        
        // Get the visemes object from the data
        const TSharedPtr<FJsonObject>* VisemesObj;
        if (!VisemeDataObj->TryGetObjectField(TEXT("visemes"), VisemesObj) || !VisemesObj->IsValid())
        {
            return;
        }
        
        // Create a single animation frame
        FAnimationFrame AnimationFrame;
        AnimationFrame.FrameIndex = 0;
        
        // Map server viseme names to expected names and extract values
        const TMap<FString, FString> VisemeNameMapping = {
            {TEXT("sil"), TEXT("sil")},
            {TEXT("pp"), TEXT("PP")},
            {TEXT("ff"), TEXT("FF")},
            {TEXT("th"), TEXT("TH")},
            {TEXT("dd"), TEXT("DD")},
            {TEXT("kk"), TEXT("kk")},
            {TEXT("ch"), TEXT("CH")},
            {TEXT("ss"), TEXT("SS")},
            {TEXT("nn"), TEXT("nn")},
            {TEXT("rr"), TEXT("RR")},
            {TEXT("aa"), TEXT("aa")},
            {TEXT("e"), TEXT("E")},
            {TEXT("ih"), TEXT("ih")},
            {TEXT("oh"), TEXT("oh")},
            {TEXT("ou"), TEXT("ou")}
        };
        
        // Initialize all visemes to 0
        for (const FString& VisemeName : ConvaiConstants::VisemeNames)
        {
            AnimationFrame.BlendShapes.Add(*VisemeName, 0.0f);
        }
        
        // Extract viseme values from server data
        for (const auto& Mapping : VisemeNameMapping)
        {
            double VisemeValue = 0.0;
            if ((*VisemesObj)->TryGetNumberField(Mapping.Key, VisemeValue))
            {
                // Clamp values between 0 and 1
                float ClampedValue = FMath::Clamp(static_cast<float>(VisemeValue), 0.0f, 1.0f);
                AnimationFrame.BlendShapes[*Mapping.Value] = ClampedValue;
            }
        }
        
        // Add the frame to the sequence
        OutAnimationSequence.AnimationFrames.Add(AnimationFrame);
        OutAnimationSequence.Duration = 0.01f; // Short duration for real-time visemes
        OutAnimationSequence.FrameRate = 100; // 100 FPS for real-time updates
    }

    // Helper function to convert a single blendshape values array to FAnimationFrame
    void ConvertBlendshapeValuesToFrame(const TArray<TSharedPtr<FJsonValue>>& BlendshapeValues, FAnimationFrame& OutFrame, int32 FrameIndex) noexcept
    {
        OutFrame.FrameIndex = FrameIndex;
        OutFrame.BlendShapes.Empty();

        const int32 NumBlendshapes = BlendshapeValues.Num();
        OutFrame.BlendShapes.Reserve(NumBlendshapes);
		const TArray<FString> BlendshapeNames = NumBlendshapes < 100 ? ConvaiConstants::ARKitBlendShapesNames : ConvaiConstants::MetaHumanCtrlNames;

        for (int32 Index = 0; Index < NumBlendshapes; ++Index)
        {
            const TSharedPtr<FJsonValue>& JsonValue = BlendshapeValues[Index];
            if (JsonValue.IsValid())
            {
                const float BlendshapeValue = static_cast<float>(JsonValue->AsNumber());
                const FString& CtrlName = BlendshapeNames[Index];
                OutFrame.BlendShapes.Add(*CtrlName, BlendshapeValue);
            }
        }
    }

    // Helper function to convert server neurosync blendshape data to FAnimationSequence
    // Supports both single frame format (array of floats) and chunked format (array of arrays)
    void ConvertBlendshapeDataToAnimationSequence(const TSharedPtr<FJsonObject>& BlendshapeDataObj, FAnimationSequence& OutAnimationSequence, bool bIsChunked = false) noexcept
    {
        // Clear any existing data
        OutAnimationSequence.AnimationFrames.Empty();
        OutAnimationSequence.Duration = 0.0f;
        OutAnimationSequence.FrameRate = 0;
        
        if (!BlendshapeDataObj.IsValid())
        {
            return;
        }
        
        // Get the blendshapes array from the data
        const TArray<TSharedPtr<FJsonValue>>* BlendshapesArray;
        if (!BlendshapeDataObj->TryGetArrayField(TEXT("blendshapes"), BlendshapesArray) || BlendshapesArray == nullptr || BlendshapesArray->Num() == 0)
        {
            return;
        }

        if (bIsChunked)
        {
            // Chunked format: array of arrays - each inner array is a frame
            OutAnimationSequence.AnimationFrames.Reserve(BlendshapesArray->Num());
            for (int32 FrameIndex = 0; FrameIndex < BlendshapesArray->Num(); ++FrameIndex)
            {
                const TSharedPtr<FJsonValue>& FrameValue = (*BlendshapesArray)[FrameIndex];
                if (FrameValue.IsValid() && FrameValue->Type == EJson::Array)
                {
                    const TArray<TSharedPtr<FJsonValue>>& FrameBlendshapes = FrameValue->AsArray();
                    FAnimationFrame AnimationFrame;
                    ConvertBlendshapeValuesToFrame(FrameBlendshapes, AnimationFrame, FrameIndex);
                    OutAnimationSequence.AnimationFrames.Add(AnimationFrame);
                }
            }

            // Set duration based on number of frames at 60 FPS
            const int32 NumFrames = OutAnimationSequence.AnimationFrames.Num();
            OutAnimationSequence.FrameRate = 60;
            OutAnimationSequence.Duration = NumFrames > 0 ? static_cast<float>(NumFrames) / 60.0f : 0.0f;
        }
        else
        {
            // Single frame format: array of floats
            FAnimationFrame AnimationFrame;
            ConvertBlendshapeValuesToFrame(*BlendshapesArray, AnimationFrame, 0);
            OutAnimationSequence.AnimationFrames.Add(AnimationFrame);
            OutAnimationSequence.Duration = 1.0f / 60.0f; // Short duration for real-time blendshapes
            OutAnimationSequence.FrameRate = 60; // 60 FPS for real-time updates
        }
    }
    
    TSharedPtr<FJsonObject> ParseJsonObject(const FString& JsonStr) noexcept
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return nullptr;
        }
        return Root;
    }

    bool GetStringSafe(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FString& Out) noexcept
    {
        Out.Reset();
        return Obj.IsValid() && Obj->TryGetStringField(Field, Out);
    }

    const char* ContextUpdateModeToString(EC_ContextUpdateMode Mode) noexcept
    {
        switch (Mode)
        {
            case EC_ContextUpdateMode::Append:  return "append";
            case EC_ContextUpdateMode::Replace: return "replace";
            case EC_ContextUpdateMode::Reset:   return "reset";
            default:                            return "append";
        }
    }

    const char* RunLLMOptionToString(EC_RunLLMOption Option) noexcept
    {
        switch (Option)
        {
            case EC_RunLLMOption::Auto:   return "auto";
            case EC_RunLLMOption::Always: return "true";
            case EC_RunLLMOption::Never:  return "false";
            default:                      return "auto";
        }
    }

    bool GetBoolSafe(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, bool& Out) noexcept
    {
        Out = false;
        return Obj.IsValid() && Obj->TryGetBoolField(Field, Out);
    }

    bool GetNumberSafe(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double& Out) noexcept
    {
        Out = 0.0;
        return Obj.IsValid() && Obj->TryGetNumberField(Field, Out);
    }

    // Handy extractor for the "data" object.
    TSharedPtr<FJsonObject> GetDataObject(const TSharedPtr<FJsonObject>& Root) noexcept
    {
        const TSharedPtr<FJsonObject>* DataObjPtr = nullptr;
        return (Root.IsValid() && Root->TryGetObjectField(TEXT("data"), DataObjPtr)) ? *DataObjPtr : nullptr;
    }

#if WITH_TESTS
    // One process, one connect seam: a unit rig installs a hook, an engine
    // scenario arms failures through the console. Both are game-thread only.
    FConvaiConnectionTestSeam::FAttemptHook GTestAttemptHook;
    FConvaiConnectionTestSeam::FSendHook GTestSendHook;
    int32 GTestAttemptCount = 0;
    int32 GTestPendingFailures = 0;
#endif


    struct FRequestState : public TSharedFromThis<FRequestState>
    {
        FString ResponseBody;
        int32   StatusCode = 0;
        bool    bSuccess   = false;

        // Completion signalling
        FEvent* DoneEvent  = nullptr;
        FThreadSafeBool bCompleted = false;

        FRequestState()
        {
            DoneEvent = FPlatformProcess::GetSynchEventFromPool();
        }

        ~FRequestState()
        {
            if (DoneEvent)
            {
                FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
                DoneEvent = nullptr;
            }
        }

        void Complete(bool bInSuccess, int32 InStatus, const FString& InBody)
        {
            if (bCompleted) return; // guard against double-complete (e.g., cancel + callback)
            bSuccess   = bInSuccess;
            StatusCode = InStatus;
            ResponseBody = InBody;
            bCompleted = true;
            if (DoneEvent) DoneEvent->Trigger();
        }
    };
} // anonymous namespace

// Connection Thread Implementation
FConvaiConnectionThread::FConvaiConnectionThread(const FConvaiConnectionParams& InConnectionParams)
    : ConnectionParams(InConnectionParams)
    , bShouldStop(false)
    , Thread(nullptr)
{
    InitializeThread();
}

FConvaiConnectionThread::FConvaiConnectionThread(FConvaiConnectionParams&& InConnectionParams)
    : ConnectionParams(MoveTemp(InConnectionParams))
    , bShouldStop(false)
    , Thread(nullptr)
{
    InitializeThread();
}

void FConvaiConnectionThread::InitializeThread()
{
    Thread = FRunnableThread::Create(this, TEXT("ConvaiConnectionThread"), 0, TPri_Normal);
    if (!Thread)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to create ConvaiConnectionThread"));
        bShouldStop = true;
    }
}

FConvaiConnectionThread::~FConvaiConnectionThread()
{
    FConvaiConnectionThread::Stop();
    if (Thread)
    {
        Thread->Kill();
        delete Thread;
        Thread = nullptr;
    }
}

uint32 FConvaiConnectionThread::Run()
{
    if (ConnectionParams.Client)
    {
        convai::ConvaiAECConfig Config;
        
        // Set AEC type
        const FString AECTypeStr = UConvaiUtils::GetAECType();
        if (AECTypeStr.Equals(TEXT("External"), ESearchCase::IgnoreCase))
        {
            Config.aec_type = convai::AECType::External;
        }
        else if (AECTypeStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
        {
            Config.aec_type = convai::AECType::None;
        }
        else // Default to Internal
        {
            Config.aec_type = convai::AECType::Internal;
        }
        
        // Common settings
        Config.aec_enabled = UConvaiUtils::IsAECEnabled();
        Config.noise_suppression_enabled = UConvaiUtils::IsNoiseSuppressionEnabled();
        Config.gain_control_enabled = UConvaiUtils::IsGainControlEnabled();
        
        // WebRTC AEC specific settings
        Config.vad_enabled = UConvaiUtils::IsVADEnabled();
        Config.vad_mode = UConvaiUtils::GetVADMode();
        
        // Core AEC specific settings
        Config.high_pass_filter_enabled = UConvaiUtils::IsHighPassFilterEnabled();
        
        // Audio settings
        Config.sample_rate = ConvaiConstants::WebRTCAudioSampleRate;
        
        if (!ConnectionParams.Client->Initialize(Config))
        {
            ConnectionParams.ReportAttemptFailed();
            CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to Initialize client"));
            return 1;
        }

        // Unset means never call, not zero: a hint of 0 ms is itself a value
        // worth sweeping, so it cannot share a sentinel with "no hint".
        if (const FString StreamDelayStr = UConvaiUtils::GetAECStreamDelayMs(); !StreamDelayStr.IsEmpty())
        {
            const int32 StreamDelayMs = FCString::Atoi(*StreamDelayStr);
#if CONVAI_HAS_AEC_STATS_API
            const bool bAccepted = ConnectionParams.Client->SetStreamDelay(StreamDelayMs);
            CONVAI_LOG(ConvaiSubsystemLog, Display, TEXT("SetStreamDelay(%d ms) -> %s"),
                       StreamDelayMs, bAccepted ? TEXT("accepted") : TEXT("rejected"));
#else
            CONVAI_LOG(ConvaiSubsystemLog, Warning,
                       TEXT("SetStreamDelay(%d ms) ignored: convai_client binaries predate the API"),
                       StreamDelayMs);
#endif
        }
        
        if (bShouldStop || IsEngineExitRequested())
        {
            return 0;
        }

        const TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
        //x-api-key
        // Get connection parameters - read from ConnectionParams
        const FString StreamURLString = UConvaiUtils::GetStreamURL();
        FString AuthKeyHeader = AuthHeaderAndKey.Key;
        const FString AuthKeyValue = AuthHeaderAndKey.Value;

        if (ConvaiConstants::API_Key_Header == AuthKeyHeader)
        {
            AuthKeyHeader = TEXT("X-API-KEY");
        }
        
        // Keep each UTF-8 string in owned, dynamically sized storage until Connect
        // returns. In particular, action_config grows with scene-object count and
        // must not be constrained by the former 4 KiB metadata scratch buffer.
        const TArray<ANSICHAR> StreamURLUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(StreamURLString);
        const TArray<ANSICHAR> AuthKeyHeaderUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(AuthKeyHeader);
        const TArray<ANSICHAR> AuthKeyValueUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(AuthKeyValue);
        const TArray<ANSICHAR> CharIDUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(ConnectionParams.CharacterID);
        const TArray<ANSICHAR> ConnectionTypeUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(ConnectionParams.ConnectionType);
        const TArray<ANSICHAR> LLMProviderUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(ConnectionParams.LLMProvider);
        const TArray<ANSICHAR> BlendshapeProviderUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(ConnectionParams.BlendshapeProvider);
        const TArray<ANSICHAR> BlendshapeFormatUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(ConnectionParams.BlendshapeFormat);
        const TArray<ANSICHAR> EmotionProviderUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(ConnectionParams.EmotionProvider);
        const TArray<ANSICHAR> EndUserIDUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(ConnectionParams.EndUserID);
        const TArray<ANSICHAR> EndUserMetadataUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(ConnectionParams.EndUserMetadata);
        const TArray<ANSICHAR> ActionConfigUtf8 = ConvaiUtf8::ConvertToNullTerminatedBytes(ConnectionParams.ActionConfigJson);

        // Log connection parameters
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Connecting to Convai service with parameters:"));
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("StreamURL: %s"), *StreamURLString);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("CharacterID: %s"), *ConnectionParams.CharacterID);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("ConnectionType: %s"), *ConnectionParams.ConnectionType);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("LLMProvider: %s"), *ConnectionParams.LLMProvider);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("BlendshapeProvider: %s"), *ConnectionParams.BlendshapeProvider);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("BlendshapeFormat: %s"), *ConnectionParams.BlendshapeFormat);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("EmotionProvider: %s"), *ConnectionParams.EmotionProvider);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("EndUserID: %s"), *ConnectionParams.EndUserID);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("EndUserMetadata: %s"), *ConnectionParams.EndUserMetadata);
        if (!ConnectionParams.ActionConfigJson.IsEmpty())
        {
            CONVAI_LOG(
                ConvaiSubsystemLog,
                Log,
                TEXT("ActionConfig (%d UTF-8 bytes): %s"),
                ActionConfigUtf8.Num() - 1,
                *ConnectionParams.ActionConfigJson);
        }
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("ChunkSize: %d"), ConnectionParams.ChunkSize);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OutputFPS: %d"), ConnectionParams.OutputFPS);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("FramesBufferDuration: %f"), ConnectionParams.FramesBufferDuration);
        
        // Create connection config struct for the new Connect API
        convai::ConvaiConnectionConfig ConvaiConnectionConfig;
        ConvaiConnectionConfig.url = StreamURLUtf8.GetData();
        ConvaiConnectionConfig.auth_value = AuthKeyValueUtf8.GetData();
        ConvaiConnectionConfig.auth_header = AuthKeyHeaderUtf8.GetData();
        ConvaiConnectionConfig.character_id = CharIDUtf8.GetData();
        ConvaiConnectionConfig.connection_type = ConnectionTypeUtf8.GetData();
        ConvaiConnectionConfig.llm_provider = LLMProviderUtf8.GetData();
        ConvaiConnectionConfig.blendshape_provider = BlendshapeProviderUtf8.GetData();
        ConvaiConnectionConfig.blendshape_format = ConnectionParams.BlendshapeFormat.IsEmpty()
            ? nullptr
            : BlendshapeFormatUtf8.GetData();
        ConvaiConnectionConfig.emotion_provider = EmotionProviderUtf8.GetData();
        ConvaiConnectionConfig.end_user_id = EndUserIDUtf8.GetData();
        ConvaiConnectionConfig.end_user_metadata = EndUserMetadataUtf8.GetData();
        ConvaiConnectionConfig.action_config = ConnectionParams.ActionConfigJson.IsEmpty()
            ? nullptr
            : ActionConfigUtf8.GetData();
        ConvaiConnectionConfig.chunk_size = ConnectionParams.ChunkSize;
        ConvaiConnectionConfig.output_fps = ConnectionParams.OutputFPS;
        ConvaiConnectionConfig.frames_buffer_duration = ConnectionParams.FramesBufferDuration;

        convai::ConvaiVadParams VadParams;
        VadParams.confidence = ConnectionParams.VADConfidence;
        VadParams.start_secs = ConnectionParams.VADStartSecs;
        VadParams.stop_secs  = ConnectionParams.VADStopSecs;
        VadParams.min_volume = ConnectionParams.VADMinVolume;
        const bool bAnyVadOverride =
            ConnectionParams.VADConfidence >= 0.0f ||
            ConnectionParams.VADStartSecs  >= 0.0f ||
            ConnectionParams.VADStopSecs   >= 0.0f ||
            ConnectionParams.VADMinVolume  >= 0.0f;
        ConvaiConnectionConfig.vad_params = bAnyVadOverride ? &VadParams : nullptr;
        if (bAnyVadOverride)
        {
            CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("VAD overrides — confidence: %f, start_secs: %f, stop_secs: %f, min_volume: %f"),
                VadParams.confidence, VadParams.start_secs, VadParams.stop_secs, VadParams.min_volume);
        }

        // Build invocation metadata
        bool bPluginFound = false;
        FString PluginVersionName, PluginEngineVersion, PluginFriendlyName;
        UConvaiUtils::GetPluginInfo(TEXT("ConvAI"), bPluginFound, PluginVersionName, PluginEngineVersion, PluginFriendlyName);

        FString PlatformName, EngineVersionStr;
        UConvaiUtils::GetPlatformInfo(EngineVersionStr, PlatformName);

        const FString AppName = FApp::GetProjectName();
        const FString AppClientVersionStr = UConvaiUtils::GetClientVersion();
        const FString ConvaiClientVersionStr = UTF8_TO_TCHAR(convai::GetConvaiClientVersion());
        const FString ClientVersionOverride =
            UConvaiUtils::GetCustomParam(TEXT("ClientVersion"));
        const FString InvocationClientVersionStr = ClientVersionOverride.IsEmpty()
            ? AppClientVersionStr
            : ClientVersionOverride;

        TSharedPtr<FJsonObject> ExtraMetadataJson = MakeShared<FJsonObject>();
        ExtraMetadataJson->SetStringField(TEXT("convai_plugin_version"), PluginVersionName);
        ExtraMetadataJson->SetStringField(TEXT("platform"), PlatformName);
        ExtraMetadataJson->SetStringField(TEXT("ue_version"), EngineVersionStr);
        ExtraMetadataJson->SetStringField(TEXT("app_name"), AppName);
        ExtraMetadataJson->SetStringField(TEXT("app_client_version"), AppClientVersionStr);
        ExtraMetadataJson->SetStringField(TEXT("convai_client_version"), ConvaiClientVersionStr);

        FString ExtraMetadataString;
        TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&ExtraMetadataString);
        FJsonSerializer::Serialize(ExtraMetadataJson.ToSharedRef(), JsonWriter);

        const TArray<ANSICHAR> InvocationSourceUtf8 =
            ConvaiUtf8::ConvertToNullTerminatedBytes(TEXT("ue_sdk"));
        const TArray<ANSICHAR> InvocationClientVersionUtf8 =
            ConvaiUtf8::ConvertToNullTerminatedBytes(InvocationClientVersionStr);
        const TArray<ANSICHAR> InvocationExtraMetadataUtf8 =
            ConvaiUtf8::ConvertToNullTerminatedBytes(ExtraMetadataString);

        convai::ConvaiInvocationMetadata InvocationMetadata;
        InvocationMetadata.source = InvocationSourceUtf8.GetData();
        InvocationMetadata.client_version = InvocationClientVersionUtf8.GetData();
        InvocationMetadata.extra_metadata = InvocationExtraMetadataUtf8.GetData();
        ConvaiConnectionConfig.invocation_metadata = &InvocationMetadata;

        CONVAI_LOG(
            ConvaiSubsystemLog,
            Log,
            TEXT("InvocationMetadata - ClientVersion: %s | NativeClientVersion: %s | AppClientVersion: %s | ExtraMetadata: %s"),
            *InvocationClientVersionStr,
            *ConvaiClientVersionStr,
            *AppClientVersionStr,
            *ExtraMetadataString);

        // The wait the game thread used to do, moved to the thread nobody is
        // watching: the old server session has to close before this POST, or
        // the two race for the account's concurrency slot. Polled rather than
        // blocked, so Stop() still ends this thread promptly - including when
        // the thread being killed is the one waiting.
        {
            const double WaitDeadline = FPlatformTime::Seconds() + 35.0;
            while (FConvaiConnectReaper::HasPendingFor(ConnectionParams.CharacterID))
            {
                if (bShouldStop || IsEngineExitRequested())
                {
                    return 0;
                }
                if (FPlatformTime::Seconds() >= WaitDeadline)
                {
                    CONVAI_LOG(ConvaiSubsystemLog, Warning,
                        TEXT("Connecting without waiting any longer for the previous session of [%s] to close"),
                        *ConnectionParams.CharacterID);
                    break;
                }
                FPlatformProcess::Sleep(0.05f);
            }
        }
        if (bShouldStop || IsEngineExitRequested())
        {
            return 0;
        }

        if (!ConnectionParams.Client->Connect(ConvaiConnectionConfig))
        {
            ConnectionParams.ReportAttemptFailed();
            CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to connect to Convai service"));
            return 2;
        }
    }
    else
    {
        ConnectionParams.ReportAttemptFailed();
        CONVAI_LOG(LogTemp, Error, TEXT("Client pointer is null; cannot Connect."));
        return 3;
    }
    
    return 0;
}

// Convai Subsystem Implementation
UConvaiSubsystem::UConvaiSubsystem()
    : bIsConnected(false)
    , bStartedPublishingVideo(false)
    , CurrentConnectionState(EC_ConnectionState::Disconnected)
    , CurrentCharacterSession(nullptr)
    , CurrentPlayerSession(nullptr)
{
}

void UConvaiSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    InitializeReconnectCoordinator();

    ConnectionManager = NewObject<UConvaiConnectionManager>(this);
    ConnectionManager->Initialize(this);

#if ConvaiDebugMode
    ResetLatencyStatistics();
#endif
}

void UConvaiSubsystem::Deinitialize()
{
    // Nothing may fire after this: a ticker that outlives the subsystem would
    // reconnect a session whose world is gone.
    ReconnectCoordinator.Shutdown();
    for (TPair<int32, FScheduledReconnect>& Entry : ScheduledReconnects)
    {
        FTSTicker::GetCoreTicker().RemoveTicker(Entry.Value.Ticker);
    }
    ScheduledReconnects.Reset();

    // The poll clock lives on UConvaiContextSubsystem now; stop it from here too
    // in case this subsystem tears down first (it also stops itself on its own
    // Deinitialize).
    if (UConvaiContextSubsystem* ContextSubsystem = GetContextSubsystem())
    {
        ContextSubsystem->StopObjectPollClock();
    }
    StopClientReadyHandshake();
    FTSTicker::GetCoreTicker().RemoveTicker(MicWatchdogTicker);
    MicWatchdogTicker.Reset();

    if (ConnectionManager)
    {
        ConnectionManager->Shutdown();
    }

    CleanupConvaiClient();

    // Defer ConvaiClient destruction to a background thread so internal WebRTC
    // threads have time to finish shutting down after Disconnect().
    {
        FScopeLock ClientLock(&ConvaiClientMutex);
        if (ConvaiClient)
        {
            TSharedPtr<convai::ConvaiClient> OldClient(ConvaiClient.Release());
            AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [OldClient]()
            {
                // Wait for internal WebRTC threads to finish shutting down
                // before the destructor runs when OldClient goes out of scope
                FPlatformProcess::Sleep(2.0f);
            });
        }
    }

    Super::Deinitialize();
}

#if ConvaiDebugMode
void UConvaiSubsystem::ResetLatencyStatistics()
{
    LatencySamples.Empty();
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[Latency] Statistics reset"));
}

void UConvaiSubsystem::RecordLatency(double LatencyMs)
{
    LatencySamples.Add(LatencyMs);
}

void UConvaiSubsystem::PrintLatencyStatistics() const
{
    if (LatencySamples.Num() == 0)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[Latency] No samples recorded"));
        return;
    }

    // Create a sorted copy for percentile calculations
    TArray<double> SortedSamples = LatencySamples;
    SortedSamples.Sort();

    const int32 NumSamples = SortedSamples.Num();

    // Get actual min and max
    const double ActualMin = SortedSamples[0];
    const double ActualMax = SortedSamples[NumSamples - 1];

    // Calculate mean
    double Sum = 0.0;
    for (double Sample : SortedSamples)
    {
        Sum += Sample;
    }
    const double Mean = Sum / NumSamples;

    // Calculate median
    double Median;
    if (NumSamples % 2 == 0)
    {
        Median = (SortedSamples[NumSamples / 2 - 1] + SortedSamples[NumSamples / 2]) / 2.0;
    }
    else
    {
        Median = SortedSamples[NumSamples / 2];
    }

    // Calculate Q1 (25th percentile)
    const int32 Q1Index = NumSamples / 4;
    const double Q1 = SortedSamples[Q1Index];

    // Calculate Q3 (75th percentile)
    const int32 Q3Index = (NumSamples * 3) / 4;
    const double Q3 = SortedSamples[Q3Index];

    // Calculate IQR and adjusted min/max
    const double IQR = Q3 - Q1;
    double AdjustedMin = Q1 - (1.5 * IQR);
    double AdjustedMax = Q3 + (1.5 * IQR);

    // Cap adjusted min/max by actual min/max
    AdjustedMin = FMath::Max(AdjustedMin, ActualMin);
    AdjustedMax = FMath::Min(AdjustedMax, ActualMax);

    // Print all statistics in one line
    CONVAI_LOG(ConvaiSubsystemLog, Log,
        TEXT("[Latency] Stats: Trials=%d | Min=%.2fms | Max=%.2fms | Mean=%.2fms | Median=%.2fms | Q1=%.2fms | Q3=%.2fms | AdjMin=%.2fms | AdjMax=%.2fms"),
        NumSamples, ActualMin, ActualMax, Mean, Median, Q1, Q3, AdjustedMin, AdjustedMax);
}
#endif

void UConvaiSubsystem::RegisterChatbotComponent(UConvaiChatbotComponent* ChatbotComponent)
{
    if (ChatbotComponent && !RegisteredChatbotComponents.Contains(ChatbotComponent))
    {
        RegisteredChatbotComponents.Add(ChatbotComponent);
        // Chatbots are spatial-awareness observers, so the poll clock must run
        // for them even when there are no object components in the level.
        if (UConvaiContextSubsystem* ContextSubsystem = GetContextSubsystem())
        {
            ContextSubsystem->RestartObjectPollClock();
        }
    }
}

void UConvaiSubsystem::UnregisterChatbotComponent(UConvaiChatbotComponent* ChatbotComponent)
{
    if (RegisteredChatbotComponents.Contains(ChatbotComponent))
    {
        ChatbotComponent->StopSession();
        RegisteredChatbotComponents.Remove(ChatbotComponent);
        // Stop the shared clock only when nothing is left to drive it.
        if (RegisteredChatbotComponents.Num() == 0 && RegisteredObjectComponents.Num() == 0)
        {
            if (UConvaiContextSubsystem* ContextSubsystem = GetContextSubsystem())
            {
                ContextSubsystem->StopObjectPollClock();
            }
        }
    }
}

TArray<UConvaiChatbotComponent*> UConvaiSubsystem::GetAllChatbotComponents() const
{
    return RegisteredChatbotComponents;
}

void UConvaiSubsystem::RegisterPlayerComponent(UConvaiPlayerComponent* PlayerComponent)
{
    if (PlayerComponent && !RegisteredPlayerComponents.Contains(PlayerComponent))
    {
        RegisteredPlayerComponents.Add(PlayerComponent);
        CONVAI_LOG(ConvaiSubsystemLog, Verbose, TEXT("Registered player component: %s"), *PlayerComponent->GetName());
    }
}

void UConvaiSubsystem::UnregisterPlayerComponent(UConvaiPlayerComponent* PlayerComponent)
{
    if (RegisteredPlayerComponents.Contains(PlayerComponent))
    {
        PlayerComponent->StopSession();
        RegisteredPlayerComponents.Remove(PlayerComponent);
        CONVAI_LOG(ConvaiSubsystemLog, Verbose, TEXT("Unregistered player component: %s"), *PlayerComponent->GetName());
    }
}

TArray<UConvaiPlayerComponent*> UConvaiSubsystem::GetAllPlayerComponents() const
{
    return RegisteredPlayerComponents;
}

// ─────────────────────────────────────────────────────────────────────────────
// UConvaiObjectComponent registry + shared poll clock
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
    // The disambiguation suffix for the Nth duplicate of a name (N starts at 2 for
    // the first collision). Plain leading space so TTS reads "Crate two" / "Crate A"
    // naturally rather than "Crate underscore 2".
    FString MakeNameSuffix(EConvaiObjectNameSuffixStyle Style, int32 N)
    {
        if (Style == EConvaiObjectNameSuffixStyle::Alphabetical)
        {
            // N=2 -> "A", N=3 -> "B", ... N=27 -> "Z", N=28 -> "AA" (bijective base-26).
            FString Suffix;
            int32 V = N - 2;
            do
            {
                Suffix = FString::Chr(static_cast<TCHAR>('A' + (V % 26))) + Suffix;
                V = V / 26 - 1;
            } while (V >= 0);
            return FString::Printf(TEXT(" %s"), *Suffix);
        }
        return FString::Printf(TEXT(" %d"), N);
    }
}

void UConvaiSubsystem::RegisterObjectComponent(UConvaiObjectComponent* ObjectComponent)
{
    if (!ObjectComponent || RegisteredObjectComponents.Contains(ObjectComponent))
    {
        return;
    }

    // Light normalization at the single entry point: trim + collapse
    // whitespace so "  Pressure   Plate " and "Pressure Plate" are one
    // identity. Casing is deliberately preserved — AI-facing names are
    // prompt and SPEECH text, never PascalCased like state keys (the model
    // would say "DoorOtherSide" out loud, and the action resolver's
    // word-window matching is built for multi-word names).
    ObjectComponent->ObjectEntry.Name =
        FConvaiObjectEntry::NormalizeMovementPointName(ObjectComponent->ObjectEntry.Name);

    // Refuse empty names. Chatbots have nothing to broadcast under "" anyway, and
    // unregistered components don't bloat BP GetAll... results.
    if (ObjectComponent->ObjectEntry.Name.IsEmpty())
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning,
            TEXT("ConvaiObjectComponent on %s skipped: ObjectEntry.Name is empty"),
            ObjectComponent->GetOwner() ? *ObjectComponent->GetOwner()->GetName() : TEXT("<no owner>"));
        return;
    }

    // Assign this object's final, unique Name. Every LOGICAL object gets a distinct
    // name: the first to register keeps its authored ("base") name and later duplicates
    // get a suffix in the configured style ("Crate 2"/"Crate 3" or "Crate A"/"Crate B").
    // Case-insensitive so "Door"/"door" don't both reach the LLM.
    //
    // Merge sets are the exception: a component that opts into merging joins any
    // already-registered sibling sharing its base name AND merge group index, adopting
    // that sibling's (possibly already-suffixed) name. The whole set thus shares ONE
    // name and is treated as a single logical object everywhere downstream (which keys
    // purely on the final name). Already-broadcast context-state keys stay stable.
    // Use the authored base name. On a re-registration the component's
    // ObjectEntry.Name may already carry a suffix from a prior pass, so prefer the
    // remembered base (set on first registration) to avoid double-suffixing.
    const FString BaseName = ObjectComponent->RegisteredBaseName.IsEmpty()
        ? ObjectComponent->ObjectEntry.Name
        : ObjectComponent->RegisteredBaseName;
    ObjectComponent->RegisteredBaseName = BaseName;

    // Registered settings instance (the one Project Settings edits), not GetDefault<>()/the
    // CDO — see ConvaiContextSubsystem::EvaluateSpatialAwareness for why the CDO is wrong.
    const UConvaiSettings* Settings = Convai::Get().GetConvaiSettings();
    const EConvaiObjectNameSuffixStyle SuffixStyle =
        Settings ? Settings->ObjectNameSuffixStyle : EConvaiObjectNameSuffixStyle::Numeric;

    FString FinalName;
    bool bJoinedMergeSet = false;

    if (ObjectComponent->bMergeWithSameNamedObjects)
    {
        for (UConvaiObjectComponent* Other : RegisteredObjectComponents)
        {
            if (!IsValid(Other)) { continue; }
            if (Other->bMergeWithSameNamedObjects
                && Other->MergeGroupIndex == ObjectComponent->MergeGroupIndex
                && Other->RegisteredBaseName.Equals(BaseName, ESearchCase::IgnoreCase))
            {
                FinalName = Other->ObjectEntry.Name; // adopt the merge set's shared name
                bJoinedMergeSet = true;
                break;
            }
        }
    }

    if (!bJoinedMergeSet)
    {
        auto IsNameTaken = [&](const FString& Candidate) -> bool
        {
            for (UConvaiObjectComponent* Other : RegisteredObjectComponents)
            {
                if (IsValid(Other) && Other->ObjectEntry.Name.Equals(Candidate, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
            return false;
        };

        FinalName = BaseName;
        int32 SuffixN = 2;
        while (IsNameTaken(FinalName))
        {
            FinalName = BaseName + MakeNameSuffix(SuffixStyle, SuffixN++);
        }
    }

    if (bJoinedMergeSet)
    {
        if (!FinalName.Equals(BaseName, ESearchCase::CaseSensitive))
        {
            ObjectComponent->ObjectEntry.Name = FinalName;
        }
        CONVAI_LOG(ConvaiSubsystemLog, Log,
            TEXT("ConvaiObjectComponent '%s' (merge group %d) on %s merged into logical object '%s'."),
            *BaseName, ObjectComponent->MergeGroupIndex,
            ObjectComponent->GetOwner() ? *ObjectComponent->GetOwner()->GetName() : TEXT("<no owner>"),
            *FinalName);
    }
    else if (!FinalName.Equals(BaseName, ESearchCase::CaseSensitive))
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning,
            TEXT("ConvaiObjectComponent name '%s' already taken; auto-renamed to '%s' for component on actor %s."),
            *BaseName, *FinalName,
            ObjectComponent->GetOwner() ? *ObjectComponent->GetOwner()->GetName() : TEXT("<no owner>"));
        ObjectComponent->ObjectEntry.Name = FinalName;
    }

    RegisteredObjectComponents.Add(ObjectComponent);
    // First registration -> (re)start the shared clock on the context subsystem.
    if (UConvaiContextSubsystem* ContextSubsystem = GetContextSubsystem())
    {
        ContextSubsystem->RestartObjectPollClock();
    }
}

void UConvaiSubsystem::UnregisterObjectComponent(UConvaiObjectComponent* ObjectComponent)
{
    if (RegisteredObjectComponents.Contains(ObjectComponent))
    {
        RegisteredObjectComponents.Remove(ObjectComponent);
        if (RegisteredObjectComponents.Num() == 0 && RegisteredChatbotComponents.Num() == 0)
        {
            if (UConvaiContextSubsystem* ContextSubsystem = GetContextSubsystem())
            {
                ContextSubsystem->StopObjectPollClock();
            }
        }
    }
}

TArray<UConvaiObjectComponent*> UConvaiSubsystem::GetAllObjectComponents() const
{
    return RegisteredObjectComponents;
}

namespace
{
    // A component's world location: its resolved sub-component if one is set,
    // otherwise its owning actor. Mirrors the spatial-awareness gather so the
    // centroid lines up with where each member is reported.
    FVector ResolvedObjectLocation(UConvaiObjectComponent* Obj)
    {
        if (USceneComponent* Comp = Obj->GetResolvedComponent())
        {
            return Comp->GetComponentLocation();
        }
        if (AActor* Owner = Obj->GetOwner())
        {
            return Owner->GetActorLocation();
        }
        return FVector::ZeroVector;
    }
}

void UConvaiSubsystem::BuildObjectGroups(TArray<FConvaiObjectGroup>& OutGroups) const
{
    // Game-thread only: ResolvedObjectLocation() calls GetResolvedComponent(),
    // which lazily mutates each component's resolve cache.
    check(IsInGameThread());
    OutGroups.Reset();

    // Lower-cased name -> index into OutGroups, so first-appearance registry order is
    // preserved and same-named members append to the existing group. Components that
    // share a name are exactly one logical object: registration gives distinct logical
    // objects distinct names, and merged-set members a shared name.
    TMap<FString, int32> NameToGroup;

    for (UConvaiObjectComponent* Obj : RegisteredObjectComponents)
    {
        if (!IsValid(Obj) || !IsValid(Obj->GetOwner())) { continue; }
        const FString& Name = Obj->ObjectEntry.Name;
        if (Name.IsEmpty()) { continue; }

        const FString Key = Name.ToLower();
        int32 GroupIndex = INDEX_NONE;
        if (int32* Existing = NameToGroup.Find(Key))
        {
            GroupIndex = *Existing;
        }
        else
        {
            GroupIndex = OutGroups.Num();
            NameToGroup.Add(Key, GroupIndex);
            FConvaiObjectGroup NewGroup;
            NewGroup.Name = Name;
            OutGroups.Add(MoveTemp(NewGroup));
        }

        FConvaiObjectGroup& Group = OutGroups[GroupIndex];
        Group.Members.Add(Obj);
        // First non-empty description in registry order wins.
        if (Group.Description.IsEmpty() && !Obj->ObjectEntry.Description.IsEmpty())
        {
            Group.Description = Obj->ObjectEntry.Description;
        }
    }

    // Centroid = mean of member resolved locations.
    for (FConvaiObjectGroup& Group : OutGroups)
    {
        if (Group.Members.Num() == 0) { continue; }
        FVector Sum = FVector::ZeroVector;
        for (UConvaiObjectComponent* Member : Group.Members)
        {
            Sum += ResolvedObjectLocation(Member);
        }
        Group.Centroid = Sum / static_cast<float>(Group.Members.Num());
    }
}

void UConvaiSubsystem::GetObjectGroupMembers(UConvaiObjectComponent* Member,
    TArray<UConvaiObjectComponent*>& OutMembers) const
{
    OutMembers.Reset();
    if (!IsValid(Member)) { return; }

    const FString Key = Member->ObjectEntry.Name;

    if (Key.IsEmpty())
    {
        OutMembers.Add(Member);
        return;
    }

    for (UConvaiObjectComponent* Obj : RegisteredObjectComponents)
    {
        if (IsValid(Obj) && Obj->ObjectEntry.Name.Equals(Key, ESearchCase::IgnoreCase))
        {
            OutMembers.Add(Obj);
        }
    }
    // Always include the queried component, even if it isn't registered (no-op if
    // it already matched above; preserves its registry-order position when present).
    OutMembers.AddUnique(Member);
}

bool UConvaiSubsystem::PollObjectComponents()
{
    for (int32 i = RegisteredObjectComponents.Num() - 1; i >= 0; --i)
    {
        UConvaiObjectComponent* Component = RegisteredObjectComponents[i];
        if (!IsValid(Component))
        {
            RegisteredObjectComponents.RemoveAt(i);
            continue;
        }
        // Skip components whose owning Actor is mid-destruction. The Actor's EndPlay
        // will fire and unregister the component shortly; we just don't want to dereference
        // it from this tick. We continue iterating so other components still get evaluated.
        if (!IsValid(Component->GetOwner()))
        {
            continue;
        }
        Component->EvaluateTrackedProperties();
    }

    // Tell the caller's clock whether to keep ticking.
    return RegisteredObjectComponents.Num() > 0;
}

UConvaiContextSubsystem* UConvaiSubsystem::GetContextSubsystem() const
{
    if (UGameInstance* GI = GetGameInstance())
    {
        return GI->GetSubsystem<UConvaiContextSubsystem>();
    }
    return nullptr;
}

void UConvaiSubsystem::InvalidateOrphanedConnection()
{
    if (ConnectionManager)
    {
        ConnectionManager->InvalidateOrphanedConnection();
    }
}

void UConvaiSubsystem::StartClientReadyHandshake(uint64 Epoch)
{
    StopClientReadyHandshake();

    SendClientReadyMessage();

    // Both carry the epoch they were armed for: a session stopped and
    // restarted inside the timeout must not be timed out by the old one.
    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    ClientReadyRetryTicker = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakThis, Epoch](float) -> bool
        {
            UConvaiSubsystem* Subsystem = WeakThis.Get();
            if (!Subsystem || !Subsystem->IsCurrentEpoch(Epoch))
            {
                return false;
            }
            Subsystem->SendClientReadyMessage();
            return true;
        }),
        UConvaiUtils::GetClientReadyRetrySecs());
    ClientReadyTimeoutTicker = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakThis, Epoch](float) -> bool
        {
            if (UConvaiSubsystem* Subsystem = WeakThis.Get())
            {
                Subsystem->ClientReadyTimeoutTicker.Reset();
                Subsystem->OnClientReadyTimeout(Epoch);
            }
            return false;
        }),
        UConvaiUtils::GetClientReadyTimeoutSecs());
}

void UConvaiSubsystem::StopClientReadyHandshake()
{
    FTSTicker::GetCoreTicker().RemoveTicker(ClientReadyRetryTicker);
    FTSTicker::GetCoreTicker().RemoveTicker(ClientReadyTimeoutTicker);
    ClientReadyRetryTicker.Reset();
    ClientReadyTimeoutTicker.Reset();
}

void UConvaiSubsystem::OnClientReadyTimeout(uint64 Epoch)
{
    if (!IsCurrentEpoch(Epoch) || ReadyEpoch == Epoch)
    {
        return;
    }
    CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("bot-ready not received within %.0fs; stopping client-ready handshake"), UConvaiUtils::GetClientReadyTimeoutSecs());
    StopClientReadyHandshake();

    // A character that never became ready is a failed attempt when it would
    // be reconnected. Otherwise the session is left as it always was: up,
    // and ready late if the bot ever says so.
    if (IsReconnectEnabled())
    {
        EndLiveSessionForReconnect(Epoch, EConvaiReconnectTrigger::BotReadyTimeout);
        return;
    }

    // What was held goes out as it always did, to a bot that may or may not be
    // listening. The same session with no gap, so nothing held is stale.
    HandshakeGaveUpEpoch = Epoch;
    for (const UConvaiConnectionSessionProxy* Session : { CurrentCharacterSession, CurrentPlayerSession })
    {
        if (!IsValid(Session))
        {
            continue;
        }
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = Session->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnReadyTimedOut();
        }
    }
    FlushQueuedText(/*bIgnoreAge*/ true);
}

void UConvaiSubsystem::HandleBotReady(uint64 Epoch)
{
    if (!IsCurrentEpoch(Epoch) || ReadyEpoch == Epoch)
    {
        return;
    }
    ReadyEpoch = Epoch;
    StopClientReadyHandshake();

    // Success is here, not at Connected: the chain's budget is spent only
    // until something actually works.
    ReconnectCoordinator.NotifyReady();
    // A handler of the Reconnected status may have stopped or replaced this
    // session; what follows is only for the one that became ready.
    if (!IsCurrentEpoch(Epoch))
    {
        return;
    }

    for (const UConvaiConnectionSessionProxy* Session : { CurrentCharacterSession, CurrentPlayerSession })
    {
        if (!IsValid(Session))
        {
            continue;
        }
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = Session->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnSessionReady();
        }
#if WITH_TESTS
        if (SessionReadyTestTap && Session == CurrentCharacterSession)
        {
            SessionReadyTestTap(Session);
        }
#endif
    }

    FlushQueuedText(/*bIgnoreAge*/ false);
}

bool UConvaiSubsystem::IsSessionReady(const UConvaiConnectionSessionProxy* SessionProxy) const
{
    return IsValid(SessionProxy) && SessionProxy == CurrentCharacterSession && bIsConnected
        && ReadyEpoch != 0 && ReadyEpoch == ConnectEpoch.load(std::memory_order_acquire);
}

void UConvaiSubsystem::SendClientReadyMessage() const
{
    if (!ConvaiClient || !bIsConnected || ReadyEpoch == ConnectEpoch.load(std::memory_order_acquire))
    {
        return;
    }

    const TSharedRef<FJsonObject> About = MakeShared<FJsonObject>();
    About->SetStringField(TEXT("library"), TEXT("ue_sdk"));
    About->SetStringField(
        TEXT("library_version"),
        UTF8_TO_TCHAR(convai::GetConvaiClientVersion()));
    About->SetStringField(TEXT("platform"), TEXT("unreal"));

    const TSharedRef<FJsonObject> DataJson = MakeShared<FJsonObject>();
    // The current Unreal parser consumes RTVI v1 packet shapes. Advertise that
    // supported wire version without implying that it is the plugin build or
    // native realtime-client version.
    DataJson->SetStringField(TEXT("version"), TEXT("1.2.0"));
    DataJson->SetObjectField(TEXT("about"), About);

    FString DataJsonStr;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJsonStr);
    FJsonSerializer::Serialize(DataJson, Writer);

    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::ClientReady, TCHAR_TO_UTF8(*DataJsonStr));
}

bool UConvaiSubsystem::ResetIdleTimer()
{
    if (!ConvaiClient || !bIsConnected)
    {
        return false;
    }
    
    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::ResetIdleTimer, TCHAR_TO_UTF8(TEXT("{}")));
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Sent reset idle timer"));
    bRenewalAwaitingAck = true;

    // The session is not closing for inactivity after all. Leaving the flag set
    // made the next drop - of any cause - read as an idle close.
    bIdleDisconnectPending = false;
    return true;
}

bool UConvaiSubsystem::ShouldRenewIdleTimer(double ElapsedSeconds, int32 RemainingSeconds, float AfkTimeSeconds)
{
    // A malformed warning must not buy the session extra life.
    const double Remaining = FMath::Max(0, RemainingSeconds);
    return ElapsedSeconds + Remaining < (double)AfkTimeSeconds;
}

namespace
{
    /** Placeholder until the soak: well over the flap a full transport restart
     *  measured (transport_resume_full, RUNLOG slice 15/16). */
    constexpr double kBotAbsenceWindowSeconds = 10.0;

    double BotAbsenceWindowSeconds()
    {
        const FString Override = UCommandLineUtils::GetCommandLineFlagValueAsString(
            TEXT("BotAbsenceWindowSeconds"), FString());
        return Override.IsEmpty() ? kBotAbsenceWindowSeconds : FMath::Max(0.0, FCString::Atod(*Override));
    }
}

void UConvaiSubsystem::InitializeReconnectCoordinator()
{
    FConvaiReconnectCoordinator::FHooks Hooks;

    Hooks.Now = []() { return FPlatformTime::Seconds(); };

    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);

    Hooks.Snapshot = [WeakThis]() -> FConvaiReconnectContext
    {
        const UConvaiSubsystem* Subsystem = WeakThis.Get();
        return Subsystem ? Subsystem->SnapshotReconnectContext() : FConvaiReconnectContext();
    };

    Hooks.StartAttempt = [WeakThis]()
    {
        UConvaiSubsystem* Subsystem = WeakThis.Get();
        if (!Subsystem)
        {
            return;
        }
        UConvaiConnectionSessionProxy* Proxy = Subsystem->ReconnectProxy.Get();
        if (!IsValid(Proxy) || Subsystem->ReconnectCharacterID.IsEmpty()
            || (Subsystem->ConnectionManager
                && !Subsystem->ConnectionManager->ReclaimForReconnect(Proxy, Subsystem->ReconnectCharacterID)))
        {
            // The game was told this session is coming back; it is not, so it
            // is told the session ended instead.
            CONVAI_LOG(ConvaiSubsystemLog, Warning,
                TEXT("Reconnect: nothing left to reopen; the session's owner is gone"));
            Subsystem->ReconnectCoordinator.Stop();
            Subsystem->CurrentConnectionState = EC_ConnectionState::Disconnected;
            Subsystem->BroadcastServerConnectionStateChanged(EC_ConnectionState::Disconnected);
            return;
        }
        // The character hears about every attempt; the subsystem announced
        // the whole chain once, as Reconnecting.
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = Proxy->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnAttendeeConnecting();
        }
        // A wake from Dormant is a new Visit, announced as Connecting; only an
        // attempt inside an announced chain stays Reconnecting.
        TGuardValue<bool> RetryScope(Subsystem->bStartingReconnectAttempt,
            Subsystem->ReconnectCoordinator.IsChainAnnounced());
        Subsystem->ConnectSession(Proxy, Subsystem->ReconnectCharacterID);
    };

    // FTSTicker, not the world's TimerManager: a retry must survive a paused
    // game and a level load, and world timers survive neither.
    Hooks.Schedule = [WeakThis](double DelaySeconds, TFunction<void()> Work) -> int32
    {
        UConvaiSubsystem* Subsystem = WeakThis.Get();
        if (!Subsystem)
        {
            return INDEX_NONE;
        }
        const int32 Handle = Subsystem->NextScheduledReconnect++;
        FScheduledReconnect& Entry = Subsystem->ScheduledReconnects.Add(Handle);
        Entry.Work = MoveTemp(Work);
        Entry.Ticker = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([WeakThis, Handle](float) -> bool
            {
                if (UConvaiSubsystem* Owner = WeakThis.Get())
                {
                    Owner->RunScheduledReconnect(Handle);
                }
                return false;  // one shot
            }),
            static_cast<float>(DelaySeconds));
        return Handle;
    };

    Hooks.Cancel = [WeakThis](int32 Handle)
    {
        UConvaiSubsystem* Subsystem = WeakThis.Get();
        FScheduledReconnect Entry;
        if (Subsystem && Subsystem->ScheduledReconnects.RemoveAndCopyValue(Handle, Entry))
        {
            FTSTicker::GetCoreTicker().RemoveTicker(Entry.Ticker);
        }
    };

    Hooks.EmitServerState = [WeakThis](EC_ConnectionState State)
    {
        UConvaiSubsystem* Subsystem = WeakThis.Get();
        if (!Subsystem)
        {
            return;
        }
        Subsystem->CurrentConnectionState = State;

        // The coordinator announces an end from inside a backoff: a Stop
        // Session, or AFK running out while it waited. A game stops from inside
        // its own handlers, and announced on that stack a handler that restarts
        // on Disconnected re-entered itself. Next tick, and dropped if a new
        // session has started by then, exactly as any other session end.
        if (State == EC_ConnectionState::Disconnected)
        {
            Subsystem->SynthesizeExplicitDisconnect(nullptr);
            return;
        }
        Subsystem->BroadcastServerConnectionStateChanged(State);
    };

    Hooks.BotAbsenceExpired = [WeakThis]()
    {
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            Subsystem->OnBotAbsenceExpired();
        }
    };

    Hooks.EmitStatus = [WeakThis](const FConvaiReconnectStatus& Status)
    {
        UConvaiSubsystem* Subsystem = WeakThis.Get();
        if (!Subsystem)
        {
            return;
        }
        Subsystem->LastReconnectStatus = Status;
        const FString Reason = UEnum::GetDisplayValueAsText(Status.Reason).ToString();
        const FString Cause = UEnum::GetDisplayValueAsText(Status.Cause).ToString();
        const FString& Character = Subsystem->ReconnectCharacterID;
        switch (Status.Status)
        {
        case EC_ReconnectStatus::Scheduled:
            CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Reconnect: attempt %d/%d for [%s] in %.1f s"),
                Status.Attempt, Status.MaxAttempts, *Character, Status.DelaySeconds);
            break;
        case EC_ReconnectStatus::Attempting:
            if (Status.Attempt == 1)
            {
                Subsystem->ChainPreviousSessionID = Subsystem->LiveSessionID;
            }
            CONVAI_LOG(ConvaiSubsystemLog, Display, TEXT("Reconnect: attempt %d/%d for [%s], reason %s, cause %s, previous session %s"),
                Status.Attempt, Status.MaxAttempts, *Character, *Reason, *Cause, *Subsystem->ChainPreviousSessionID);
            break;
        case EC_ReconnectStatus::Reconnected:
            CONVAI_LOG(ConvaiSubsystemLog, Display, TEXT("Reconnect: [%s] back after %d attempt(s), session %s -> %s"),
                *Character, Status.Attempt, *Subsystem->ChainPreviousSessionID, *Subsystem->LiveSessionID);
            break;
        case EC_ReconnectStatus::GaveUp:
            CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("Reconnect: gave up on [%s] after %d attempt(s), reason %s, cause %s"),
                *Character, Status.Attempt, *Reason, *Cause);
            break;
        case EC_ReconnectStatus::Dormant:
            CONVAI_LOG(ConvaiSubsystemLog, Display, TEXT("Reconnect: [%s] is dormant until the player is back"), *Character);
            break;
        }
#if WITH_TESTS
        if (Subsystem->ReconnectStatusTestTap)
        {
            Subsystem->ReconnectStatusTestTap(Status);
        }
#endif
    };

    ReconnectCoordinator.Initialize(MoveTemp(Hooks));
}

FConvaiReconnectContext UConvaiSubsystem::SnapshotReconnectContext() const
{
    FConvaiReconnectContext Context;
    Context.Reason = LastDisconnectReason;
    Context.Cause = LastDisconnectCause;
    Context.AttemptsAllowed = UConvaiUtils::GetReconnectAttempts();
    Context.IdleTimeout = UConvaiUtils::GetIdleTimeoutBehavior();
    Context.bRenewalUnacked = bRenewalAwaitingAck.load();
    Context.BotAbsenceWindowSeconds = BotAbsenceWindowSeconds();
    Context.SecondsSinceActivity = FPlatformTime::Seconds() - LastPlayerActivityTime;
    if (const UConvaiSettings* Settings = Convai::Get().GetConvaiSettings())
    {
        Context.AfkBudgetSeconds = Settings->AfkTimeSeconds;
    }
    // A retry needs something to reopen and somebody to reopen it for: an
    // orphaned or prepared session has neither, and reopening it gave a
    // session nobody listens to.
    const UConvaiConnectionSessionProxy* Proxy = ReconnectProxy.Get();
    const UObject* Owner = IsValid(Proxy) ? Proxy->GetConnectionInterface().GetObject() : nullptr;
    Context.bOwnerAlive = Owner && !Owner->IsA<UConvaiPrepConnectionListener>() && !ReconnectCharacterID.IsEmpty()
        && (!ConnectionManager || ConnectionManager->GetManagedProxy() == Proxy
            || ConnectionManager->GetManagedState() == EConnectionEntryState::None);
    if (const UWorld* World = GetWorld())
    {
        Context.bDedicatedServer = World->GetNetMode() == NM_DedicatedServer;
    }
    return Context;
}

void UConvaiSubsystem::OnBotAbsenceExpired()
{
    if (!IsCurrentEpoch(BotAbsentEpoch))
    {
        return;
    }
    PendingDisconnectCause.store(EC_DisconnectCause::BotLeft);
    EndLiveSessionForReconnect(BotAbsentEpoch, EConvaiReconnectTrigger::BotLeft);
}

bool UConvaiSubsystem::TriggerReconnect(EConvaiReconnectTrigger Trigger)
{
    // What the session told us before it ended is the cause of this ending,
    // and of no later one.
    LastDisconnectCause = PendingDisconnectCause.exchange(EC_DisconnectCause::Unknown);
    const bool bRetryPending = ReconnectCoordinator.Trigger(Trigger);
    if (!bRetryPending)
    {
        DropQueuedText(TEXT("the session ended"));
    }
    return bRetryPending;
}

bool UConvaiSubsystem::AdoptStartSession(const UConvaiConnectionSessionProxy* SessionProxy, const FString& CharacterID)
{
#if WITH_TESTS
    // The control arm: what a legacy restart did before adoption existed.
    if (UCommandLineUtils::GetCommandLineFlagValueAsString(TEXT("ConvaiReconnectAdoptLegacyStart"), TEXT("1")) == TEXT("0"))
    {
        return false;
    }
#endif
    const EConvaiReconnectState State = ReconnectCoordinator.State();
    if (!IsValid(SessionProxy) || SessionProxy != ReconnectProxy.Get() || CharacterID != ReconnectCharacterID
        || (State != EConvaiReconnectState::Backoff && State != EConvaiReconnectState::Connecting))
    {
        return false;
    }
    CONVAI_LOG(ConvaiSubsystemLog, Display,
        TEXT("Reconnect: Start Session adopted into the pending attempt for character [%s]"), *CharacterID);
    ReconnectCoordinator.FireNow();
    return true;
}

bool UConvaiSubsystem::SupersedeReconnect(const FString& NewCharacterID)
{
    const EConvaiReconnectState State = ReconnectCoordinator.State();
    const bool bPending = State == EConvaiReconnectState::Backoff || State == EConvaiReconnectState::Connecting;
    // Dormant and a Failed that a wake could still revive both belong to the
    // old character; neither may be woken against the new one.
    const bool bRevivable = State == EConvaiReconnectState::Dormant || State == EConvaiReconnectState::Failed;
    if ((!bPending && !bRevivable) || !ReconnectProxy.IsValid()
        || ReconnectCharacterID == NewCharacterID)
    {
        return false;
    }

    CONVAI_LOG(ConvaiSubsystemLog, Log,
        TEXT("Reconnect: character [%s] started; the pending reconnect of [%s] is superseded"),
        *NewCharacterID, *ReconnectCharacterID);

    UConvaiConnectionSessionProxy* Superseded = ReconnectProxy.Get();
    ReconnectCoordinator.Stop(/*bAnnounceEnding*/ false);
    ReconnectProxy.Reset();
    ReconnectCharacterID.Empty();
    DropQueuedText(TEXT("another character was started"));

    LastDisconnectCause = EC_DisconnectCause::Superseded;
    // Announced here, synchronously, before the new character's connect: a
    // next-tick Disconnected is stamped with an epoch that connect replaces.
    // Dormant already announced its ending.
    if (bPending)
    {
        LastDisconnectReason = EC_DisconnectReason::Explicit;
        BroadcastExplicitDisconnect(Superseded);
    }

    // Its owner lets the lease go, as its own Stop Session would.
    if (UConvaiChatbotComponent* Owner = Cast<UConvaiChatbotComponent>(Superseded->GetConnectionInterface().GetObject()))
    {
        Owner->StopSession();
    }
    return true;
}

bool UConvaiSubsystem::IsReconnectEnabled() const
{
    return UConvaiUtils::GetReconnectAttempts() > 0;
}

void UConvaiSubsystem::EndLiveSessionForReconnect(uint64 Epoch, EConvaiReconnectTrigger Trigger)
{
    // Only for an owner that would be reconnected: otherwise the session is
    // left exactly as it was before reconnect existed, and the server decides.
    if (!IsCurrentEpoch(Epoch) || !bIsConnected || !IsValid(CurrentCharacterSession)
        || !IsReconnectEnabled())
    {
        return;
    }

    CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("Ending the live session to reconnect it: %s"),
        Trigger == EConvaiReconnectTrigger::BotReadyTimeout ? TEXT("the character never became ready")
        : Trigger == EConvaiReconnectTrigger::BotLeft ? TEXT("the character left and did not come back")
        : Trigger == EConvaiReconnectTrigger::MicUnheard ? TEXT("the server never heard the player")
        : TEXT("the server reported an error it marked fatal"));

    bIsConnected = false;
    TSet<FString> DepartedAttendees;
    {
        FScopeLock Lock(&AttendeeMutex);
        DepartedAttendees = MoveTemp(ConnectedAttendees);
        ConnectedAttendees.Empty();
    }
    // The same path a drop takes, so the client is let go - and its epoch
    // with it - before a retry can start: a late bot-ready from it must not
    // mark the retry ready.
    EndSession(Epoch, DepartedAttendees, Trigger);
}

bool UConvaiSubsystem::FailPreflight(UConvaiConnectionSessionProxy* SessionProxy, const FString& CharacterID)
{
    CONVAI_LOG(ConvaiSubsystemLog, Error,
        TEXT("Not connecting: %s. Retrying cannot fix this; set it and start the session again."),
        CharacterID.IsEmpty()
            ? TEXT("the character has no Character ID")
            : TEXT("no API key or auth token is set (Project Settings > Plugins > Convai)"));

    PendingDisconnectCause.store(EC_DisconnectCause::Preflight);
    ReconnectProxy = SessionProxy;
    ReconnectCharacterID = CharacterID;
    const bool bWasRetrying = bStartingReconnectAttempt;
    TriggerReconnect(EConvaiReconnectTrigger::AttemptFailed);

    // A chain the game was told is coming back ends here, and says so, and
    // lets go of the lease its retry had reclaimed.
    if (bWasRetrying)
    {
        const FDisconnectBroadcastScope BroadcastScope(this);
        CurrentConnectionState = EC_ConnectionState::Disconnected;
        BroadcastServerConnectionStateChanged(EC_ConnectionState::Disconnected);
        if (ConnectionManager)
        {
            ConnectionManager->OnServerDisconnected(SessionProxy, ReconnectCoordinator.State());
        }
    }
    return false;
}

void UConvaiSubsystem::RunScheduledReconnect(int32 Handle)
{
    FScheduledReconnect Entry;
    if (ScheduledReconnects.RemoveAndCopyValue(Handle, Entry) && Entry.Work)
    {
        Entry.Work();
    }
}

void UConvaiSubsystem::WakeReconnect()
{
    // A session brought back from Dormant is the player arriving again: a new
    // Visit, with its own AFK window.
    if (ReconnectCoordinator.Wake())
    {
        BeginVisit();
    }
}

bool UConvaiSubsystem::ShouldHoldStartForWake(const UConvaiConnectionSessionProxy* SessionProxy)
{
    if (!IsInsideDisconnectBroadcast() || !IsValid(SessionProxy) || SessionProxy != ReconnectProxy.Get()
        || LastDisconnectReason != EC_DisconnectReason::Idle)
    {
        return false;
    }
    // Decided from the snapshot, not the coordinator's state: a handler bound
    // to the character runs before the coordinator has been asked.
    if (ReconnectCoordinator.Predict(SnapshotReconnectContext(), EConvaiReconnectTrigger::Drop).Decision != EConvaiReconnectDecision::Dormant)
    {
        return false;
    }
    if (!bWarnedHeldStart)
    {
        bWarnedHeldStart = true;
        CONVAI_LOG(ConvaiSubsystemLog, Warning,
            TEXT("Start Session from inside an idle close is held until the player is back: opening now would hold a "
                 "session for an empty room. Remove Start Session from Disconnected; the plugin reconnects by itself."));
    }
    return true;
}

void UConvaiSubsystem::StopReconnect(const UConvaiConnectionSessionProxy* SessionProxy)
{
    if (!IsValid(SessionProxy) || SessionProxy != ReconnectProxy.Get())
    {
        return;
    }

    // A backoff owes the game one Disconnected, and it is the game's own stop.
    // A Dormant or Failed session already ended with its reason, and an
    // attempt in flight is relabelled by the teardown that follows.
    if (ReconnectCoordinator.State() == EConvaiReconnectState::Backoff)
    {
        LastDisconnectReason = EC_DisconnectReason::Explicit;
    }
    ReconnectCoordinator.Stop();
    ReconnectProxy.Reset();
    ReconnectCharacterID.Empty();
    DropQueuedText(TEXT("the game stopped the session"));
}

void UConvaiSubsystem::BeginVisit()
{
    // A Visit is the player's presence, not one connection: it spans every
    // session a drop chain opens. Only a start the game asked for begins one -
    // not a reconnect, not the microphone watchdog, not a player component
    // attaching to a session that is already up.
    LastPlayerActivityTime = FPlatformTime::Seconds();
    bIdleDisconnectPending = false;
    bSpentTalkingGrace = false;
    LastDisconnectReason = EC_DisconnectReason::Unexpected;
    LastDisconnectCause = EC_DisconnectCause::Unknown;
    PendingDisconnectCause.store(EC_DisconnectCause::Unknown);
    bRenewalAwaitingAck = false;
    PendingServerIdle.store(-1);
    bWarnedHeldStart = false;
    bMicUnheardSpentThisVisit = false;
    ExplicitEpoch.store(0, std::memory_order_release);
}

void UConvaiSubsystem::MarkPlayerActivity()
{
    LastPlayerActivityTime = FPlatformTime::Seconds();
    // The player is back, so the server restarts its countdown and the idle
    // disconnect we had stopped deferring is no longer coming. A fresh window
    // also earns back the reprieve.
    bIdleDisconnectPending = false;
    bSpentTalkingGrace = false;
}

bool UConvaiSubsystem::IsAnyChatbotTalking()
{
    for (UConvaiChatbotComponent* Chatbot : GetAllChatbotComponents())
    {
        if (IsValid(Chatbot) && Chatbot->GetIsTalking())
        {
            return true;
        }
    }
    return false;
}

void UConvaiSubsystem::MarkExplicitDisconnect()
{
    // Scoped to the epoch whose transport is being torn down. It used to be one
    // global flag, so a player's Stop Session relabelled a character's drop —
    // and a verdict set for one session outlived it into the next.
    ExplicitEpoch.store(ConnectEpoch.load(std::memory_order_acquire), std::memory_order_release);
}

FConvaiClientListenerShim::FConvaiClientListenerShim(UConvaiSubsystem* InSubsystem, uint64 InEpoch)
    : Subsystem(InSubsystem)
    , Epoch(InEpoch)
{
}

void FConvaiClientListenerShim::OnConnectedToServer(const char* session_id, const char* char_session_id)
{
    if (UConvaiSubsystem* Sub = Subsystem.Get())
    {
        Sub->OnTransportConnected(Epoch, UTF8_TO_TCHAR(session_id ? session_id : ""),
                                  UTF8_TO_TCHAR(char_session_id ? char_session_id : ""));
    }
}

void FConvaiClientListenerShim::OnDisconnectedFromServer()
{
    if (UConvaiSubsystem* Sub = Subsystem.Get())
    {
        Sub->OnTransportDisconnected(Epoch);
    }
}

void FConvaiClientListenerShim::OnAudioData(const char* attendee_id, const int16_t* audio_data,
                                            size_t num_frames, uint32_t sample_rate,
                                            uint32_t bits_per_sample, uint32_t num_channels)
{
    if (UConvaiSubsystem* Sub = Subsystem.Get())
    {
        Sub->OnTransportAudioData(Epoch, attendee_id, audio_data, num_frames, sample_rate,
                                  bits_per_sample, num_channels);
    }
}

void FConvaiClientListenerShim::OnAttendeeConnected(const char* attendee_id)
{
    if (UConvaiSubsystem* Sub = Subsystem.Get())
    {
        Sub->OnTransportAttendeeConnected(Epoch, UTF8_TO_TCHAR(attendee_id ? attendee_id : ""));
    }
}

void FConvaiClientListenerShim::OnAttendeeDisconnected(const char* attendee_id)
{
    if (UConvaiSubsystem* Sub = Subsystem.Get())
    {
        Sub->OnTransportAttendeeDisconnected(Epoch, UTF8_TO_TCHAR(attendee_id ? attendee_id : ""));
    }
}

void FConvaiClientListenerShim::OnActiveSpeakerChanged(const char* Speaker)
{
    if (UConvaiSubsystem* Sub = Subsystem.Get())
    {
        Sub->OnTransportActiveSpeakerChanged(Epoch, UTF8_TO_TCHAR(Speaker ? Speaker : ""));
    }
}

void FConvaiClientListenerShim::OnDataPacketReceived(const char* JsonData, const char* attendee_id)
{
    if (UConvaiSubsystem* Sub = Subsystem.Get())
    {
        Sub->OnTransportDataPacket(Epoch, UTF8_TO_TCHAR(JsonData ? JsonData : ""),
                                   UTF8_TO_TCHAR(attendee_id ? attendee_id : ""));
    }
}

void FConvaiClientListenerShim::OnLog(const char* log_message)
{
    if (UConvaiSubsystem* Sub = Subsystem.Get())
    {
        Sub->OnTransportLog(UConvaiUtils::FUTF8ToFString(log_message));
    }
}

uint64 UConvaiSubsystem::BumpConnectEpoch()
{
    return ConnectEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool UConvaiSubsystem::IsCurrentEpoch(uint64 Epoch) const
{
    return Epoch == ConnectEpoch.load(std::memory_order_acquire);
}

UConvaiSubsystem::FDisconnectBroadcastScope::FDisconnectBroadcastScope(UConvaiSubsystem* InSubsystem)
    : Subsystem(InSubsystem)
{
    if (UConvaiSubsystem* Sub = Subsystem.Get())
    {
        ++Sub->DisconnectBroadcastDepth;
    }
}

UConvaiSubsystem::FDisconnectBroadcastScope::~FDisconnectBroadcastScope()
{
    if (UConvaiSubsystem* Sub = Subsystem.Get())
    {
        --Sub->DisconnectBroadcastDepth;
    }
}

#if WITH_TESTS
void FConvaiConnectionTestSeam::Install(FAttemptHook Hook)
{
    GTestAttemptHook = MoveTemp(Hook);
}

void FConvaiConnectionTestSeam::InstallSend(FSendHook Hook)
{
    GTestSendHook = MoveTemp(Hook);
}

namespace
{
    FConvaiConnectionTestSeam::FMessageTap GConvaiMessageTap;
}

void FConvaiConnectionTestSeam::InstallMessageTap(FMessageTap Tap)
{
    GConvaiMessageTap = MoveTemp(Tap);
}

bool FConvaiConnectionTestSeam::TakeMessage(const TCHAR* Type)
{
    if (!GConvaiMessageTap)
    {
        return false;
    }
    GConvaiMessageTap(Type);
    return true;
}

bool FConvaiConnectionTestSeam::TakeSend(const FString& Json, bool& bOutSent)
{
    if (!GTestSendHook)
    {
        return false;
    }
    bOutSent = GTestSendHook(Json);
    return true;
}

void FConvaiConnectionTestSeam::Reset()
{
    GTestSendHook = nullptr;
    GTestAttemptHook = nullptr;
    GTestAttemptCount = 0;
    GTestPendingFailures = 0;
}

int32 FConvaiConnectionTestSeam::AttemptCount()
{
    return GTestAttemptCount;
}

void FConvaiConnectionTestSeam::FailNextConnects(int32 Count)
{
    GTestPendingFailures = FMath::Max(0, Count);
}

int32 FConvaiConnectionTestSeam::PendingFailures()
{
    return GTestPendingFailures;
}

bool FConvaiConnectionTestSeam::TakeAttempt(const FConvaiConnectionParams& Params)
{
    ++GTestAttemptCount;
    return GTestAttemptHook ? GTestAttemptHook(Params) : false;
}

bool FConvaiConnectionTestSeam::TakeFailure()
{
    if (GTestPendingFailures <= 0)
    {
        return false;
    }
    --GTestPendingFailures;
    return true;
}
#endif // WITH_TESTS

void UConvaiSubsystem::ResolveDisconnectReason(uint64 Epoch)
{
    // Explicit belongs to the epoch whose transport the game tore down, not to
    // the subsystem. A verdict that outlived its session made the next drop
    // read as one the game asked for — and a reconnect skips those.
    if (ExplicitEpoch.load(std::memory_order_acquire) == Epoch)
    {
        LastDisconnectReason = EC_DisconnectReason::Explicit;
        return;
    }

    const int8 ServerSaidIdle = PendingServerIdle.exchange(-1);
    const bool bIdle = ServerSaidIdle >= 0 ? ServerSaidIdle == 1 : bIdleDisconnectPending;
    LastDisconnectReason = bIdle ? EC_DisconnectReason::Idle : EC_DisconnectReason::Unexpected;
}

EConvaiIdleWarningAction UConvaiSubsystem::DecideIdleWarningAction(
    double ElapsedSeconds, int32 RemainingSeconds, float AfkTimeSeconds,
    bool bCharacterTalking, bool bGraceAlreadySpent)
{
    if (ShouldRenewIdleTimer(ElapsedSeconds, RemainingSeconds, AfkTimeSeconds))
    {
        return EConvaiIdleWarningAction::Renew;
    }

    // Out of budget, but cutting a character off mid-word reads worse than a late
    // disconnect, so one more server window is bought for the line to land. Once
    // only: a character still going after that is talking to an empty chair,
    // which is the thing AFK Time exists to end.
    if (bCharacterTalking && !bGraceAlreadySpent)
    {
        return EConvaiIdleWarningAction::RenewForSpeech;
    }

    return EConvaiIdleWarningAction::LetClose;
}

void UConvaiSubsystem::HandleUserIdleWarning(int32 RemainingSeconds)
{
    check(IsInGameThread());

    float AfkTimeSeconds = 600.0f;
    if (const UConvaiSettings* Settings = Convai::Get().GetConvaiSettings())
    {
        AfkTimeSeconds = Settings->AfkTimeSeconds;
    }

    const double ElapsedSeconds = FPlatformTime::Seconds() - LastPlayerActivityTime;

    switch (DecideIdleWarningAction(ElapsedSeconds, RemainingSeconds, AfkTimeSeconds,
        IsAnyChatbotTalking(), bSpentTalkingGrace))
    {
    case EConvaiIdleWarningAction::Renew:
        ResetIdleTimer();
        CONVAI_LOG(ConvaiSubsystemLog, Log,
            TEXT("Idle warning: %.0fs since the player did anything, %ds left, AFK budget %.0fs - renewed"),
            ElapsedSeconds, RemainingSeconds, AfkTimeSeconds);
        return;

    case EConvaiIdleWarningAction::RenewForSpeech:
        bSpentTalkingGrace = true;
        ResetIdleTimer();
        CONVAI_LOG(ConvaiSubsystemLog, Log,
            TEXT("Idle warning: past the %.0fs AFK budget but the character is mid-line - renewed once so it can finish"),
            AfkTimeSeconds);
        return;

    case EConvaiIdleWarningAction::LetClose:
        break;
    }

    bIdleDisconnectPending = true;
    CONVAI_LOG(ConvaiSubsystemLog, Log,
        TEXT("Idle warning: %.0fs since the player did anything, %ds left, AFK budget %.0fs - letting the session close"),
        ElapsedSeconds, RemainingSeconds, AfkTimeSeconds);

    // Only surfaced once it is going to come true. A warning we were about to
    // renew away would have handed the game a countdown that never runs out.
    OnUserIdleWarning.Broadcast(RemainingSeconds);
}


void UConvaiSubsystem::MicWatchdogReset()
{
    // Atomics only: reached from the transport thread (OnConnectedToServer).
    // A timer already armed for the old session finds no speech and no-ops.
    MicWatchdogSpeechFrames = 0;
    MicWatchdogLastSpeechTime = 0.0;
    MicWatchdogNoiseFloor = 0.0;
    MicWatchdogBotSpeakingUntil = 0.0;
    MicWatchdogLastTextSendTime = 0.0;
    bMicWatchdogArmed = false;
    bMicWatchdogServerHeardUser = false;
    bMicWatchdogReconnected = false;
}

void UConvaiSubsystem::MicWatchdogServerHeardUser()
{
    if (FPlatformTime::Seconds() - MicWatchdogLastTextSendTime.load() < MicWatchdogTextEchoSeconds)
    {
        return;
    }
    if (!bMicWatchdogServerHeardUser.exchange(true))
    {
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[MicWatchdog] server acknowledged player speech (after reconnect=%d)"), bMicWatchdogReconnected.load() ? 1 : 0);
    }
}

void UConvaiSubsystem::MicWatchdogScheduleCheck(double DelaySeconds, uint64 ArmedEpoch)
{
    FTSTicker::GetCoreTicker().RemoveTicker(MicWatchdogTicker);
    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    MicWatchdogTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [WeakThis, ArmedEpoch](float)
        {
            if (UConvaiSubsystem* Subsystem = WeakThis.Get())
            {
                Subsystem->MicWatchdogTicker.Reset();
                Subsystem->MicWatchdogCheck(ArmedEpoch);
            }
            return false;
        }), static_cast<float>(DelaySeconds));
}

void UConvaiSubsystem::MicWatchdogCheck(uint64 ArmedEpoch)
{
    // Armed by speech into a session that has since been replaced.
    if (!IsCurrentEpoch(ArmedEpoch))
    {
        bMicWatchdogArmed = false;
        return;
    }
    const double Quiet = FPlatformTime::Seconds() - MicWatchdogLastSpeechTime.load();
    if (Quiet < MicWatchdogSilenceSeconds)
    {
        MicWatchdogScheduleCheck(MicWatchdogSilenceSeconds - Quiet, ArmedEpoch);
        return;
    }
    // Disarmed either way; the next speech chunk past the threshold re-arms.
    bMicWatchdogArmed = false;

    const int64 SpeechFrames = MicWatchdogSpeechFrames.load();
    if (SpeechFrames < MicWatchdogMinSpeechFrames
        || bMicWatchdogReconnected || bMicWatchdogServerHeardUser || !bIsConnected || !ConvaiClient
        || !ConnectionManager || !ConnectionManager->IsCharacterConnectionActive()
        || ConnectionManager->GetManagedProxy() != CurrentCharacterSession
        || GetSessionConnectionState(CurrentCharacterSession) != EC_ConnectionState::Connected)
    {
        return;
    }
    {
        FScopeLock Lock(&AttendeeMutex);
        if (ConnectedAttendees.Num() == 0)
        {
            return;
        }
    }

    const bool bThroughCoordinator = IsReconnectEnabled();
    if (bThroughCoordinator
        && (bMicUnheardSpentThisVisit || ReconnectCoordinator.State() != EConvaiReconnectState::Ready))
    {
        return;
    }

    CONVAI_LOG(ConvaiSubsystemLog, Error,
        TEXT("[MicWatchdog] %.1fs of speech sent, server never acknowledged hearing the player; reconnecting character session"),
        static_cast<double>(SpeechFrames) / static_cast<double>(ConvaiConstants::WebRTCAudioSampleRate));
    bMicWatchdogReconnected = true;

    if (bThroughCoordinator)
    {
        // A player who has been talking for two seconds is here: without this
        // the classify could send them Dormant.
        bMicUnheardSpentThisVisit = true;
        MarkPlayerActivity();
        EndLiveSessionForReconnect(ConnectEpoch.load(std::memory_order_acquire), EConvaiReconnectTrigger::MicUnheard);
        return;
    }

    // The same fresh /connect a StartSession-while-connected performs: the
    // proxy, the manager's lease and the player's session all survive, and the
    // listener is detached before the old client disconnects so its deferred
    // OnDisconnectedFromServer cannot tear the new client down.
    ConnectSession(CurrentCharacterSession, ConnectionManager->ManagedCharacterID);
}

EC_ConnectionState UConvaiSubsystem::GetServerConnectionState() const
{
    return CurrentConnectionState;
}

EC_ConnectionState UConvaiSubsystem::GetSessionConnectionState(const UConvaiConnectionSessionProxy* SessionProxy) const
{
    if (!IsValid(SessionProxy))
    {
        return EC_ConnectionState::Disconnected;
    }

    FScopeLock SessionLock(&SessionMutex);

    // Check if this session is the current active session and server is connected
    bool bIsActiveSession = false;
    if (SessionProxy->IsPlayerSession())
    {
        bIsActiveSession = (SessionProxy == CurrentPlayerSession && bIsConnected);
    }
    else
    {
        bIsActiveSession = (SessionProxy == CurrentCharacterSession && bIsConnected);
    }

    if (!bIsActiveSession)
    {
        return EC_ConnectionState::Disconnected;
    }

    // Check if the attendee associated with this session is connected to the WebRTC room
    const FString& AttendeeId = SessionProxy->GetAttendeeId();
    if (!AttendeeId.IsEmpty())
    {
        FScopeLock Lock(&AttendeeMutex);
        if (ConnectedAttendees.Contains(AttendeeId))
        {
            return EC_ConnectionState::Connected;
        }
    }

    // Session is active and server is connected but attendee hasn't joined yet - "Connecting" state
    return EC_ConnectionState::Connecting;
}

void UConvaiSubsystem::GetAndroidMicPermission()
{
	if (!UConvaiAndroid::ConvaiAndroidHasMicrophonePermission())
		UConvaiAndroid::ConvaiAndroidAskMicrophonePermission();
}

bool UConvaiSubsystem::ConnectSession(UConvaiConnectionSessionProxy* SessionProxy, const FString& CharacterID)
{
    if (!IsValid(SessionProxy))
    {
        CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to connect session: Invalid session proxy"));
        return false;
    }
    
    // For player sessions, handle replacement properly
    if (SessionProxy->IsPlayerSession())
    {
        const UConvaiConnectionSessionProxy* OldSession = nullptr;
        
        // Acquire lock, check and swap sessions
        {
            FScopeLock SessionLock(&SessionMutex);
            
            // If there's an existing player session, store it for notification
            if (IsValid(CurrentPlayerSession) && CurrentPlayerSession != SessionProxy)
            {
                CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("Replacing existing player session"));
                OldSession = CurrentPlayerSession;
            }
            
            // Store the new player session
            CurrentPlayerSession = SessionProxy;
        }
        // Lock released here
        
        // Notify old session outside the lock to avoid deadlock
        if (IsValid(OldSession))
        {
            if (const TScriptInterface<IConvaiConnectionInterface> Interface = OldSession->GetConnectionInterface(); Interface.GetObject())
            {
                Interface->OnDisconnectedFromServer();
            }
        }
        
        return true;
    }
    
    if (CharacterID.IsEmpty() || UConvaiUtils::GetAuthHeaderAndKey().Value.IsEmpty())
    {
        return FailPreflight(SessionProxy, CharacterID);
    }

    // A connect asked for from inside a disconnect broadcast — a game
    // restarting its session from a Disconnected event — runs next tick
    // instead. Inline it would tear down the very transport whose teardown is
    // still on the stack, and join that teardown's thread to do it.
    if (IsInsideDisconnectBroadcast())
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning,
            TEXT("Start Session was called from inside a disconnect event; opening the session on the next tick instead. "
                 "Bind Start Session to something other than Disconnected: the plugin reconnects by itself."));

        TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
        TWeakObjectPtr<UConvaiConnectionSessionProxy> WeakProxy(SessionProxy);
        AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakProxy, CharacterID]()
        {
            UConvaiSubsystem* Subsystem = WeakThis.Get();
            UConvaiConnectionSessionProxy* Proxy = WeakProxy.Get();
            if (!Subsystem || !IsValid(Proxy))
            {
                return;
            }
            // The proxy may have been returned to the manager in the meantime -
            // the game stopped the session, or another component took the
            // character. Connecting a proxy nobody owns opens a session with no
            // listener on it.
            if (!Proxy->GetConnectionInterface().GetObject())
            {
                CONVAI_LOG(ConvaiSubsystemLog, Log,
                    TEXT("Dropping a deferred Start Session: its session no longer has an owner"));
                return;
            }
            Subsystem->ConnectSession(Proxy, CharacterID);
        });
        return true;
    }

    // If we already have a character session, just log it
    // No need to notify - CleanupConvaiClient() handles full disconnection
    if (IsValid(CurrentCharacterSession) && CurrentCharacterSession != SessionProxy)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("Replacing existing character session"));
    }
    
    // Every abandoned attempt still holds a thread and a client. Past the cap
    // something is restarting in a loop, and one refused connect says so
    // instead of adding to it.
    if (FConvaiConnectReaper::LiveCount() >= FConvaiConnectReaper::Capacity())
    {
        CONVAI_LOG(ConvaiSubsystemLog, Error,
            TEXT("Refusing to connect: %d previous attempts are still being torn down."),
            FConvaiConnectReaper::LiveCount());
        OnConnectAttemptFailed(ConnectEpoch.load(std::memory_order_acquire));
        return false;
    }

    // Outside the session lock: this runs the owner's Blueprint.
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = SessionProxy->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnPrepareConnect();
    }

    // Text held for another character is not for this one. A retry, or the
    // microphone watchdog, reopens the same proxy and keeps it.
    QueuedText.RemoveAll([SessionProxy](const FQueuedText& Item) { return Item.Owner.Get() != SessionProxy; });

    // Clean up and reinitialize the Client (this handles all cleanup and disconnection)
    CleanupConvaiClient();
    PendingAttendees.Reset();
    
    if (!InitializeConvaiClient())
    {
        CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to initialize Client client"));
        // A retry that never started is a failed attempt, or its chain would
        // wait in Connecting for ever.
        OnConnectAttemptFailed(ConnectEpoch.load(std::memory_order_acquire));
        return false;
    }

    if (ConvaiClient)
    {
        FScopeLock SessionLock(&SessionMutex);
        CurrentCharacterSession = SessionProxy;
        
        // Get raw pointer for connection params
        convai::ConvaiClient* ClientPtr = ConvaiClient.Get();
        FConvaiConnectionParams ConnectionParams = FConvaiConnectionParams::Create(ClientPtr, CharacterID, SessionProxy);
        // Who to tell if this attempt fails, and which attempt it was. Under
        // PIE multi-client there is more than one subsystem, and the attempt
        // knows which one it belongs to; nothing else does.
        ConnectionParams.OwningSubsystem = this;
        ConnectionParams.ConnectEpoch = ConnectEpoch.load(std::memory_order_acquire);

        // Push-to-talk owns the end of the turn, so the server's silence timer must not fire
        // first. This deliberately wins over an explicit VAD stop_secs — the two disagree about
        // who ends the turn, and the button is the one the player can see.
        if (const float PushToTalkStopSecs = UConvaiPlayerComponent::ResolvePushToTalkStopSecs(RegisteredPlayerComponents); PushToTalkStopSecs >= 0.0f)
        {
            ConnectionParams.VADStopSecs = PushToTalkStopSecs;
        }

        // Publish the advertised-action contract onto the proxy BEFORE the
        // connection thread exists: incoming action responses parse against the
        // proxy contract, and it must survive orphan rebinds to a different
        // component. Create() above just invoked the interface's
        // GetActionConfigJson, so the paired contract getter reflects exactly
        // what this payload advertises — including an explicit EMPTY contract
        // for prepared connections (prep listener) and disabled-action bots.
        {
            TArray<FConvaiAction> AdvertisedActions;
            TArray<FString> AdvertisedBuiltIns;
            if (const TScriptInterface<IConvaiConnectionInterface> Interface = SessionProxy->GetConnectionInterface(); Interface.GetObject())
            {
                Interface->GetAdvertisedActionContract(AdvertisedActions, AdvertisedBuiltIns);
            }
            SessionProxy->SetAdvertisedActions(AdvertisedActions, AdvertisedBuiltIns);
        }

#if WITH_TESTS
        // The one place an attempt turns into a thread, and so the one place a
        // test can stand in for the network. Everything around it still runs,
        // so what a test sees afterwards is the real ConnectSession.
        if (FConvaiConnectionTestSeam::TakeAttempt(ConnectionParams))
        {
            AnnounceConnecting();
            return true;
        }
        if (FConvaiConnectionTestSeam::TakeFailure())
        {
            // A forced failure reports exactly as a real one does, without a POST.
            AnnounceConnecting();
            OnConnectAttemptFailed(ConnectEpoch.load(std::memory_order_acquire));
            return true;
        }
#endif

        ConnectionThread = MakeUnique<FConvaiConnectionThread>(MoveTemp(ConnectionParams));
		if (!ConnectionThread->IsThreadStarted())
		{
			ConnectionThread.Reset();
			OnConnectAttemptFailed(ConnectEpoch.load(std::memory_order_acquire));
			return false;
		}

        AnnounceConnecting();
        
        return true;
    }
    
    return false;
}

void UConvaiSubsystem::AnnounceConnecting()
{
    // A retry belongs to a chain the game was told about once, as
    // Reconnecting. Announcing Connecting per attempt made a game that shows
    // "connecting" flicker, and one that acts on it act once per attempt.
    //
    // The flag covers the coordinator's own attempt only, and it lives on that
    // one stack frame. A game that starts a session from inside the Reconnecting
    // broadcast runs on a different stack, so ask the chain too: while one is
    // announced, a connect belongs to it. A wake from Dormant clears the
    // announcement before it attempts, so it is still Connecting.
    if (bStartingReconnectAttempt || ReconnectCoordinator.IsChainAnnounced())
    {
        CurrentConnectionState = EC_ConnectionState::Reconnecting;
        return;
    }
    CurrentConnectionState = EC_ConnectionState::Connecting;
    OnServerConnectionStateChangedEvent.Broadcast(EC_ConnectionState::Connecting);
}

void UConvaiSubsystem::DisconnectSession(const UConvaiConnectionSessionProxy* SessionProxy)
{
    if (!IsValid(SessionProxy))
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("DisconnectSession: Invalid session proxy"));
        return;
    }

    // Check if the attendee associated with this session was connected and fire disconnection callback
    const FString AttendeeId = SessionProxy->GetAttendeeId();
    if (!AttendeeId.IsEmpty())
    {
        bool bWasConnected = false;
        {
            FScopeLock Lock(&AttendeeMutex);
            bWasConnected = ConnectedAttendees.Contains(AttendeeId);
            ConnectedAttendees.Remove(AttendeeId);
        }

        if (bWasConnected)
        {
            CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🔌 Attendee disconnected via session disconnect: %s"), *AttendeeId);

            // Fire the disconnection callback to the session's interface
            if (const TScriptInterface<IConvaiConnectionInterface> Interface = SessionProxy->GetConnectionInterface(); Interface.GetObject())
            {
                Interface->OnAttendeeDisconnected(AttendeeId);
            }
        }
    }

    // If this is a player session, and it's the current one, clear it
    if (SessionProxy->IsPlayerSession())
    {
        FScopeLock SessionLock(&SessionMutex);
        if (SessionProxy == CurrentPlayerSession)
        {
            CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Disconnecting player session"));
            CurrentPlayerSession = nullptr;
        }
        else
        {
            CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("DisconnectSession: Player session is not the current active session"));
        }
        return;
    }
    
    // A session that has already ended is torn down as bookkeeping: its end was
    // announced once, with a reason, and this must not relabel it. The forum's
    // restart does exactly this — it stops the session it was told had dropped,
    // and that Stop used to turn an idle close into one the game asked for.
    if (CurrentConnectionState == EC_ConnectionState::Disconnected)
    {
        FScopeLock SessionLock(&SessionMutex);
        if (SessionProxy == CurrentCharacterSession)
        {
            CurrentCharacterSession = nullptr;
        }
        return;
    }

    // If this is a character session, and it's the current one, disconnect the client
    {
        FScopeLock SessionLock(&SessionMutex);
        if (SessionProxy != CurrentCharacterSession)
        {
            CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("DisconnectSession: Character session is not the current active session"));
            return;
        }
    }
    
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Disconnecting character session"));

    // The game is tearing this transport down, so this epoch's disconnect is
    // Explicit and nothing the dying client reports afterwards belongs to the
    // session that follows. Detach the listener before Disconnect(): the DLL
    // answers a client-initiated Disconnect with a disconnect callback of its
    // own, and that callback is not a drop the game needs to hear about.
    const uint64 DyingEpoch = ConnectEpoch.load(std::memory_order_acquire);
    ExplicitEpoch.store(DyingEpoch, std::memory_order_release);
    BumpConnectEpoch();

    // The Disconnect itself is the reaper's, not this thread's: Stop Session
    // used to hold the game thread for as long as the transport took.
    AbandonLiveClient();
    bIsConnected = false;

    // Clear the current character session
    {
        FScopeLock SessionLock(&SessionMutex);
        CurrentCharacterSession = nullptr;
    }

    DropQueuedText(TEXT("the game stopped the session"));

    // The transport will not report this one — the listener is gone — so the
    // session end is announced here, with the reason already final.
    LastDisconnectReason = EC_DisconnectReason::Explicit;
    SynthesizeExplicitDisconnect(SessionProxy);
}

void UConvaiSubsystem::SynthesizeExplicitDisconnect(const UConvaiConnectionSessionProxy* SessionProxy)
{
    // Always next tick, even from the game thread. The transport used to
    // deliver this one through a queued task, and a game that stops a session
    // from inside its own disconnect handler would otherwise see the end of
    // the session it just stopped arrive inside that handler.
    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    TWeakObjectPtr<const UConvaiConnectionSessionProxy> WeakProxy(SessionProxy);
    // Stamped like every other queued step: a connect started before this task
    // runs would otherwise be announced as disconnected by the session it
    // replaced.
    const uint64 Epoch = ConnectEpoch.load(std::memory_order_acquire);
    AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakProxy, Epoch]()
    {
        UConvaiSubsystem* Subsystem = WeakThis.Get();
        if (!Subsystem || !Subsystem->IsCurrentEpoch(Epoch))
        {
            return;
        }
        Subsystem->BroadcastExplicitDisconnect(WeakProxy.Get());
    });
}

void UConvaiSubsystem::BroadcastExplicitDisconnect(const UConvaiConnectionSessionProxy* SessionProxy)
{
    const FDisconnectBroadcastScope BroadcastScope(this);

    CurrentConnectionState = EC_ConnectionState::Disconnected;
    PendingAttendees.Reset();
    BroadcastServerConnectionStateChanged(EC_ConnectionState::Disconnected);

    if (IsValid(SessionProxy))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = SessionProxy->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnDisconnectedFromServer();
        }
    }

    if (IsValid(CurrentPlayerSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnDisconnectedFromServer();
        }
    }
}

int32 UConvaiSubsystem::SendAudio(const UConvaiConnectionSessionProxy* SessionProxy, const int16_t* AudioData, const size_t NumFrames) const
{
    if (!IsValid(SessionProxy))
    {
        return -1;
    }
    // Ahead of the connection gate: with no session there is nowhere to send
    // this, but it can still be the player talking to a character that went
    // Dormant.
    if (SessionProxy->IsPlayerSession() && !bIsConnected)
    {
        FeedMicWake(AudioData, NumFrames);
    }
    if (!ConvaiClient || !bIsConnected)
    {
        return -1;
    }

    if (SessionProxy->IsPlayerSession() && ConnectionManager && !ConnectionManager->IsCharacterConnectionActive())
    {
        return -1;
    }

    // One microphone producer per session. A displaced player component keeps a
    // valid proxy and keeps ticking, so without this two components interleave
    // their chunks into one client and the mic stream arrives at roughly twice
    // the rate of the far-end stream - which reads to the canceller exactly like
    // the two streams running on different clocks.
    if (SessionProxy->IsPlayerSession())
    {
        FScopeLock SessionLock(&SessionMutex);
        if (CurrentPlayerSession && CurrentPlayerSession != SessionProxy)
        {
            if (!bWarnedDisplacedAudioProducer)
            {
                bWarnedDisplacedAudioProducer = true;
                CONVAI_LOG(ConvaiSubsystemLog, Warning,
                           TEXT("Refusing microphone audio from a displaced player session; "
                                "another component holds the session."));
            }
            return -1;
        }
    }

    // Pre-AEC audio: while the character is talking (plus an echo tail) the
    // chunk is mostly its own playback, so it is not evidence of the player.
    const double Now = FPlatformTime::Seconds();
    // Only once the character is ready: before that the server is not
    // listening yet, and not hearing the player means nothing.
    const uint64 LiveEpoch = ConnectEpoch.load(std::memory_order_acquire);
    if (NumFrames > 0 && !bMicWatchdogServerHeardUser && !bMicWatchdogReconnected
        && (ReadyEpoch.load() == LiveEpoch || HandshakeGaveUpEpoch.load() == LiveEpoch)
        && Now >= MicWatchdogBotSpeakingUntil.load())
    {
        int64 SumSquares = 0;
        for (size_t i = 0; i < NumFrames; ++i)
        {
            const int64 Sample = AudioData[i];
            SumSquares += Sample * Sample;
        }
        const double Rms = FMath::Sqrt(static_cast<double>(SumSquares) / NumFrames);
        // Floor = quietest recent chunk: drops instantly, climbs ~10 %/s so a
        // sentence cannot drag it up to its own level.
        const double PrevFloor = MicWatchdogNoiseFloor.load();
        const double Floor = PrevFloor <= 0.0 ? Rms : FMath::Min(Rms, PrevFloor * MicWatchdogFloorRisePerChunk + 1.0);
        MicWatchdogNoiseFloor = Floor;
        if (Rms >= FMath::Max(MicWatchdogSpeechRms, Floor * MicWatchdogSpeechOverFloor))
        {
            MicWatchdogLastSpeechTime = Now;
            const int64 SpeechFrames = (MicWatchdogSpeechFrames += static_cast<int64>(NumFrames));
            if (SpeechFrames >= MicWatchdogMinSpeechFrames && !bMicWatchdogArmed.exchange(true))
            {
                TWeakObjectPtr<UConvaiSubsystem> WeakThis(const_cast<UConvaiSubsystem*>(this));
                AsyncTask(ENamedThreads::GameThread, [WeakThis, LiveEpoch]()
                {
                    if (UConvaiSubsystem* Subsystem = WeakThis.Get())
                    {
                        Subsystem->MicWatchdogScheduleCheck(MicWatchdogSilenceSeconds, LiveEpoch);
                    }
                });
            }
        }
    }

    ConvaiClient->SendAudio(AudioData, NumFrames);

    return 0;
}

namespace
{
    /** Sustained speech that counts as the player talking again. Placeholders
     *  until the soak: room noise is what they trade against. */
    constexpr double kMicWakeSpeechSeconds = 0.5;
    /** A gap longer than this starts the count again: separate noises are not
     *  a sentence. */
    constexpr double kMicWakeMaxGapSeconds = 0.25;
    constexpr double kIdleWakeHoldSeconds = 60.0;

    double IdleWakeHoldSeconds()
    {
        const FString Override = UCommandLineUtils::GetCommandLineFlagValueAsString(
            TEXT("ConvaiIdleWakeHoldSeconds"), FString());
        return Override.IsEmpty() ? kIdleWakeHoldSeconds : FMath::Max(0.0, FCString::Atod(*Override));
    }
}

void UConvaiSubsystem::FeedMicWake(const int16_t* AudioData, size_t NumFrames) const
{
    if (!AudioData || NumFrames == 0)
    {
        return;
    }
    int64 SumSquares = 0;
    for (size_t i = 0; i < NumFrames; ++i)
    {
        const int64 Sample = AudioData[i];
        SumSquares += Sample * Sample;
    }
    const double Rms = FMath::Sqrt(static_cast<double>(SumSquares) / NumFrames);

    // The quietest recent chunk, as the watchdog tracks it - but with a real
    // "unset" rather than 0, which a digitally silent microphone reaches and
    // then treats as unset forever.
    const double PrevFloor = MicWakeFloor.load();
    const double Floor = PrevFloor < 0.0 ? Rms : FMath::Min(Rms, PrevFloor * MicWatchdogFloorRisePerChunk + 1.0);
    MicWakeFloor = Floor;

    // Measured in audio, not wall time: the chunks are the clock.
    const double Seconds = static_cast<double>(NumFrames) / ConvaiConstants::VoiceCaptureSampleRate;
    if (Rms < FMath::Max(MicWatchdogSpeechRms, Floor * MicWatchdogSpeechOverFloor))
    {
        const double Quiet = MicWakeQuietSeconds.load() + Seconds;
        MicWakeQuietSeconds = Quiet;
        if (Quiet > kMicWakeMaxGapSeconds)
        {
            MicWakeSpeechSeconds = 0.0;
        }
        return;
    }
    MicWakeQuietSeconds = 0.0;
    const double Total = MicWakeSpeechSeconds.load() + Seconds;
    MicWakeSpeechSeconds = Total;

    if (Total >= kMicWakeSpeechSeconds && !bMicWakePosted.exchange(true))
    {
        TWeakObjectPtr<UConvaiSubsystem> WeakThis(const_cast<UConvaiSubsystem*>(this));
        AsyncTask(ENamedThreads::GameThread, [WeakThis]()
        {
            if (UConvaiSubsystem* Subsystem = WeakThis.Get())
            {
                Subsystem->OnMicWakeEnergy();
            }
        });
    }
}

void UConvaiSubsystem::OnMicWakeEnergy()
{
    MicWakeSpeechSeconds = 0.0;
    bMicWakePosted = false;

    // Only a Dormant session is woken by a voice: anything else is either up,
    // already on its way back, or ended for good.
    if (ReconnectCoordinator.State() != EConvaiReconnectState::Dormant
        || FPlatformTime::Seconds() < IdleWakeHoldUntil)
    {
        return;
    }
    CONVAI_LOG(ConvaiSubsystemLog, Display, TEXT("The player is talking again; waking the dormant session"));
    WakeReconnect();
}

void UConvaiSubsystem::SendImage(const UConvaiConnectionSessionProxy* SessionProxy, const uint32 Width, const uint32 Height,
                                 TArray<uint8>& Data)
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }
    
    if (!bStartedPublishingVideo)
    {
        bStartedPublishingVideo = ConvaiClient->StartVideoPublishing(Width, Height);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Started video publishing"));
    }
    else
    {        
        ConvaiClient->SendImage(Width, Height, Data.GetData());
    }
}

void UConvaiSubsystem::StopVideoPublishing(const UConvaiConnectionSessionProxy* SessionProxy)
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }

    if (bStartedPublishingVideo)
    {
        ConvaiClient->StopVideoPublishing();
        bStartedPublishingVideo = false;
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Stopped video publishing"));
    }
}

namespace
{
    /** Held text is for a player who is waiting; past this, it is stale. */
    constexpr double kQueuedTextLifetimeSeconds = 30.0;
    constexpr int32 kQueuedTextCapacity = 8;
}

void UConvaiSubsystem::SendTextMessage(const UConvaiConnectionSessionProxy* SessionProxy, const FString& Message)
{
    if (!IsValid(SessionProxy))
    {
        return;
    }

    const uint64 Live = ConnectEpoch.load(std::memory_order_acquire);
    if (ConvaiClient && bIsConnected && (ReadyEpoch == Live || HandshakeGaveUpEpoch == Live))
    {
        SendUserText(Message);
        return;
    }

    // Typed while the character is coming up - connecting, or back from a
    // drop. Sent now it reached a bot that was not listening yet, or no
    // transport at all, and was lost without a word.
    const bool bSessionComing = CurrentConnectionState == EC_ConnectionState::Connecting
        || CurrentConnectionState == EC_ConnectionState::Connected
        || CurrentConnectionState == EC_ConnectionState::Reconnecting;
    const UConvaiConnectionSessionProxy* Owner =
        IsValid(CurrentCharacterSession) ? CurrentCharacterSession : ReconnectProxy.Get();
    if (!bSessionComing || !IsValid(Owner))
    {
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Text not sent: no character session is open"));
        return;
    }

    if (QueuedText.Num() >= kQueuedTextCapacity)
    {
        QueuedText.RemoveAt(0);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Text queue full: dropped the oldest held message"));
    }
    FQueuedText& Held = QueuedText.AddDefaulted_GetRef();
    Held.Text = Message;
    Held.QueuedAt = FPlatformTime::Seconds();
    Held.Owner = Owner;
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Holding text until the character is ready (%d held)"), QueuedText.Num());
}

bool UConvaiSubsystem::SendUserText(const FString& Message)
{
    if (!ConvaiClient)
    {
        return false;
    }

    TSharedRef<FJsonObject> DataJson = MakeShared<FJsonObject>();
    DataJson->SetStringField(TEXT("text"), Message);
    FString DataJsonStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJsonStr);
    FJsonSerializer::Serialize(DataJson, Writer);

    bool bSent = false;
#if WITH_TESTS
    if (!FConvaiConnectionTestSeam::TakeSend(DataJsonStr, bSent))
#endif
    {
        bSent = ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default,
            ConvaiConstants::WebRTC::MessageType::UserTextMessage, TCHAR_TO_UTF8(*DataJsonStr));
    }
    if (!bSent)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("The transport did not take the player's text"));
        return false;
    }

    MicWatchdogLastTextSendTime = FPlatformTime::Seconds();
    // Only text that reached the character is the player doing something: a
    // message lost to a dead transport must not keep an empty kiosk awake.
    MarkPlayerActivity();

    // When the user sends a text message, it interrupts the character session
    if (IsValid(CurrentCharacterSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> CharacterInterface = CurrentCharacterSession->GetConnectionInterface(); CharacterInterface.GetObject())
        {
            CharacterInterface->OnInterrupt();
        }
    }
    return true;
}

void UConvaiSubsystem::FlushQueuedText(bool bIgnoreAge)
{
    if (QueuedText.Num() == 0)
    {
        return;
    }
    const TArray<FQueuedText> Held = MoveTemp(QueuedText);
    QueuedText.Reset();

    const double Now = FPlatformTime::Seconds();
    int32 Delivered = 0;
    for (const FQueuedText& Item : Held)
    {
        // For another character's session, or waited too long: the player has
        // moved on, and delivering it now would answer a question nobody is
        // asking any more.
        if (Item.Owner.Get() != CurrentCharacterSession
            || (!bIgnoreAge && Now - Item.QueuedAt > kQueuedTextLifetimeSeconds))
        {
            continue;
        }
        Delivered += SendUserText(Item.Text) ? 1 : 0;
    }
    CONVAI_LOG(ConvaiSubsystemLog, Display,
        TEXT("Delivered %d queued text message(s) once the character was ready (%d dropped)"),
        Delivered, Held.Num() - Delivered);
}

void UConvaiSubsystem::DropQueuedText(const TCHAR* Why)
{
    if (QueuedText.Num() > 0)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Dropped %d held text message(s): %s"), QueuedText.Num(), Why);
        QueuedText.Reset();
    }
}

void UConvaiSubsystem::SendTriggerMessage(const UConvaiConnectionSessionProxy* SessionProxy,const FString& Trigger_Name, const FString& Trigger_Message) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }

    TSharedRef<FJsonObject> DataJson = MakeShared<FJsonObject>();
    if (!Trigger_Name.IsEmpty())
    {
        DataJson->SetStringField(TEXT("trigger_name"), Trigger_Name);
    }
    if (!Trigger_Message.IsEmpty())
    {
        DataJson->SetStringField(TEXT("trigger_message"), Trigger_Message);
    }
    FString DataJsonStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJsonStr);
    FJsonSerializer::Serialize(DataJson, Writer);

    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Sent trigger-message (name %s)"), *Trigger_Name);
    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::TriggerMessage, TCHAR_TO_UTF8(*DataJsonStr));
}

void UConvaiSubsystem::UpdateTemplateKeys(const UConvaiConnectionSessionProxy* SessionProxy,TMap<FString, FString> Template_Keys) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }
    
    TSharedRef<FJsonObject> KeysJson = MakeShared<FJsonObject>();
    for (const TPair<FString, FString>& Kv : Template_Keys)
    {
        KeysJson->SetStringField(Kv.Key, Kv.Value);
    }

#if WITH_TESTS
    if (FConvaiConnectionTestSeam::TakeMessage(TEXT("update-template-keys")))
    {
        return;
    }
#endif
    TSharedRef<FJsonObject> DataJson = MakeShared<FJsonObject>();
    DataJson->SetObjectField(TEXT("template_keys"), KeysJson);

    FString DataJsonStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJsonStr);
    FJsonSerializer::Serialize(DataJson, Writer);

    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::UpdateTemplateKeys, TCHAR_TO_UTF8(*DataJsonStr));
}

void UConvaiSubsystem::UpdateDynamicInfo(const UConvaiConnectionSessionProxy* SessionProxy,const FString& Context_Text) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }

#if WITH_TESTS
    if (FConvaiConnectionTestSeam::TakeMessage(TEXT("update-dynamic-info")))
    {
        return;
    }
#endif
    TSharedRef<FJsonObject> InnerJson = MakeShared<FJsonObject>();
    InnerJson->SetStringField(TEXT("text"), Context_Text);

    TSharedRef<FJsonObject> DataJson = MakeShared<FJsonObject>();
    DataJson->SetObjectField(TEXT("dynamic_info"), InnerJson);

    FString DataJsonStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJsonStr);
    FJsonSerializer::Serialize(DataJson, Writer);

    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::UpdateDynamicInfo, TCHAR_TO_UTF8(*DataJsonStr));
}

void UConvaiSubsystem::ToggleSTT(const UConvaiConnectionSessionProxy* SessionProxy, bool bMuted) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }

    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Sent stt-toggle (muted %s)"), bMuted ? TEXT("true") : TEXT("false"));
    const TSharedRef<FJsonObject> DataJson = MakeShared<FJsonObject>();
    DataJson->SetBoolField(TEXT("muted"), bMuted);
    FString DataJsonStr;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJsonStr);
    FJsonSerializer::Serialize(DataJson, Writer);

    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::STTToggle, TCHAR_TO_UTF8(*DataJsonStr));
}

void UConvaiSubsystem::ForceUserStoppedSpeaking(const UConvaiConnectionSessionProxy* SessionProxy) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }

    // A unique id per release lets the server reject an overlapping release instead of folding it
    // into the one still finalizing, which would lose the second capture without a word.
    const FString StopId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    const TSharedRef<FJsonObject> MessageJson = MakeShared<FJsonObject>();
    MessageJson->SetStringField(TEXT("id"), StopId);
    MessageJson->SetStringField(TEXT("label"), UTF8_TO_TCHAR(ConvaiConstants::WebRTC::label::Default));
    MessageJson->SetStringField(TEXT("type"), UTF8_TO_TCHAR(ConvaiConstants::WebRTC::MessageType::ForceUserStoppedSpeaking));
    MessageJson->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());
    FString MessageStr;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&MessageStr);
    FJsonSerializer::Serialize(MessageJson, Writer);

    ConvaiClient->SendRawMessage(TCHAR_TO_UTF8(*MessageStr));
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Sent force-user-stopped-speaking (id %s)"), *StopId);
}

void UConvaiSubsystem::UpdateContext(const UConvaiConnectionSessionProxy* SessionProxy, const FString& Text, EC_ContextUpdateMode Mode, EC_RunLLMOption ShouldRespond, const FConvaiObjectEntry* OptionalAttention) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }

    const TSharedRef<FJsonObject> DataJson = MakeShared<FJsonObject>();
    DataJson->SetStringField(TEXT("mode"), UTF8_TO_TCHAR(ContextUpdateModeToString(Mode)));
    DataJson->SetStringField(TEXT("run_llm"), UTF8_TO_TCHAR(RunLLMOptionToString(ShouldRespond)));

    if (Mode != EC_ContextUpdateMode::Reset || !Text.IsEmpty())
    {
        DataJson->SetStringField(TEXT("text"), Text);
    }

    if (OptionalAttention)
    {
        // Empty string is the documented "clear attention" signal. Non-empty Name lets the
        // server resolve the attention object; unresolved names are warned-and-ignored
        // server-side without clobbering the previous attention.
        DataJson->SetStringField(TEXT("current_attention_object"), OptionalAttention->Name);
    }

    FString DataJsonStr;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJsonStr);
    FJsonSerializer::Serialize(DataJson, Writer);

    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::ContextUpdate, TCHAR_TO_UTF8(*DataJsonStr));
}

void UConvaiSubsystem::UpdateSceneMetadata(const UConvaiConnectionSessionProxy* SessionProxy, const TArray<FConvaiObjectEntry>& SceneObjects) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }

    // Per server contract: data payload is a JSON array of {name, description}.
    TArray<TSharedPtr<FJsonValue>> Items;
    Items.Reserve(SceneObjects.Num());
    for (const FConvaiObjectEntry& Entry : SceneObjects)
    {
        if (Entry.Name.IsEmpty())
        {
            continue;
        }
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Entry.Name);
        Item->SetStringField(TEXT("description"), Entry.Description);
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }

    FString DataJsonStr;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJsonStr);
    FJsonSerializer::Serialize(Items, Writer);

    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::UpdateSceneMetadata, TCHAR_TO_UTF8(*DataJsonStr));
}

void UConvaiSubsystem::OnConnectAttemptFailed(uint64 Epoch)
{
    // Ensure delegate broadcast and cleanup happen on game thread since this callback may come from WebRTC thread
    TWeakObjectPtr<UConvaiSubsystem> WeakSubsystem(this);
    AsyncTask(ENamedThreads::GameThread, [WeakSubsystem, Epoch]()
    {
        if (UConvaiSubsystem* ValidSubsystem = WeakSubsystem.Get())
        {
            // The attempt that failed may already have been abandoned — a Stop
            // Session, or a newer attempt that replaced it. Ending the session
            // for a failure that is no longer anyone's is how a stale attempt
            // used to kill a live one.
            if (!ValidSubsystem->IsCurrentEpoch(Epoch))
            {
                CONVAI_LOG(ConvaiSubsystemLog, Log,
                    TEXT("Ignoring the failure of a connect attempt this session no longer owns"));
                return;
            }

            // A failed attempt ends a session as far as the game is concerned,
            // so a restart from one of these handlers is deferred like any
            // other.
            const FDisconnectBroadcastScope BroadcastScope(ValidSubsystem);

            ValidSubsystem->ResolveDisconnectReason(Epoch);
            ValidSubsystem->PendingAttendees.Reset();
            // A retry refused before it got a session (the reaper's cap, a
            // client that would not initialize) has no current session; it is
            // still the chain's attempt.
            UConvaiConnectionSessionProxy* const FailedProxy = IsValid(ValidSubsystem->CurrentCharacterSession)
                ? ValidSubsystem->CurrentCharacterSession
                : ValidSubsystem->ReconnectProxy.Get();

            // A failed attempt is decided like a drop: a retry that fails keeps
            // its chain going, and so does the first attempt of a Visit.
            if (IsValid(ValidSubsystem->CurrentCharacterSession))
            {
                ValidSubsystem->ReconnectProxy = ValidSubsystem->CurrentCharacterSession;
                ValidSubsystem->ReconnectCharacterID = ValidSubsystem->ConnectionManager
                    ? ValidSubsystem->ConnectionManager->ManagedCharacterID
                    : FString();
            }
            const bool bRetryPending =
                ValidSubsystem->TriggerReconnect(EConvaiReconnectTrigger::AttemptFailed);

            if (bRetryPending)
            {
                ValidSubsystem->CurrentConnectionState = EC_ConnectionState::Reconnecting;
            }
            else
            {
                ValidSubsystem->CurrentConnectionState = EC_ConnectionState::Disconnected;
                ValidSubsystem->BroadcastServerConnectionStateChanged(EC_ConnectionState::Disconnected);
            }

            if (ValidSubsystem->ConnectionManager)
            {
                ValidSubsystem->ConnectionManager->OnServerDisconnected(
                    FailedProxy, ValidSubsystem->ReconnectCoordinator.State());
            }

            // Cleanup on game thread to avoid race conditions
            ValidSubsystem->CleanupConvaiClient();
        }
    });
}

bool UConvaiSubsystem::InitializeConvaiClient()
{
    FScopeLock ClientLock(&ConvaiClientMutex);

    // Display, not Log: Log verbosity reaches Saved\Logs but not stdout, and
    // Saved\Logs keeps ten backups against a sweep of twenty-two launches --
    // so nine launches in ten had no record of which native library answered
    // for them. Which binary produced a result is not a debugging detail.
    const char* Version = convai::GetConvaiClientVersion();
    CONVAI_LOG(ConvaiSubsystemLog, Display, TEXT("ConvaiClient Version: %s"),
               UTF8_TO_TCHAR(Version));

    // If an old client exists, move it to a background task for deferred destruction.
    // The ConvaiClient destructor crashes if called while internal WebRTC threads
    // are still shutting down from a recent Disconnect(). By deferring destruction
    // to an async task, we give the internal threads time to finish.
    // A client still here has not been through CleanupConvaiClient; the reaper
    // takes it, along with its listener, so neither dies under a live thread.
    AbandonLiveClient();

    // Every client is its own generation, so a callback can say which client
    // it came from and the game thread can drop anything older.
    const uint64 Epoch = BumpConnectEpoch();

    ConvaiClient = MakeUnique<convai::ConvaiClient>();

    if (ConvaiClient)
    {
        ClientListenerShim = MakeShared<FConvaiClientListenerShim>(this, Epoch);
        SetupClientCallbacks();
        return true;
    }

    return false;
}

void UConvaiSubsystem::AbandonLiveClient()
{
    // The reference-audio capture holds a RAW pointer to the client and writes
    // to it from the audio render thread. It has to stop here, with the
    // release, rather than in one of the callers: the reaper really does
    // delete the client, and a caller that forgot left the render thread
    // writing into freed memory for the rest of the level.
    if (ReferenceAudioThread.IsValid())
    {
        ReferenceAudioThread->StopCapture();
        ReferenceAudioThread.Reset();
    }

    FScopeLock ClientLock(&ConvaiClientMutex);
    if (!ConnectionThread.IsValid() && !ConvaiClient)
    {
        return;
    }

    // The whole teardown - joining a connection thread that may be inside its
    // POST, and the Disconnect the transport allows five seconds for - goes to
    // the reaper. None of it happens on this thread. The successor's connect
    // thread waits for that Disconnect before its own POST, so the old server
    // session still closes first.
    if (ConvaiClient)
    {
        ConvaiClient->SetConvaiClientListner(nullptr);
    }

    FConvaiConnectReaper::FTriple Triple;
    Triple.Thread = MoveTemp(ConnectionThread);
    Triple.Client = TSharedPtr<convai::ConvaiClient>(ConvaiClient.Release());
    Triple.Shim = MoveTemp(ClientListenerShim);
    Triple.CharacterID = ConnectionManager ? ConnectionManager->ManagedCharacterID : FString();

    // Always accepted: the cap refuses the next CONNECT (see ConnectSession)
    // rather than the teardown, because a refused teardown would run on this
    // thread.
    FConvaiConnectReaper::Abandon(MoveTemp(Triple));
}

void UConvaiSubsystem::CleanupConvaiClient()
{
    // The client being torn down here owns the current epoch. Bumping it now
    // means anything that client still delivers — including the disconnect its
    // own Disconnect() is about to produce — is stale by construction.
    BumpConnectEpoch();

    // AbandonLiveClient stops the reference capture: it holds a raw pointer to
    // the client being released.
    AbandonLiveClient();

    // Clear the current character session
    {
        FScopeLock SessionLock(&SessionMutex);
        CurrentCharacterSession = nullptr;
    }

    bIsConnected = false;
    bStartedPublishingVideo = false;
    MicWatchdogReset();
}

void UConvaiSubsystem::SetupClientCallbacks()
{
    if (!ConvaiClient || !ClientListenerShim.IsValid())
    {
        return;
    }
    ConvaiClient->SetConvaiClientListner(ClientListenerShim.Get());
}

UConvaiSubsystem::FConvaiAECStats UConvaiSubsystem::GetAECStats() const
{
    FConvaiAECStats Stats;
    if (!ConvaiClient)
    {
        return Stats;
    }

#if !CONVAI_HAS_AEC_STATS_API
    // No stats source in this binary drop; bValid stays false, which callers
    // already treat as "no canceller active".
    return Stats;
#else
    convai::ConvaiAECStats Raw;
    if (!ConvaiClient->GetAECStats(Raw))
    {
        return Stats;
    }

    Stats.bValid = true;
    Stats.MicChunks = static_cast<int64>(Raw.mic_chunks);
    Stats.ReferenceChunks = static_cast<int64>(Raw.reference_chunks);
    Stats.ReferenceCalls = static_cast<int64>(Raw.reference_calls);
    Stats.PublishedFrames = static_cast<int64>(Raw.published_frames);
    Stats.DroppedFrames = static_cast<int64>(Raw.dropped_frames);
    Stats.QueueDepth = static_cast<int64>(Raw.queue_depth);
    Stats.LastRmsIn = Raw.last_rms_in;
    Stats.LastRmsOut = Raw.last_rms_out;
    return Stats;
#endif
}

UConvaiSubsystem::FReferenceAudioStatus UConvaiSubsystem::GetReferenceAudioStatus() const
{
    FReferenceAudioStatus Status;
    if (!ReferenceAudioThread.IsValid())
    {
        return Status;
    }

    Status.bCapturing = ReferenceAudioThread->IsCapturing();
    Status.Feed = ReferenceAudioThread->GetStats();

    // The capture exists only while a client does, and there is one of each.
    FScopeLock ClientLock(&ConvaiClientMutex);
    Status.ClientCount = ConvaiClient ? 1 : 0;
    return Status;
}

void UConvaiSubsystem::BroadcastServerConnectionStateChanged(EC_ConnectionState State)
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
        AsyncTask(ENamedThreads::GameThread, [WeakThis, State]()
        {
            if (UConvaiSubsystem* Subsystem = WeakThis.Get())
            {
                Subsystem->OnServerConnectionStateChangedEvent.Broadcast(State);
            }
        });
        return;
    }

    // Already on game thread - broadcast directly
    OnServerConnectionStateChangedEvent.Broadcast(State);
}

void UConvaiSubsystem::OnTransportConnected(uint64 Epoch, const FString& SessionID, const FString& CharSessionID)
{
    if (!IsCurrentEpoch(Epoch))
    {
        return;
    }
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnConnectedToServer called SessionID: %s, CharSessionID: %s"), *SessionID, *CharSessionID);

    if (!ConvaiClient)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("OnConnectedToServer: ConvaiClient is null"));
        return;
    }

    if (!IsValid(CurrentCharacterSession))
    {
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnConnectedToServer: CurrentCharacterSession is invalid"));
        CleanupConvaiClient();
        return;
    }

    MicWatchdogReset();
    bIsConnected = true;
    if (!ConvaiClient->StartAudioPublishing())
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("Microphone track not published yet (server did not accept it); the native client keeps retrying"));
    }
    
    // Start reference audio capture for echo cancellation
    if (!ReferenceAudioThread.IsValid() && UConvaiUtils::IsAECEnabled())
    {
        if (UWorld* World = GetWorld())
        {
            ReferenceAudioThread = MakeShared<FConvaiReferenceAudioThread>(ConvaiClient.Get(), World);
            ReferenceAudioThread->StartCapture();
            CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Started reference audio capture thread"));
        }
        else
        {
            CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("Could not get World for reference audio capture"));
        }
    }
    
    // Ensure delegate broadcast happens on game thread since this callback comes from WebRTC thread
    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis, Epoch, SessionID]()
    {
        GetAndroidMicPermission();
        
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            // The client that connected may have been abandoned while this task
            // sat in the queue; announcing it would announce a dead session.
            if (!Subsystem->IsCurrentEpoch(Epoch))
            {
                return;
            }
            Subsystem->LiveSessionID = SessionID;

            // The AFK clock is NOT reset here. A reconnect opens a new session
            // for the same player, and restarting their clock on every connect
            // meant a kiosk nobody is standing at never idled out: each
            // reconnect bought another full AFK window. The clock belongs to
            // the Visit, and BeginVisit is what starts one.
            //
            // What the last session knew about its own coming close is not
            // this session's: a Reopen Immediately reopen must not read its first drop
            // as idle.
            Subsystem->bIdleDisconnectPending = false;
            Subsystem->bRenewalAwaitingAck = false;
            Subsystem->PendingServerIdle.store(-1);

            // Held attendees join the session before the Connected broadcast, so a listener that
            // re-reads the session state there sees Connected. They are announced further down.
            if (IsValid(Subsystem->CurrentCharacterSession))
            {
                for (const FString& AttendeeId : Subsystem->PendingAttendees)
                {
                    Subsystem->CurrentCharacterSession->SetAttendeeId(AttendeeId);
                }
            }

            // Broadcast connection state change to subsystem level
            Subsystem->CurrentConnectionState = EC_ConnectionState::Connected;
            Subsystem->BroadcastServerConnectionStateChanged(EC_ConnectionState::Connected);

            Subsystem->StartClientReadyHandshake(Epoch);

            if (IsValid(Subsystem->CurrentCharacterSession))
            {
                if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
                {
                    Interface->OnConnectedToServer();
                }
            }
            
            if (IsValid(Subsystem->CurrentPlayerSession))
            {
                if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
                {
                    Interface->OnConnectedToServer();
                }
            }

            // Moved out first: a listener can reconnect from inside OnAttendeeConnected,
            // and ConnectSession resets the list.
            const TArray<FString> HeldAttendees = MoveTemp(Subsystem->PendingAttendees);
            for (const FString& AttendeeId : HeldAttendees)
            {
                Subsystem->NotifyAttendeeConnected(AttendeeId);
            }
        }
    });
}

void UConvaiSubsystem::OnTransportDisconnected(uint64 Epoch)
{
    if (!IsCurrentEpoch(Epoch))
    {
        // A client the game already let go. Acting on this is what used to
        // kill the session that had replaced it.
        CONVAI_LOG(ConvaiSubsystemLog, Log,
            TEXT("Ignoring a disconnect from a client this session no longer owns"));
        return;
    }

    CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Disconnected from Server"));
    bIsConnected = false;

    // Copy and clear the connected attendees set (thread-safe)
    TSet<FString> AttendeesToDisconnect;
    {
        FScopeLock Lock(&AttendeeMutex);
        AttendeesToDisconnect = MoveTemp(ConnectedAttendees);
        ConnectedAttendees.Empty();
    }

    // Ensure delegate broadcast and cleanup happen on game thread since this callback comes from WebRTC thread
    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis, AttendeesToDisconnect, Epoch]()
    {
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            Subsystem->EndSession(Epoch, AttendeesToDisconnect, EConvaiReconnectTrigger::Drop);
        }
    });
}

void UConvaiSubsystem::EndSession(uint64 Epoch, const TSet<FString>& AttendeesToDisconnect, EConvaiReconnectTrigger Trigger)
{
    if (!IsCurrentEpoch(Epoch))
    {
        return;
    }

    // The reason is settled here, before a single handler runs.
    // Resolving it after the attendee loop meant the first handler a
    // game wrote read the previous session's verdict, and a restart
    // from inside that loop could relabel an idle close as explicit.
    ResolveDisconnectReason(Epoch);

    // The state moves with the reason, for the same reason: a handler
    // that reads Get Server Connection State from inside the drop must
    // not be told the session is still up, and a restart that stops
    // "the session that dropped" must find it already ended.
    CurrentConnectionState = EC_ConnectionState::Disconnected;
    PendingAttendees.Reset();

    // Everything below tells the game the session ended, and a game
    // that restarts its session from one of those events must not do
    // it on this stack. See FDisconnectBroadcastScope.
    const FDisconnectBroadcastScope BroadcastScope(this);

    StopClientReadyHandshake();

    // Stop reference audio capture when disconnected (must be on game thread)
    if (ReferenceAudioThread.IsValid())
    {
        ReferenceAudioThread->StopCapture();
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Stopped reference audio capture on disconnect"));
    }

    // The cause an attendee handler reads is this ending's.
    LastDisconnectCause = PendingDisconnectCause.load();

    // Known before any handler runs, so a Start Session from inside one can
    // tell whether it is for the session that just ended.
    UConvaiConnectionSessionProxy* const DroppedProxy = CurrentCharacterSession;
    ReconnectProxy = CurrentCharacterSession;
    ReconnectCharacterID = ConnectionManager
        ? ConnectionManager->ManagedCharacterID
        : FString();

    // Fire OnAttendeeDisconnected for each attendee that was connected
    for (const FString& AttendeeId : AttendeesToDisconnect)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🔌 Attendee disconnected (server disconnect): %s"), *AttendeeId);

        if (IsValid(CurrentCharacterSession))
        {
            if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
            {
                Interface->OnAttendeeDisconnected(AttendeeId);
            }
        }

        if (IsValid(CurrentPlayerSession))
        {
            if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
            {
                Interface->OnAttendeeDisconnected(AttendeeId);
            }
        }
    }

    // What the game is told depends on what happens next: a session
    // that is coming back reports Reconnecting once for the whole
    // chain, and Disconnected is reserved for a session that ended.
    const bool bRetryPending = TriggerReconnect(Trigger);
    if (ReconnectCoordinator.State() == EConvaiReconnectState::Dormant
        && LastDisconnectReason == EC_DisconnectReason::Idle)
    {
        IdleWakeHoldUntil = FPlatformTime::Seconds() + IdleWakeHoldSeconds();
    }

    // Announced here, after the attendees have left: the order a game
    // sees is unchanged. A later drop in a chain already announced
    // stays Reconnecting without saying so twice.
    if (bRetryPending)
    {
        CurrentConnectionState = EC_ConnectionState::Reconnecting;
    }
    else
    {
        BroadcastServerConnectionStateChanged(EC_ConnectionState::Disconnected);
    }
    
    if (IsValid(CurrentCharacterSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnDisconnectedFromServer();
        }
    }

    if (IsValid(CurrentPlayerSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnDisconnectedFromServer();
        }
    }
    
    if (ConnectionManager)
    {
        ConnectionManager->OnServerDisconnected(DroppedProxy, ReconnectCoordinator.State());
    }

    // Cleanup on game thread to avoid race conditions with callbacks
    CleanupConvaiClient();
}

void UConvaiSubsystem::OnTransportAudioData(uint64 Epoch, const char* attendee_id,
                                   const int16_t* audio_data, size_t num_frames,
                                   uint32_t sample_rate, uint32_t bits_per_sample, uint32_t num_channels)
{
    if (!IsCurrentEpoch(Epoch))
    {
        return;
    }

    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }
    
    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnAudioDataReceived(audio_data, num_frames, sample_rate, bits_per_sample, num_channels);
    }
}

void UConvaiSubsystem::OnTransportAttendeeConnected(uint64 Epoch, const FString& Attendee)
{
    if (!IsCurrentEpoch(Epoch))
    {
        return;
    }
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🔌 Attendee connected: %s"), *Attendee);

    {
        FScopeLock Lock(&AttendeeMutex);
        ConnectedAttendees.Add(Attendee);
    }

    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis, Attendee, Epoch]()
    {
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            if (!Subsystem->IsCurrentEpoch(Epoch))
            {
                return;
            }

            // The native client can report an attendee before OnConnectedToServer. Announced
            // now, a listener reads the session as not connected and is never told again. The
            // attendee id is held too, or the session getters read Connected before the
            // listeners have been told.
            if (Subsystem->CurrentConnectionState != EC_ConnectionState::Connected)
            {
                CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Attendee %s held until the server connection completes"), *Attendee);
                Subsystem->PendingAttendees.AddUnique(Attendee);
                return;
            }

            if (IsValid(Subsystem->CurrentCharacterSession))
            {
                Subsystem->CurrentCharacterSession->SetAttendeeId(Attendee);
            }
            Subsystem->NotifyAttendeeConnected(Attendee);
            Subsystem->ReconnectCoordinator.OnBotReturned();
        }
    });
}

void UConvaiSubsystem::NotifyAttendeeConnected(const FString& AttendeeId) const
{
    if (IsValid(CurrentCharacterSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnAttendeeConnected(AttendeeId);
        }
    }

    if (IsValid(CurrentPlayerSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnAttendeeConnected(AttendeeId);
        }
    }
}

void UConvaiSubsystem::OnTransportAttendeeDisconnected(uint64 Epoch, const FString& Attendee)
{
    if (!IsCurrentEpoch(Epoch))
    {
        return;
    }
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🔌 Attendee disconnected: %s"), *Attendee);

    {
        FScopeLock Lock(&AttendeeMutex);
        ConnectedAttendees.Remove(Attendee);
    }

    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis, Attendee, Epoch]()
    {
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            if (!Subsystem->IsCurrentEpoch(Epoch))
            {
                return;
            }

            Subsystem->PendingAttendees.Remove(Attendee);

            // A game restarting its session from the character's departure does
            // it next tick, as from any other disconnect event.
            const FDisconnectBroadcastScope BroadcastScope(Subsystem);

            if (IsValid(Subsystem->CurrentCharacterSession))
            {
                if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
                {
                    Interface->OnAttendeeDisconnected(Attendee);
                }
            }

            if (IsValid(Subsystem->CurrentPlayerSession))
            {
                if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
                {
                    Interface->OnAttendeeDisconnected(Attendee);
                }
            }

            // The character's participant left and the transport did not: a
            // full transport restart, or the character gone. Only the window
            // can tell which.
            bool bNobodyLeft = false;
            {
                FScopeLock Lock(&Subsystem->AttendeeMutex);
                bNobodyLeft = Subsystem->ConnectedAttendees.Num() == 0;
            }
            if (bNobodyLeft && Subsystem->bIsConnected
                && Subsystem->CurrentConnectionState == EC_ConnectionState::Connected
                && IsValid(Subsystem->CurrentCharacterSession))
            {
                Subsystem->ReconnectProxy = Subsystem->CurrentCharacterSession;
                Subsystem->ReconnectCharacterID = Subsystem->ConnectionManager
                    ? Subsystem->ConnectionManager->ManagedCharacterID
                    : FString();
                Subsystem->BotAbsentEpoch = Epoch;
                Subsystem->ReconnectCoordinator.OnBotLeft();
            }
        }
    });
}

void UConvaiSubsystem::OnTransportActiveSpeakerChanged(uint64 Epoch, const FString& SpeakerStr)
{
    if (!IsCurrentEpoch(Epoch))
    {
        return;
    }
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🎤 Active speaker changed: %s"), *SpeakerStr);
}

void UConvaiSubsystem::OnTransportDataPacket(uint64 Epoch, const FString& JsonStr, const FString& Attendee)
{
    if (!IsCurrentEpoch(Epoch))
    {
        return;
    }
    
	if (!JsonStr.Contains("blendshape"))
	{
		CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Attendee ID: %s, Data: %s"), *Attendee, *JsonStr);
	}
    
    const TSharedPtr<FJsonObject> Root = ParseJsonObject(JsonStr);
    if (!Root.IsValid())
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("OnDataPacketReceived: Failed to parse Root JSON."));
        return;
    }

    FString DataPacketTypeStr;
    if (!Root->TryGetStringField(TEXT("type"), DataPacketTypeStr))
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("OnDataPacketReceived: type filed missing in root json"));
        return;
    }

    const EC_PacketType DataPacketType = ToPacketType(DataPacketTypeStr);
    const TSharedPtr<FJsonObject> DataObj = GetDataObject(Root);

    switch (DataPacketType)
    {
        case EC_PacketType::UserStartedSpeaking:
            {
                MicWatchdogServerHeardUser();
                OnUserStartedSpeaking(TCHAR_TO_UTF8(*Attendee));
            }
            break;

        case EC_PacketType::UserStoppedSpeaking:
            {
                OnUserStoppedSpeaking(TCHAR_TO_UTF8(*Attendee));
            }
            break;

        case EC_PacketType::UserTranscription:
        {
            MicWatchdogServerHeardUser();
            if (DataObj.IsValid())
            {
                FString Text, Timestamp;
                bool bFinal = false;

                GetStringSafe(DataObj, TEXT("text"),      Text);
                GetStringSafe(DataObj, TEXT("timestamp"), Timestamp);
                GetBoolSafe  (DataObj, TEXT("final"),     bFinal);

                OnUserTranscript(
                    TCHAR_TO_UTF8(*Text),
                    TCHAR_TO_UTF8(*Attendee),
                    bFinal,
                    TCHAR_TO_UTF8(*Timestamp)
                );
            }
        }
            break;

        case EC_PacketType::BotLLMStarted:
            {
                FString ResponseId;
                if (!GetStringSafe(Root, TEXT("response_id"), ResponseId))
                {
                    GetStringSafe(DataObj, TEXT("response_id"), ResponseId);
                }
                OnBotLLMStarted(TCHAR_TO_UTF8(*Attendee), ResponseId);
            }
            break;

        case EC_PacketType::BotLLMStopped:
            {
                OnBotLLMStopped(TCHAR_TO_UTF8(*Attendee));
            }
            break;

        case EC_PacketType::BotStartedSpeaking:
            {
                FString ResponseId;
                if (!GetStringSafe(Root, TEXT("response_id"), ResponseId))
                {
                    GetStringSafe(DataObj, TEXT("response_id"), ResponseId);
                }
                OnBotStartedSpeaking(TCHAR_TO_UTF8(*Attendee), ResponseId);
            }
            break;

        case EC_PacketType::BotStoppedSpeaking:
            {
                FString ResponseId;
                if (!GetStringSafe(Root, TEXT("response_id"), ResponseId))
                {
                    GetStringSafe(DataObj, TEXT("response_id"), ResponseId);
                }
                OnBotStoppedSpeaking(TCHAR_TO_UTF8(*Attendee), ResponseId);
            }
            break;

        case EC_PacketType::BotTurnCompleted:
            {
                FString ResponseId;
                FString ErrorReason;
                bool bWasInterrupted = false;
                bool bWasAborted = false;

                if (!GetStringSafe(Root, TEXT("response_id"), ResponseId))
                {
                    GetStringSafe(DataObj, TEXT("response_id"), ResponseId);
                }
                if (!GetBoolSafe(Root, TEXT("was_interrupted"), bWasInterrupted))
                {
                    GetBoolSafe(DataObj, TEXT("was_interrupted"), bWasInterrupted);
                }
                if (!GetBoolSafe(Root, TEXT("was_aborted"), bWasAborted))
                {
                    GetBoolSafe(DataObj, TEXT("was_aborted"), bWasAborted);
                }
                if (!GetStringSafe(Root, TEXT("error_reason"), ErrorReason))
                {
                    GetStringSafe(DataObj, TEXT("error_reason"), ErrorReason);
                }

                OnBotTurnCompleted(TCHAR_TO_UTF8(*Attendee), ResponseId,
                    bWasInterrupted, bWasAborted, ErrorReason);
            }
            break;

        case EC_PacketType::BotTranscription:
        {
            if (DataObj.IsValid())
            {
                FString Text;
                if (GetStringSafe(DataObj, TEXT("text"), Text) && !Text.IsEmpty())
                {
                    OnBotTranscript(TCHAR_TO_UTF8(*Text), TCHAR_TO_UTF8(*Attendee));
                }
            }
        }
            break;

        case EC_PacketType::ServerMessage:
            {
                if (DataObj.IsValid())
                {
                    FString ServerPacketTypeStr;
                    DataObj->TryGetStringField(TEXT("type"), ServerPacketTypeStr);
                    
                    switch (const EC_ServerPacketType ServerPacketType = ToServerPacketType(ServerPacketTypeStr))
                    {
                    case EC_ServerPacketType::BotEmotion:
                        {
                            if (DataObj.IsValid())
                            {                    
                                FString EmotionType;
                                int32 EmotionScale = 2;
                                GetStringSafe(DataObj, TEXT("emotion"),      EmotionType);
                                DataObj->TryGetNumberField(TEXT("scale"), EmotionScale);
                                const FString EmotionResponse = FString::Printf(TEXT("%s %d"), *EmotionType, EmotionScale);
                                OnEmotionReceived(EmotionResponse, FAnimationFrame(), false);
                            }
                        }
                        break;
                        
                    case EC_ServerPacketType::ActionResponse:
                        {
                            // Server contract: actions is an array of {name, target?} objects.
                            // Empty array is the no-op signal — there is no "None" sentinel.
                            if (!DataObj.IsValid())
                            {
                                break;
                            }
                            const TArray<TSharedPtr<FJsonValue>>* ActionsArray = nullptr;
                            if (!DataObj->TryGetArrayField(TEXT("actions"), ActionsArray) || ActionsArray == nullptr)
                            {
                                break;
                            }

                            // Look up the registered chatbot once so the parser can match against
                            // its configured action templates and resolve env objects/characters.
                            const UConvaiChatbotComponent* Chatbot = nullptr;
                            if (IsValid(CurrentCharacterSession))
                            {
                                if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
                                {
                                    Chatbot = Cast<UConvaiChatbotComponent>(Interface.GetObject());
                                }
                            }
                            // Empty fallback env so CoerceParam still works when no chatbot is bound.
                            static const FConvaiEnvironmentData EmptyEnv;
                            const FConvaiEnvironmentData& Env = Chatbot ? Chatbot->EnvironmentData : EmptyEnv;

                            // Materialize the template list once for the whole sequence:
                            // the contract frozen at /connect and published on the PROXY
                            // (the server can only send what was advertised) — read from
                            // the proxy, not the component, so an orphan rebind can't
                            // race parsing against the new owner's live list, and live
                            // toggle edits can't desync parsing from the wire config.
                            // Template pointers below point into this array; it must
                            // outlive the loop.
                            TArray<FConvaiAction> EffectiveActions;
                            if (IsValid(CurrentCharacterSession) && CurrentCharacterSession->HasAdvertisedActions())
                            {
                                TArray<FString> AdvertisedBuiltIns;
                                CurrentCharacterSession->GetAdvertisedActions(EffectiveActions, AdvertisedBuiltIns);
                            }
                            else if (Chatbot != nullptr)
                            {
                                // Never-connected fallback — unreachable in practice
                                // (no contract means no server-side actions to receive).
                                Chatbot->GetActionsForParsing(EffectiveActions);
                            }

                            // Local: backfill the deprecated mirror fields after parsing.
                            auto PopulateDeprecatedMirrors = [](FConvaiResultAction& Result)
                            {
                                bool bFilledRef = false;
                                bool bFilledNum = false;
                                bool bFilledTxt = false;
                                Result.ConvaiExtraParams = FConvaiExtraParams();
                                for (const auto& Pair : Result.Parameters)
                                {
                                    const FConvaiResultParam& P = Pair.Value;
                                    if (!bFilledRef && !P.RefValue.Name.IsEmpty())
                                    {
                                        Result.RelatedObjectOrCharacter = P.RefValue;
                                        bFilledRef = true;
                                    }
                                    if (!bFilledNum && P.NumberValue != 0.f)
                                    {
                                        Result.ConvaiExtraParams.Number = P.NumberValue;
                                        bFilledNum = true;
                                    }
                                    if (!bFilledTxt && (P.Type == EConvaiActionParamType::String) && !P.StringValue.IsEmpty())
                                    {
                                        Result.ConvaiExtraParams.Text = P.StringValue;
                                        bFilledTxt = true;
                                    }
                                    Result.ConvaiExtraParams.NamedParams.Add(Pair.Key, P.StringValue);
                                }
                            };

                            TArray<FConvaiResultAction> Sequence;
                            Sequence.Reserve(ActionsArray->Num());
                            for (const TSharedPtr<FJsonValue>& Value : *ActionsArray)
                            {
                                if (!Value.IsValid() || Value->Type != EJson::Object)
                                {
                                    continue;
                                }
                                const TSharedPtr<FJsonObject> ActionObj = Value->AsObject();
                                if (!ActionObj.IsValid())
                                {
                                    continue;
                                }
                                FConvaiResultAction Result;
                                FString RawName;
                                GetStringSafe(ActionObj, TEXT("name"),   RawName);
                                FString Target;
                                GetStringSafe(ActionObj, TEXT("target"), Target);
                                Result.Action = RawName;
                                Result.ActionString = Target.IsEmpty()
                                    ? RawName
                                    : FString::Printf(TEXT("%s %s"), *RawName, *Target);

                                const FConvaiAction* Tpl = nullptr;
                                if (Chatbot != nullptr)
                                {
                                    Tpl = UConvaiActions::FindActionTemplate(RawName, EffectiveActions);
                                }

                                if (Tpl)
                                {
                                    Result.Action = Tpl->Name;

                                    // Mirror the wait-for-speech policy from the template so the chatbot
                                    // can decide at dispatch time without re-resolving the template.
                                    // Delay collapses to 0 unless bWaitForBotSpeech is set, regardless
                                    // of any stale persisted value.
                                    Result.bWaitForBotSpeech = Tpl->bWaitForBotSpeech;
                                    Result.DelayAfterBotSpeechSec = Tpl->bWaitForBotSpeech ? Tpl->DelayAfterBotSpeechSec : 0.0f;

                                    const FString NameLeftover = UConvaiActions::StripActionPrefix(RawName, Tpl->Name);
                                    const FString Combined =
                                        NameLeftover.IsEmpty() ? Target :
                                        Target.IsEmpty()       ? NameLeftover :
                                        (NameLeftover + TEXT(" ") + Target);

                                    if (Tpl->Parameters.Num() > 0)
                                    {
                                        const TArray<FString> Values = UConvaiActions::SplitParamValues(Combined, Tpl->Parameters);
                                        for (int32 i = 0; i < FMath::Min(Tpl->Parameters.Num(), Values.Num()); ++i)
                                        {
                                            const FConvaiActionParam& Decl = Tpl->Parameters[i];
                                            // LLM sometimes mimics the prompt's `{name: type}` form by emitting
                                            // `{name: value}` — strip the leading "name:" so we coerce the value.
                                            const FString CleanValue = UConvaiActions::StripParamNameMimicry(
                                                Values[i], Decl.Name, &Env);
                                            bool bConstraintMatched = true;
                                            FConvaiResultParam Param = UConvaiActions::CoerceParam(
                                                CleanValue, Decl.Type, Env, Decl.EnumType, &Decl.Choices, &bConstraintMatched);

                                            // Constraint warnings — value still flows through; handler decides.
                                            if (CleanValue.IsEmpty())
                                            {
                                                CONVAI_LOG(ConvaiSubsystemLog, Warning,
                                                    TEXT("Action '%s' omitted declared parameter '%s'; the client cannot infer a missing server value."),
                                                    *Tpl->Name, *Decl.Name);
                                            }
                                            else if (Decl.Type == EConvaiActionParamType::Enum && !Decl.EnumType)
                                            {
                                                CONVAI_LOG(ConvaiSubsystemLog, Warning,
                                                    TEXT("Action '%s' param '%s' has Type=Enum but EnumType is unset — choice validation skipped."),
                                                    *Tpl->Name, *Decl.Name);
                                            }
                                            else if (!bConstraintMatched)
                                            {
                                                if (Decl.Type == EConvaiActionParamType::Enum)
                                                {
                                                    CONVAI_LOG(ConvaiSubsystemLog, Warning,
                                                        TEXT("Action '%s' param '%s' value '%s' not in declared Enum '%s'."),
                                                        *Tpl->Name, *Decl.Name, *Param.StringValue, *Decl.EnumType->GetName());
                                                }
                                                else
                                                {
                                                    CONVAI_LOG(ConvaiSubsystemLog, Warning,
                                                        TEXT("Action '%s' param '%s' value '%s' not in declared Choices."),
                                                        *Tpl->Name, *Decl.Name, *Param.StringValue);
                                                }
                                            }

                                            Result.Parameters.Add(Decl.Name, Param);
                                        }
                                    }
                                    else if (!Combined.IsEmpty())
                                    {
                                        // No declared params, but server sent a value — surface it under "target".
                                        FConvaiResultParam Param = UConvaiActions::CoerceParam(Combined, EConvaiActionParamType::Auto, Env);
                                        Result.Parameters.Add(TEXT("target"), Param);
                                    }
                                }
                                else if (!Target.IsEmpty())
                                {
                                    // No template matched — still surface the target so handlers see something.
                                    FConvaiResultParam Param = UConvaiActions::CoerceParam(Target, EConvaiActionParamType::Auto, Env);
                                    Result.Parameters.Add(TEXT("target"), Param);
                                }

                                PopulateDeprecatedMirrors(Result);
                                Sequence.Add(MoveTemp(Result));
                            }
                            DispatchActionSequence(Sequence);
                        }
                        break;

                    case EC_ServerPacketType::BTResponse:
                        {
                            if (DataObj.IsValid())
                            {
                                FString BT_Code, BT_Constants, NarrativeSectionID;
                                GetStringSafe(DataObj, TEXT("bt_code"),      BT_Code);
                                GetStringSafe(DataObj, TEXT("bt_constants"),      BT_Constants);
                                GetStringSafe(DataObj, TEXT("narrative_section_id"),      NarrativeSectionID);
                                OnNarrativeSectionReceived(BT_Code, BT_Constants, NarrativeSectionID);
                            }
                        }
                        break;
                        
                    case EC_ServerPacketType::ModerationResponse:
                        {
                            CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("OnDataPacketReceived: ModerationResponse"));
                        }
                        break;
                        
                    case EC_ServerPacketType::Visemes:
                        {
                            if (DataObj.IsValid())
                            {
                                FAnimationSequence VisemeAnimationSequence;
                                ConvertVisemeDataToAnimationSequence(DataObj, VisemeAnimationSequence);
                                OnFaceDataReceived(VisemeAnimationSequence);
                            }
                        }
                        break;

                    case EC_ServerPacketType::NeurosyncBlendshapes:
                        {
                            if (DataObj.IsValid())
                            {
                                FAnimationSequence BlendshapeAnimationSequence;
                                ConvertBlendshapeDataToAnimationSequence(DataObj, BlendshapeAnimationSequence, false);
                                const int32 BatchFrameCount = BlendshapeAnimationSequence.AnimationFrames.Num();
                                ReceivedBlendshapeFrameCount += BatchFrameCount;
                                LastBlendshapeFrameTime = FPlatformTime::Seconds();
                                #if ConvaiDebugMode
                                CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[Blendshapes] Batch received: %d frames | Total: %d"), BatchFrameCount, ReceivedBlendshapeFrameCount);
                                #endif
                                OnFaceDataReceived(BlendshapeAnimationSequence);
                            }
                        }
                        break;

                    case EC_ServerPacketType::ChunkedNeurosyncBlendshapes:
                        {
                            if (DataObj.IsValid())
                            {
                                FAnimationSequence BlendshapeAnimationSequence;
                                ConvertBlendshapeDataToAnimationSequence(DataObj, BlendshapeAnimationSequence, true);
                                const int32 BatchFrameCount = BlendshapeAnimationSequence.AnimationFrames.Num();
                                ReceivedBlendshapeFrameCount += BatchFrameCount;
                                LastBlendshapeFrameTime = FPlatformTime::Seconds();
                                #if ConvaiDebugMode
                                CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[Blendshapes] Batch received: %d frames | Total: %d"), BatchFrameCount, ReceivedBlendshapeFrameCount);
                                #endif
                                OnFaceDataReceived(BlendshapeAnimationSequence);
                            }
                        }
                        break;

                    case EC_ServerPacketType::BlendshapeTurnStats:
                        {
                            if (DataObj.IsValid())
                            {
                                const TSharedPtr<FJsonObject>* StatsObj;
                                if (DataObj->TryGetObjectField(TEXT("stats"), StatsObj) && StatsObj->IsValid())
                                {
                                    int32 TotalBlendshapes = 0;
                                    int32 TotalAudioBytes = 0;
                                    double TotalTurnDurationMs = 0.0;
                                    double Fps = 0.0;

                                    (*StatsObj)->TryGetNumberField(TEXT("total_blendshapes"), TotalBlendshapes);
                                    (*StatsObj)->TryGetNumberField(TEXT("total_audio_bytes"), TotalAudioBytes);
                                    (*StatsObj)->TryGetNumberField(TEXT("total_turn_duration_ms"), TotalTurnDurationMs);
                                    (*StatsObj)->TryGetNumberField(TEXT("fps"), Fps);

                                    // Log immediate stats (frames received up to this point)
                                    const bool bFrameCountMatch = (ReceivedBlendshapeFrameCount == TotalBlendshapes);
                                    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[BlendshapeTurnStats] Server: %d frames | Received: %d frames | Match: %s | Audio: %d bytes | Duration: %.2f s | FPS: %.2f"),
                                        TotalBlendshapes, ReceivedBlendshapeFrameCount, bFrameCountMatch ? TEXT("YES") : TEXT("NO"),
                                        TotalAudioBytes, TotalTurnDurationMs / 1000.0, Fps);

                                    // Store values for delayed verification
                                    ExpectedBlendshapeFrameCount = TotalBlendshapes;
                                    FramesAtTurnStatsReceived = ReceivedBlendshapeFrameCount;

                                    // Only start delayed verification if not already running
                                    if (!bDelayedVerificationPending.exchange(true))
                                    {
                                        AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, []()
                                        {
                                            // Loop until no new frames for BlendshapeVerificationDelaySeconds
                                            while (true)
                                            {
                                                FPlatformProcess::Sleep(0.5f);
                                                const double TimeSinceLastFrame = FPlatformTime::Seconds() - LastBlendshapeFrameTime;
                                                if (TimeSinceLastFrame >= BlendshapeVerificationDelaySeconds)
                                                {
                                                    // Log on game thread
                                                    AsyncTask(ENamedThreads::GameThread, []()
                                                    {
                                                        const int32 LateFrames = ReceivedBlendshapeFrameCount - FramesAtTurnStatsReceived;
                                                        const bool bFinalMatch = (ReceivedBlendshapeFrameCount == ExpectedBlendshapeFrameCount);
                                                        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[BlendshapeTurnStats] After %.1fs inactivity - Server: %d | Final Received: %d | Late arrivals: %d | Match: %s"),
                                                            BlendshapeVerificationDelaySeconds, ExpectedBlendshapeFrameCount, ReceivedBlendshapeFrameCount, LateFrames, bFinalMatch ? TEXT("YES") : TEXT("NO"));

                                                        // Reset counter for next turn
                                                        ReceivedBlendshapeFrameCount = 0;
                                                        bDelayedVerificationPending = false;
                                                    });
                                                    break;
                                                }
                                            }
                                        });
                                    }
                                }
                            }
                        }
                        break;

                    case EC_ServerPacketType::UserIdleWarning:
                        {
                            if (DataObj.IsValid())
                            {
                                int32 RemainingSeconds = 0;
                                if (!DataObj->TryGetNumberField(TEXT("remaining_seconds"), RemainingSeconds))
                                {
                                    // Treated as no time left rather than left indeterminate: the
                                    // renewal decision reads this, and an unknown deadline must not
                                    // be allowed to look like a distant one.
                                    RemainingSeconds = 0;
                                    CONVAI_LOG(ConvaiSubsystemLog, Warning,
                                        TEXT("user-idle-warning carried no usable remaining_seconds; assuming none left"));
                                }

                                TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
                                AsyncTask(ENamedThreads::GameThread, [WeakThis, RemainingSeconds]()
                                {
                                    if (UConvaiSubsystem* Subsystem = WeakThis.Get())
                                    {
                                        Subsystem->HandleUserIdleWarning(RemainingSeconds);
                                    }
                                });
                            }
                        }
                        break;

                    case EC_ServerPacketType::SessionEnding:
                        {
                            // The server saying why it is about to close the
                            // session, before it does. Better than the plugin's
                            // own guess from the idle warnings, which a renewal
                            // lost on the way can make wrong. Sent both ways,
                            // so it may arrive twice.
                            FString Reason;
                            if (DataObj.IsValid())
                            {
                                GetStringSafe(DataObj, TEXT("reason"), Reason);
                            }
                            PendingServerIdle.store(Reason == TEXT("idle_timeout") ? 1 : 0);
                            CONVAI_LOG(ConvaiSubsystemLog, Display, TEXT("Server is ending the session: %s"),
                                Reason.IsEmpty() ? TEXT("no reason given") : *Reason);
                        }
                        break;

                    case EC_ServerPacketType::UsageLimitReached:
                        {
                            // The server closes the session after this. The close
                            // itself carries no reason on this transport, so the
                            // cause is recorded now: retrying would only meet the
                            // same limit again.
                            FString QuotaType;
                            FString Message;
                            if (DataObj.IsValid())
                            {
                                GetStringSafe(DataObj, TEXT("quota_type"), QuotaType);
                                GetStringSafe(DataObj, TEXT("message"), Message);
                            }
                            PendingDisconnectCause.store(EC_DisconnectCause::Quota);
                            CONVAI_LOG(ConvaiSubsystemLog, Warning,
                                TEXT("Server reported a usage limit reached (%s): %s. This session will not be reconnected."),
                                *QuotaType, *Message);
                        }
                        break;

                    case EC_ServerPacketType::LLMNoResponse:
                        {
                            // The server signals the LLM chose not to respond (no audio, no text,
                            // no actions). Anything waiting on a bot-speech event would otherwise
                            // hang until its timeout — give it the same unblock signal as a normal
                            // speech end. We deliberately don't go through OnBotStoppedSpeaking
                            // (which also broadcasts an empty final transcript and could trigger
                            // spurious UI updates); we just call OnFinishedTalking.
                            FString Reason;
                            if (DataObj.IsValid())
                            {
                                GetStringSafe(DataObj, TEXT("reason"), Reason);
                            }
                            CONVAI_LOG(ConvaiSubsystemLog, Log,
                                TEXT("LLM declined to respond (reason: %s) — unblocking wait-for-speech listeners."),
                                Reason.IsEmpty() ? TEXT("none") : *Reason);

                            if (IsValid(CurrentCharacterSession))
                            {
                                if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
                                {
                                    Interface->OnFinishedTalking();
                                }
                            }
                        }
                        break;

                    case EC_ServerPacketType::InteractionCreated:
                        {
                            FString InteractionId;
                            if (DataObj.IsValid() &&
                                GetStringSafe(DataObj, TEXT("interaction_id"), InteractionId) &&
                                !InteractionId.IsEmpty())
                            {
                                TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
                                AsyncTask(ENamedThreads::GameThread,
                                    [WeakThis, InteractionId]()
                                {
                                    UConvaiSubsystem* Subsystem = WeakThis.Get();
                                    if (!IsValid(Subsystem) ||
                                        !IsValid(Subsystem->CurrentCharacterSession))
                                    {
                                        return;
                                    }
                                    if (const TScriptInterface<IConvaiConnectionInterface> Interface =
                                            Subsystem->CurrentCharacterSession->GetConnectionInterface();
                                        Interface.GetObject())
                                    {
                                        Interface->OnInteractionIDReceived(InteractionId);
                                    }
                                });
                            }
                        }
                        break;

                    case EC_ServerPacketType::ServerResponse:
                        {
                            FString EventType;
                            FString Message;
                            FString Status;
                            if (DataObj.IsValid())
                            {
                                GetStringSafe(DataObj, TEXT("event_type"), EventType);
                                GetStringSafe(DataObj, TEXT("message"), Message);
                                GetStringSafe(DataObj, TEXT("status"), Status);
                            }

                            if (EventType == TEXT("reset-idle-timer") && Status.Equals(TEXT("success"), ESearchCase::IgnoreCase))
                            {
                                bRenewalAwaitingAck = false;
                            }

                            if (Status.Equals(TEXT("error"), ESearchCase::IgnoreCase))
                            {
                                CONVAI_LOG(ConvaiSubsystemLog, Warning,
                                    TEXT("Server response '%s' failed: %s"),
                                    EventType.IsEmpty() ? TEXT("unknown") : *EventType,
                                    Message.IsEmpty() ? TEXT("no details") : *Message);
                            }
                            else
                            {
                                CONVAI_LOG(ConvaiSubsystemLog, Verbose,
                                    TEXT("Server response '%s' (%s): %s"),
                                    EventType.IsEmpty() ? TEXT("unknown") : *EventType,
                                    Status.IsEmpty() ? TEXT("no status") : *Status,
                                    Message.IsEmpty() ? TEXT("no details") : *Message);
                            }
                        }
                        break;

                    case EC_ServerPacketType::FinalUserTranscription:
                        // Always preceded by the user-transcription packet the
                        // player session consumes; only the watchdog needs it.
                        MicWatchdogServerHeardUser();
                        break;

                    case EC_ServerPacketType::Unknown:
                        {
                            CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("OnDataPacketReceived: Unknown server type '%s'."), *ServerPacketTypeStr);
                        }
                        break;
                        
                    default:
                        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("OnDataPacketReceived: Unhandled type '%s'."), *ServerPacketTypeStr);
                    }
                }
            }
            break;

        case EC_PacketType::BotReady:
            // Belongs to the client that delivered it, decided on the game
            // thread after the epoch check: a flag flipped here, on the
            // transport thread, let a late bot-ready from a client already
            // let go mark its successor ready.
            {
                TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
                AsyncTask(ENamedThreads::GameThread, [WeakThis, Epoch]()
                {
                    if (UConvaiSubsystem* Subsystem = WeakThis.Get())
                    {
                        Subsystem->HandleBotReady(Epoch);
                    }
                });
            }
            break;
        
        case EC_PacketType::BotLLMText:
            // Deliberately discarded. These are sub-word tokens ("us", "ed",
            // "."), and the chat widget that ships with the plugin joins the
            // fragments it is handed with a space -- so forwarding them printed
            // "It's a system us ed to ev aluate con versational AI p
            // erformance." A turn's text reaches the game as bot-transcription
            // sentences instead.
            //
            // The cost is FINDINGS F23's text-prompt half: the server sends no
            // bot-transcription when the player types, so a typed prompt puts
            // no character text on the transcript delegate at all. Delivering
            // it needs the tokens buffered to a sentence boundary first, not
            // forwarded raw.
            break;
    
        case EC_PacketType::FinalUserTranscription:
            // The utterance already reached the player session through
            // user-transcription; only the watchdog wants to know.
            MicWatchdogServerHeardUser();
            break;

        case EC_PacketType::UserLLMText:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: UserLLMText"));
            break;
        case EC_PacketType::BotTTSStarted:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: OnBotTTSStarted"));
            break;
        case EC_PacketType::BotTTSStopped:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: OnBotTTSStopped"));
            break;
        case EC_PacketType::BotTTSText:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: BotTTSText"));
            break;
        case EC_PacketType::BotOutput:
            break;

        case EC_PacketType::LLMFunctionCallLifecycle:
            // These packets describe the server-side tool-call lifecycle. The
            // authoritative action payload still arrives as action-response.
            CONVAI_LOG(ConvaiSubsystemLog, Verbose,
                TEXT("Action function-call lifecycle packet: %s"), *DataPacketTypeStr);
            break;

        case EC_PacketType::ErrorResponse:
            {
                // Not a compatibility notice. That label was hardcoded, so every
                // error-response the server ever sent was filed as one and then
                // dropped (FINDINGS F9). Report what the server actually said, and
                // hand it to the game as non-fatal: a healthy connected session
                // receives three of these.
                FString ErrorStr;
                if (DataObj.IsValid())
                {
                    GetStringSafe(DataObj, TEXT("error"), ErrorStr);
                }
                if (ErrorStr.IsEmpty())
                {
                    ErrorStr = TEXT("no details");
                }
                CONVAI_LOG(ConvaiSubsystemLog, Warning,
                    TEXT("Server error-response: %s"), *ErrorStr);
                OnServerError(ErrorStr, /*bFatal=*/false);
            }
            break;
        
        case EC_PacketType::Error:
            {
                // Severity is carried per packet, not implied by the packet type: this server
                // marks every error the session survives with fatal:false, and routing those to
                // OnFailure reported a broken connection on a connection that never broke. A
                // missing field still means fatal, so an older server keeps escalating -- hence
                // TryGetBoolField over a true default rather than GetBoolSafe, which zeroes it.
                FString ErrorStr;
                bool bFatal = true;
                // Reconnecting asks more of the packet than reporting does: only
                // an explicit fatal:true ends a live session to reconnect it. A
                // missing field still reaches the game as fatal, as it always has.
                bool bSaidFatal = false;
                if (DataObj.IsValid())
                {
                    GetStringSafe(DataObj, TEXT("error"), ErrorStr);
                    DataObj->TryGetBoolField(TEXT("fatal"), bFatal);
                    bool bFatalField = false;
                    bSaidFatal = DataObj->TryGetBoolField(TEXT("fatal"), bFatalField) && bFatalField;
                }
                if (ErrorStr.IsEmpty())
                {
                    ErrorStr = TEXT("no details");
                }

                if (bFatal)
                {
                    OnError(ErrorStr);
                }
                else
                {
                    CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("Server error: %s"), *ErrorStr);
                    OnServerError(ErrorStr, /*bFatal=*/false);
                }

                if (bSaidFatal)
                {
                    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
                    AsyncTask(ENamedThreads::GameThread, [WeakThis, Epoch]()
                    {
                        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
                        {
                            Subsystem->EndLiveSessionForReconnect(Epoch, EConvaiReconnectTrigger::FatalError);
                        }
                    });
                }
            }
            break;
            
        case EC_PacketType::Unknown:
            CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("OnDataPacketReceived: Unknown packet type '%s'."), *DataPacketTypeStr);
            break;
            
        default:
            CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("OnDataPacketReceived: Unhandled type '%s'."), *DataPacketTypeStr);
    }    
}

void UConvaiSubsystem::OnTransportLog(const FString& LogStr)
{
    CONVAI_LOG(ConvaiClientLog, Verbose, TEXT("%s"), *LogStr);
}

void UConvaiSubsystem::OnBotStartedSpeaking(const char* attendee_id, const FString& ResponseId) const
{
    MicWatchdogBotSpeakingUntil = TNumericLimits<double>::Max();
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }

#if ConvaiDebugMode
    // Calculate and log latency from last user stopped speaking
    if (LastUserStoppedSpeakingTimestamp > 0.0)
    {
        const double CurrentTime = FPlatformTime::Seconds();
        const double LatencyMs = (CurrentTime - LastUserStoppedSpeakingTimestamp) * 1000.0;

        // Record latency for statistics
        const_cast<UConvaiSubsystem*>(this)->RecordLatency(LatencyMs);

        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[Latency] Finished Calculating Latency: %.2f ms"), LatencyMs);

        // Print statistics
        PrintLatencyStatistics();
    }
#endif

    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnStartedTalkingForResponse(ResponseId);
    }
}

void UConvaiSubsystem::OnBotStoppedSpeaking(const char* attendee_id, const FString& ResponseId) const
{
    MicWatchdogBotSpeakingUntil = FPlatformTime::Seconds() + MicWatchdogEchoTailSeconds;
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }

    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnFinishedTalkingForResponse(ResponseId);
    }
}

void UConvaiSubsystem::OnBotTurnCompleted(const char* attendee_id, const FString& ResponseId,
    const bool bWasInterrupted, const bool bWasAborted, const FString& ErrorReason) const
{
    MicWatchdogBotSpeakingUntil = FPlatformTime::Seconds() + MicWatchdogEchoTailSeconds;
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }

    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnBotTurnCompleted(ResponseId, bWasInterrupted, bWasAborted, ErrorReason);
    }
}

void UConvaiSubsystem::OnBotTranscript(const char* text, const char* attendee_id) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }

    // One sentence of the answer, forwarded as it arrives. The listener appends
    // it to what came before -- IsTranscriptionReady says so -- which is why
    // this must stay a sentence and must not become the turn assembled so far.
    // Broadcasting the running total appended the whole answer once per packet
    // and rendered "Hello Hello. Hello."
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnTranscriptionReceived(UConvaiUtils::FUTF8ToFString(text), true, false);
    }
}

void UConvaiSubsystem::OnNarrativeSectionReceived(const FString& BT_Code, const FString& BT_Constants,
    const FString& ReceivedNarrativeSectionID) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }
    
    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnNarrativeSectionReceived(BT_Code, BT_Constants, ReceivedNarrativeSectionID);
    }
}

void UConvaiSubsystem::OnEmotionReceived(const FString& ReceivedEmotionResponse, const FAnimationFrame& EmotionBlendshapesFrame,
    const bool MultipleEmotions) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }
    
    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnEmotionReceived(ReceivedEmotionResponse, EmotionBlendshapesFrame, MultipleEmotions);
    }
}

void UConvaiSubsystem::OnFaceDataReceived(const FAnimationSequence& VisemeAnimationSequence) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }
     
    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnFaceDataReceived(VisemeAnimationSequence);
    }
}

void UConvaiSubsystem::DispatchActionSequence(TArray<FConvaiResultAction>& Actions) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }

    const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface();
    if (!Interface.GetObject())
    {
        return;
    }

    // Targets are already fully resolved during parsing (see CoerceParam +
    // PopulateDeprecatedMirrors in the ActionResponse case). We just log + dispatch.
    for (const FConvaiResultAction& Action : Actions)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Action: %s | Params: %d | Ref: %s"),
            *Action.Action,
            Action.Parameters.Num(),
            *Action.RelatedObjectOrCharacter.Name);
    }

    Interface->OnActionSequenceReceived(Actions);
}

void UConvaiSubsystem::OnError(const FString& ErrorMessage) const
{
    CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Error : '%s'."), *ErrorMessage);
    OnServerError(ErrorMessage, /*bFatal=*/true);
}

void UConvaiSubsystem::OnServerError(const FString& ErrorMessage, bool bFatal) const
{
    // Both sessions, because either side's owner may be the one the game bound its
    // delegate to and the subsystem cannot know which. A session that does not
    // override OnServerError gets the interface default, which forwards a fatal
    // error to OnFailure and drops the rest.
    for (const UConvaiConnectionSessionProxy* Session : { CurrentCharacterSession, CurrentPlayerSession })
    {
        if (!IsValid(Session))
        {
            continue;
        }
        if (const TScriptInterface<IConvaiConnectionInterface> Interface =
                Session->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnServerError(ErrorMessage, bFatal);
        }
    }
}

void UConvaiSubsystem::OnUserTranscript(const char* text, const char* attendee_id, bool final, const char* timestamp) const
{
    if (!IsValid(CurrentPlayerSession))
    {
        return;
    }

    // Forwarded as it arrives. user-transcription carries the whole utterance so
    // far on the sessions measured in PIE, which is what the listener wants; a
    // session that instead sends word-sized deltas -- the test harness saw one
    // -- would render a word at a time, and no field in the packet says which
    // shape it is. Assembling here was tried and removed: it was written for a
    // symptom the same commit's other half already explains (see the empty
    // final in OnUserStoppedSpeaking), and it broke the common shape.
    const FString Transcript = UConvaiUtils::FUTF8ToFString(text);

    // Forward to the current player session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnTranscriptionReceived(Transcript, false, final);
    }
}

void UConvaiSubsystem::OnUserStartedSpeaking(const char* attendee_id) const
{
    if (!IsValid(CurrentPlayerSession))
    {
        return;
    }

    // Forward to the current player session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnStartedTalking();
    }

    // Interrupt the character session when user starts speaking
    if (IsValid(CurrentCharacterSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> CharacterInterface = CurrentCharacterSession->GetConnectionInterface(); CharacterInterface.GetObject())
        {
            CharacterInterface->OnInterrupt();
        }
    }
}

void UConvaiSubsystem::OnUserStoppedSpeaking(const char* attendee_id) const
{
    if (!IsValid(CurrentPlayerSession))
    {
        return;
    }

#if ConvaiDebugMode
    // Record timestamp for latency measurement
    const_cast<UConvaiSubsystem*>(this)->LastUserStoppedSpeakingTimestamp = FPlatformTime::Seconds();
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[Latency] Started Calculating Latency"));
#endif

    // Forward to the current player session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
    {
        // No empty final here. user-stopped-speaking arrives before the server's
        // corrected final transcript, so finalising "" told every listener the
        // player had said nothing and then handed them the words a moment later
        // -- which is what left the chat line blank, or holding a single word.
        Interface->OnFinishedTalking();
    }

    // End interrupt on the character session when user stops speaking
    if (IsValid(CurrentCharacterSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> CharacterInterface = CurrentCharacterSession->GetConnectionInterface(); CharacterInterface.GetObject())
        {
            CharacterInterface->OnInterruptEnd();
        }
    }
}

void UConvaiSubsystem::OnBotLLMStarted(const char* attendee_id, const FString& ResponseId) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }

    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnLLMStartedForResponse(ResponseId);
    }
}

void UConvaiSubsystem::OnBotLLMStopped(const char* attendee_id) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }

    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnLLMStopped();
        // An empty final. It closes the utterance without adding text, which
        // is the only thing a listener that appends can be told here: the
        // sentences above already carried every word, so finalising with the
        // assembled turn would hand it the answer a second time.
        //
        // This is FINDINGS F23's other half, deliberately reopened: IsFinal
        // carries nothing for the character, so a game keyed on the final
        // transcription alone gets an empty string and must accumulate the
        // sentences itself.
        Interface->OnTranscriptionReceived(TEXT(""), true, true);
    }
}
