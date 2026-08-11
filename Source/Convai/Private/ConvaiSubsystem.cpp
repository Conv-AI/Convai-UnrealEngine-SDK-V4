// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiSubsystem.h"
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

DEFINE_LOG_CATEGORY(ConvaiSubsystemLog);
DEFINE_LOG_CATEGORY(ConvaiClientLog);

namespace
{
    enum class EC_PacketType : uint8
    {
        UserStartedSpeaking,
        UserStoppedSpeaking,
        UserTranscription,
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

    EC_PacketType ToPacketType(const FString& In) noexcept
    {
        if (In == TEXT("user-started-speaking"))   return EC_PacketType::UserStartedSpeaking;
        if (In == TEXT("user-stopped-speaking"))   return EC_PacketType::UserStoppedSpeaking;
        if (In == TEXT("user-transcription"))      return EC_PacketType::UserTranscription;
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

    UConvaiSubsystem* GetConvaiSubsystemInstance()
    {
        UConvaiSubsystem* Subsystem = nullptr;
    
        // Get the first world context
        if (GEngine)
        {
            for (const FWorldContext& Context : GEngine->GetWorldContexts())
            {
                if (const UWorld* World = Context.World(); World && World->IsGameWorld())
                {
                    if (const UGameInstance* GameInstance = World->GetGameInstance())
                    {
                        Subsystem = GameInstance->GetSubsystem<UConvaiSubsystem>();
                        if (Subsystem)
                        {
                            break;
                        }
                    }
                }
            }
        }
    
        return Subsystem;
    }

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
            UConvaiSubsystem::OnConnectionFailed();
            CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to Initialize client"));
            return 1;
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

        if (!ConnectionParams.Client->Connect(ConvaiConnectionConfig))
        {
            UConvaiSubsystem::OnConnectionFailed();
            CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to connect to Convai service"));
            return 2;
        }
    }
    else
    {
        UConvaiSubsystem::OnConnectionFailed();
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

    ConnectionManager = NewObject<UConvaiConnectionManager>(this);
    ConnectionManager->Initialize(this);

#if ConvaiDebugMode
    ResetLatencyStatistics();
#endif
}

void UConvaiSubsystem::Deinitialize()
{
    // The poll clock lives on UConvaiContextSubsystem now; stop it from here too
    // in case this subsystem tears down first (it also stops itself on its own
    // Deinitialize).
    if (UConvaiContextSubsystem* ContextSubsystem = GetContextSubsystem())
    {
        ContextSubsystem->StopObjectPollClock();
    }
    StopClientReadyHandshake();

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

void UConvaiSubsystem::StartClientReadyHandshake()
{
    StopClientReadyHandshake();
    bIsBotReady = false;

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    SendClientReadyMessage();

    FTimerManager& TimerManager = World->GetTimerManager();
    TimerManager.SetTimer(ClientReadyRetryTimerHandle, this, &UConvaiSubsystem::SendClientReadyMessage,
        UConvaiUtils::GetClientReadyRetrySecs(), true);
    TimerManager.SetTimer(ClientReadyTimeoutTimerHandle, this, &UConvaiSubsystem::OnClientReadyTimeout,
        UConvaiUtils::GetClientReadyTimeoutSecs(), false);
}

void UConvaiSubsystem::StopClientReadyHandshake()
{
    if (UWorld* World = GetWorld())
    {
        FTimerManager& TimerManager = World->GetTimerManager();
        TimerManager.ClearTimer(ClientReadyRetryTimerHandle);
        TimerManager.ClearTimer(ClientReadyTimeoutTimerHandle);
    }
}

void UConvaiSubsystem::OnClientReadyTimeout()
{
    if (bIsBotReady)
    {
        return;
    }
    CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("bot-ready not received within %.0fs; stopping client-ready handshake"), UConvaiUtils::GetClientReadyTimeoutSecs());
    StopClientReadyHandshake();
}

void UConvaiSubsystem::SendClientReadyMessage() const
{
    if (!ConvaiClient || !bIsConnected || bIsBotReady)
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

void UConvaiSubsystem::ResetIdleTimer()
{
    if (!ConvaiClient || !bIsConnected)
    {
        return;
    }
    
    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::ResetIdleTimer, TCHAR_TO_UTF8(TEXT("{}")));
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Sent reset idle timer"));
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
    
    if (CharacterID.IsEmpty())
    {
        CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to connect session: Character ID is empty"))
        return false;
    }
    
    // If we already have a character session, just log it
    // No need to notify - CleanupConvaiClient() handles full disconnection
    if (IsValid(CurrentCharacterSession) && CurrentCharacterSession != SessionProxy)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("Replacing existing character session"));
    }
    
    // Clean up and reinitialize the Client (this handles all cleanup and disconnection)
    CleanupConvaiClient();
    
    if (!InitializeConvaiClient())
    {
        CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to initialize Client client"));
        return false;
    }

    if (ConvaiClient)
    {
        FScopeLock SessionLock(&SessionMutex);
        CurrentCharacterSession = SessionProxy;
        
        // Get raw pointer for connection params
        convai::ConvaiClient* ClientPtr = ConvaiClient.Get();
        FConvaiConnectionParams ConnectionParams = FConvaiConnectionParams::Create(ClientPtr, CharacterID, SessionProxy);

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

        ConnectionThread = MakeUnique<FConvaiConnectionThread>(MoveTemp(ConnectionParams));
		if (!ConnectionThread->IsThreadStarted())
		{
			ConnectionThread.Reset();
			UConvaiSubsystem::OnConnectionFailed();
			return false;
		}
        
        // Broadcast that we're starting to connect
        CurrentConnectionState = EC_ConnectionState::Connecting;
        OnServerConnectionStateChangedEvent.Broadcast(EC_ConnectionState::Connecting);
        
        return true;
    }
    
    return false;
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
    
    // Disconnect the client with mutex protection
    {
        FScopeLock ClientLock(&ConvaiClientMutex);
        if (ConvaiClient)
        {
            ConvaiClient->Disconnect();
            bIsConnected = false;
        }
    }
    
    // Clear the current character session
    {
        FScopeLock SessionLock(&SessionMutex);
        CurrentCharacterSession = nullptr;
    }
}

int32 UConvaiSubsystem::SendAudio(const UConvaiConnectionSessionProxy* SessionProxy, const int16_t* AudioData, const size_t NumFrames) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return -1;
    }

    if (SessionProxy->IsPlayerSession() && ConnectionManager && !ConnectionManager->IsCharacterConnectionActive())
    {
        return -1;
    }

    ConvaiClient->SendAudio(AudioData, NumFrames);
    
    return 0;
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

void UConvaiSubsystem::SendTextMessage(const UConvaiConnectionSessionProxy* SessionProxy,const FString& Message) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }

    TSharedRef<FJsonObject> DataJson = MakeShared<FJsonObject>();
    DataJson->SetStringField(TEXT("text"), Message);
    FString DataJsonStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJsonStr);
    FJsonSerializer::Serialize(DataJson, Writer);

    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::UserTextMessage, TCHAR_TO_UTF8(*DataJsonStr));

        // When the user sends a text message, it interrupts the character session
    if (IsValid(CurrentCharacterSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> CharacterInterface = CurrentCharacterSession->GetConnectionInterface(); CharacterInterface.GetObject())
        {
            CharacterInterface->OnInterrupt();
        }
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

    const TSharedRef<FJsonObject> DataJson = MakeShared<FJsonObject>();
    DataJson->SetBoolField(TEXT("muted"), bMuted);
    FString DataJsonStr;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJsonStr);
    FJsonSerializer::Serialize(DataJson, Writer);

    ConvaiClient->SendMessageWithLabel(ConvaiConstants::WebRTC::label::Default, ConvaiConstants::WebRTC::MessageType::STTToggle, TCHAR_TO_UTF8(*DataJsonStr));
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

void UConvaiSubsystem::OnConnectionFailed()
{
    UConvaiSubsystem* Subsystem = GetConvaiSubsystemInstance();
    if (!IsValid(Subsystem))
    {
        return;
    }
    
    // Ensure delegate broadcast and cleanup happen on game thread since this callback may come from WebRTC thread
    TWeakObjectPtr<UConvaiSubsystem> WeakSubsystem(Subsystem);
    AsyncTask(ENamedThreads::GameThread, [WeakSubsystem]()
    {
        if (UConvaiSubsystem* ValidSubsystem = WeakSubsystem.Get())
        {
            // Broadcast that connection failed (treat as disconnected)
            ValidSubsystem->CurrentConnectionState = EC_ConnectionState::Disconnected;
            ValidSubsystem->BroadcastServerConnectionStateChanged(EC_ConnectionState::Disconnected);

            if (ValidSubsystem->ConnectionManager)
            {
                ValidSubsystem->ConnectionManager->OnServerDisconnected();
            }

            // Cleanup on game thread to avoid race conditions
            ValidSubsystem->CleanupConvaiClient();
        }
    });
}

bool UConvaiSubsystem::InitializeConvaiClient()
{
    FScopeLock ClientLock(&ConvaiClientMutex);

    // Log the ConvaiClient library version
    const char* Version = convai::GetConvaiClientVersion();
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("ConvaiClient Version: %s"), UTF8_TO_TCHAR(Version));

    // If an old client exists, move it to a background task for deferred destruction.
    // The ConvaiClient destructor crashes if called while internal WebRTC threads
    // are still shutting down from a recent Disconnect(). By deferring destruction
    // to an async task, we give the internal threads time to finish.
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

    ConvaiClient = MakeUnique<convai::ConvaiClient>();

    if (ConvaiClient)
    {
        // Setup callbacks
        SetupClientCallbacks();
        return true;
    }

    return false;
}

void UConvaiSubsystem::CleanupConvaiClient()
{
    if (ConnectionThread.IsValid())
    {
        ConnectionThread->Stop();
        ConnectionThread.Reset();
    }
    
    // Stop and cleanup reference audio thread
    if (ReferenceAudioThread.IsValid())
    {
        ReferenceAudioThread->StopCapture();
        ReferenceAudioThread.Reset();
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Stopped and cleaned up reference audio capture thread"));
    }
    
    // Cleanup ConvaiClient with mutex protection
    // NOTE: We intentionally do NOT call ConvaiClient.Reset() here.
    // The ConvaiClient destructor crashes if called while internal WebRTC
    // threads are still shutting down from Disconnect(). Instead we just
    // disconnect and clear the listener, letting the object be destroyed
    // safely when the subsystem is torn down or a new client is created.
    {
        FScopeLock ClientLock(&ConvaiClientMutex);
        if (ConvaiClient)
        {
            ConvaiClient->SetConvaiClientListner(nullptr);
            ConvaiClient->Disconnect();
        }
    }
    
    // Clear the current character session
    {
        FScopeLock SessionLock(&SessionMutex);
        CurrentCharacterSession = nullptr;
    }
    
    bIsConnected = false;
    bStartedPublishingVideo = false;
}

void UConvaiSubsystem::SetupClientCallbacks()
{
    if (!ConvaiClient)
    {
        return;
    }
    ConvaiClient->SetConvaiClientListner(this);
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

void UConvaiSubsystem::OnConnectedToServer(const char* session_id, const char* char_session_id)
{
    const FString SessionID      = UTF8_TO_TCHAR(session_id);
    const FString CharSessionID  = UTF8_TO_TCHAR(char_session_id);
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
    
    bIsConnected = true;
    ConvaiClient->StartAudioPublishing();
    
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
    AsyncTask(ENamedThreads::GameThread, [WeakThis]()
    {
        GetAndroidMicPermission();
        
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            // Broadcast connection state change to subsystem level
            Subsystem->CurrentConnectionState = EC_ConnectionState::Connected;
            Subsystem->BroadcastServerConnectionStateChanged(EC_ConnectionState::Connected);

            Subsystem->StartClientReadyHandshake();

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
        }
    });
}

void UConvaiSubsystem::OnDisconnectedFromServer()
{
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
    AsyncTask(ENamedThreads::GameThread, [WeakThis, AttendeesToDisconnect]()
    {
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            Subsystem->StopClientReadyHandshake();

            // Stop reference audio capture when disconnected (must be on game thread)
            if (Subsystem->ReferenceAudioThread.IsValid())
            {
                Subsystem->ReferenceAudioThread->StopCapture();
                CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Stopped reference audio capture on disconnect"));
            }

            // Fire OnAttendeeDisconnected for each attendee that was connected
            for (const FString& AttendeeId : AttendeesToDisconnect)
            {
                CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🔌 Attendee disconnected (server disconnect): %s"), *AttendeeId);

                if (IsValid(Subsystem->CurrentCharacterSession))
                {
                    if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
                    {
                        Interface->OnAttendeeDisconnected(AttendeeId);
                    }
                }

                if (IsValid(Subsystem->CurrentPlayerSession))
                {
                    if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
                    {
                        Interface->OnAttendeeDisconnected(AttendeeId);
                    }
                }
            }

            // Broadcast connection state change to subsystem level
            Subsystem->CurrentConnectionState = EC_ConnectionState::Disconnected;
            Subsystem->BroadcastServerConnectionStateChanged(EC_ConnectionState::Disconnected);
            
            if (IsValid(Subsystem->CurrentCharacterSession))
            {
                if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
                {
                    Interface->OnDisconnectedFromServer();
                }
            }

            if (IsValid(Subsystem->CurrentPlayerSession))
            {
                if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
                {
                    Interface->OnDisconnectedFromServer();
                }
            }
            
            if (Subsystem->ConnectionManager)
            {
                Subsystem->ConnectionManager->OnServerDisconnected();
            }

            // Cleanup on game thread to avoid race conditions with callbacks
            Subsystem->CleanupConvaiClient();
        }
    });
}

void UConvaiSubsystem::OnAudioData(const char* attendee_id, const int16_t* audio_data, size_t num_frames,
                                   uint32_t sample_rate, uint32_t bits_per_sample, uint32_t num_channels)
{
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

void UConvaiSubsystem::OnAttendeeConnected(const char* attendee_id)
{
    const FString Attendee  = UTF8_TO_TCHAR(attendee_id);
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🔌 Attendee connected: %s"), *Attendee);

    {
        FScopeLock Lock(&AttendeeMutex);
        ConnectedAttendees.Add(Attendee);
    }

    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis, Attendee]()
    {
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            if (IsValid(Subsystem->CurrentCharacterSession))
            {
                Subsystem->CurrentCharacterSession->SetAttendeeId(Attendee);
                if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
                {
                    Interface->OnAttendeeConnected(Attendee);
                }
            }

            if (IsValid(Subsystem->CurrentPlayerSession))
            {
                if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
                {
                    Interface->OnAttendeeConnected(Attendee);
                }
            }
        }
    });
}

void UConvaiSubsystem::OnAttendeeDisconnected(const char* attendee_id)
{
    const FString Attendee = UTF8_TO_TCHAR(attendee_id ? attendee_id : "");
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🔌 Attendee disconnected: %s"), *Attendee);

    {
        FScopeLock Lock(&AttendeeMutex);
        ConnectedAttendees.Remove(Attendee);
    }

    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis, Attendee]()
    {
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
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
        }
    });
}

void UConvaiSubsystem::OnActiveSpeakerChanged(const char* Speaker)
{
    const FString SpeakerStr  = UTF8_TO_TCHAR(Speaker);
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🎤 Active speaker changed: %s"), *SpeakerStr);
}

void UConvaiSubsystem::OnDataPacketReceived(const char* JsonData, const char* attendee_id)
{
    const FString JsonStr      = UTF8_TO_TCHAR(JsonData);
    const FString Attendee  = UTF8_TO_TCHAR(attendee_id);
    
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
                                int32 EmotionScale;
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
                                int32 RemainingSeconds;
                                DataObj->TryGetNumberField(TEXT("remaining_seconds"), RemainingSeconds);

                                TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
                                AsyncTask(ENamedThreads::GameThread, [WeakThis, RemainingSeconds]()
                                {
                                    if (const UConvaiSubsystem* Subsystem = WeakThis.Get())
                                    {
                                        Subsystem->OnUserIdleWarning.Broadcast(RemainingSeconds);
                                    }
                                });
                            }
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
            // Completes the client-ready handshake. This callback is on the WebRTC thread,
            // so flip the flag here (stops further sends immediately) and hop to the game
            // thread to clear the timers — FTimerManager is game-thread-only.
            if (!bIsBotReady)
            {
                bIsBotReady = true;
                TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
                AsyncTask(ENamedThreads::GameThread, [WeakThis]()
                {
                    if (UConvaiSubsystem* Subsystem = WeakThis.Get())
                    {
                        Subsystem->StopClientReadyHandshake();
                    }
                });
            }
            break;
        
        case EC_PacketType::BotLLMText:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: BotLLMText"));
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

        case EC_PacketType::LLMFunctionCallLifecycle:
            // These packets describe the server-side tool-call lifecycle. The
            // authoritative action payload still arrives as action-response.
            CONVAI_LOG(ConvaiSubsystemLog, Verbose,
                TEXT("Action function-call lifecycle packet: %s"), *DataPacketTypeStr);
            break;

        case EC_PacketType::ErrorResponse:
            {
                FString ErrorStr;
                if (DataObj.IsValid())
                {
                    GetStringSafe(DataObj, TEXT("error"), ErrorStr);
                }
                CONVAI_LOG(ConvaiSubsystemLog, Warning,
                    TEXT("Server compatibility notice: %s"),
                    ErrorStr.IsEmpty() ? TEXT("no details") : *ErrorStr);
            }
            break;
        
        case EC_PacketType::Error:
            {
                if (DataObj.IsValid())
                {                    
                    FString ErrorStr;
                    GetStringSafe(DataObj, TEXT("error"),ErrorStr);
                    OnError(ErrorStr);
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

void UConvaiSubsystem::OnLog(const char* log_message)
{
    const FString LogStr      = UConvaiUtils::FUTF8ToFString(log_message);
    CONVAI_LOG(ConvaiClientLog, Verbose, TEXT("%s"), *LogStr);
}

void UConvaiSubsystem::OnBotStartedSpeaking(const char* attendee_id, const FString& ResponseId) const
{
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
    
    // Forward to the current character session
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

void UConvaiSubsystem::OnError(const FString& ErrorMessage)
{
    CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Error : '%s'."), *ErrorMessage);
}

void UConvaiSubsystem::OnUserTranscript(const char* text, const char* attendee_id, bool final, const char* timestamp) const
{
    if (!IsValid(CurrentPlayerSession))
    {
        return;
    }

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
        Interface->OnFinishedTalking();
		Interface->OnTranscriptionReceived("", true, true);
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
        Interface->OnTranscriptionReceived("", true, true);
    }
}
