// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiSubsystem.h"

#include "ConvaiActionUtils.h"
#include "ConvaiUtils.h"
#include "ConvaiAndroid.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiReferenceAudioThread.h"
#include "HttpModule.h"
#include "convai/convai_client.h"
#include "../Convai.h"
#include "Interfaces/IHttpResponse.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
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
        BotTranscription,
        ServerMessage,
        BotReady,
        BotLLMText,
        UserLLMText,
        BotTTSStarted,
        BotTTSStopped,
        BotTTSText,
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
        Unknown
    };

    // Counter for received blendshape frames (for verification against turn stats)
    static int32 ReceivedBlendshapeFrameCount = 0;

    // For delayed frame count verification
    static int32 ExpectedBlendshapeFrameCount = 0;
    static int32 FramesAtTurnStatsReceived = 0;
    static constexpr float BlendshapeVerificationDelaySeconds = 2.0f;
    static double LastBlendshapeFrameTime = 0.0;
    static std::atomic<bool> bDelayedVerificationPending{false};

    inline EC_PacketType ToPacketType(const FString& In) noexcept
    {
        if (In == TEXT("user-started-speaking"))   return EC_PacketType::UserStartedSpeaking;
        if (In == TEXT("user-stopped-speaking"))   return EC_PacketType::UserStoppedSpeaking;
        if (In == TEXT("user-transcription"))      return EC_PacketType::UserTranscription;
        if (In == TEXT("bot-llm-started"))         return EC_PacketType::BotLLMStarted;
        if (In == TEXT("bot-llm-stopped"))         return EC_PacketType::BotLLMStopped;
        if (In == TEXT("bot-started-speaking"))    return EC_PacketType::BotStartedSpeaking;
        if (In == TEXT("bot-stopped-speaking"))    return EC_PacketType::BotStoppedSpeaking;
        if (In == TEXT("bot-transcription"))       return EC_PacketType::BotTranscription;
        if (In == TEXT("server-message"))          return EC_PacketType::ServerMessage;
        if (In == TEXT("bot-ready"))               return EC_PacketType::BotReady;
        if (In == TEXT("bot-llm-text"))            return EC_PacketType::BotLLMText;
        if (In == TEXT("user-llm-text"))           return EC_PacketType::UserLLMText;
        if (In == TEXT("bot-tts-started"))         return EC_PacketType::BotTTSStarted;
        if (In == TEXT("bot-tts-stopped"))         return EC_PacketType::BotTTSStopped;
        if (In == TEXT("bot-tts-text"))            return EC_PacketType::BotTTSText;
        if (In == TEXT("error"))                   return EC_PacketType::Error;
        return EC_PacketType::Unknown;
    }

    inline EC_ServerPacketType ToServerPacketType(const FString& In) noexcept
    {
        if (In == TEXT("bot-emotion"))                     return EC_ServerPacketType::BotEmotion;
        if (In == TEXT("action-response"))                 return EC_ServerPacketType::ActionResponse;
        if (In == TEXT("behavior-tree-response"))          return EC_ServerPacketType::BTResponse;
        if (In == TEXT("moderation-response"))             return EC_ServerPacketType::ModerationResponse;
        if (In == TEXT("visemes"))                         return EC_ServerPacketType::Visemes;
        if (In == TEXT("neurosync-blendshapes"))           return EC_ServerPacketType::NeurosyncBlendshapes;
        if (In == TEXT("chunked-neurosync-blendshapes"))   return EC_ServerPacketType::ChunkedNeurosyncBlendshapes;
        if (In == TEXT("blendshape-turn-stats"))           return EC_ServerPacketType::BlendshapeTurnStats;
        return EC_ServerPacketType::Unknown;
    }

    // Helper function to convert server viseme data to FAnimationSequence
    inline void ConvertVisemeDataToAnimationSequence(const TSharedPtr<FJsonObject>& VisemeDataObj, FAnimationSequence& OutAnimationSequence) noexcept
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
    inline void ConvertBlendshapeValuesToFrame(const TArray<TSharedPtr<FJsonValue>>& BlendshapeValues, FAnimationFrame& OutFrame, int32 FrameIndex) noexcept
    {
        OutFrame.FrameIndex = FrameIndex;
        OutFrame.BlendShapes.Empty();

        const int32 NumBlendshapes = BlendshapeValues.Num();
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
    inline void ConvertBlendshapeDataToAnimationSequence(const TSharedPtr<FJsonObject>& BlendshapeDataObj, FAnimationSequence& OutAnimationSequence, bool bIsChunked = false) noexcept
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
    
    inline TSharedPtr<FJsonObject> ParseJsonObject(const FString& JsonStr) noexcept
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return nullptr;
        }
        return Root;
    }

    inline bool GetStringSafe(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FString& Out) noexcept
    {
        Out.Reset();
        return Obj.IsValid() && Obj->TryGetStringField(Field, Out);
    }

    inline bool GetBoolSafe(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, bool& Out) noexcept
    {
        Out = false;
        return Obj.IsValid() && Obj->TryGetBoolField(Field, Out);
    }

    inline bool GetNumberSafe(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double& Out) noexcept
    {
        Out = 0.0;
        return Obj.IsValid() && Obj->TryGetNumberField(Field, Out);
    }

    // Handy extractor for the "data" object.
    inline TSharedPtr<FJsonObject> GetDataObject(const TSharedPtr<FJsonObject>& Root) noexcept
    {
        const TSharedPtr<FJsonObject>* DataObjPtr = nullptr;
        return (Root.IsValid() && Root->TryGetObjectField(TEXT("data"), DataObjPtr)) ? *DataObjPtr : nullptr;
    }

    static UConvaiSubsystem* GetConvaiSubsystemInstance()
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
        
        // Safe UTF8 conversion with explicit null termination
        auto SafeConvertToUTF8 = [](char* Buffer, int32 BufferSize, const TCHAR* Source) -> int32
        {
            FTCHARToUTF8 UTF8Converter(Source);
            const char* UTF8Source = UTF8Converter.Get();
            const int32 UTF8Len = FCStringAnsi::Strlen(UTF8Source);
            
            if (UTF8Len < BufferSize) {
                FCStringAnsi::Strncpy(Buffer, UTF8Source, BufferSize - 1);
                Buffer[UTF8Len] = '\0'; // Explicit null termination
                return UTF8Len;
            }
            return -1;
        };
        
        constexpr int32 BUFFER_SIZE = 512;
        constexpr int32 METADATA_BUFFER_SIZE = 4096;
        
        // Convert all connection parameters to UTF8
        char StreamURLBuffer[BUFFER_SIZE];
        char AuthKeyHeaderBuffer[BUFFER_SIZE];
        char AuthKeyValueBuffer[BUFFER_SIZE];
        char CharIDBuffer[BUFFER_SIZE];
        char ConnectionTypeBuffer[BUFFER_SIZE];
        char LLMProviderBuffer[BUFFER_SIZE];
        char BlendshapeProviderBuffer[BUFFER_SIZE];
        char BlendshapeFormatBuffer[BUFFER_SIZE];
        char EndUserIDBuffer[BUFFER_SIZE];
        char EndUserMetadataBuffer[METADATA_BUFFER_SIZE];

        const int32 StreamURLLen = SafeConvertToUTF8(StreamURLBuffer, BUFFER_SIZE, *StreamURLString);
        const int32 AuthKeyHeaderLen = SafeConvertToUTF8(AuthKeyHeaderBuffer, BUFFER_SIZE, *AuthKeyHeader);
        const int32 AuthKeyValueLen = SafeConvertToUTF8(AuthKeyValueBuffer, BUFFER_SIZE, *AuthKeyValue);
        const int32 CharIDLen = SafeConvertToUTF8(CharIDBuffer, BUFFER_SIZE, *ConnectionParams.CharacterID);
        const int32 ConnectionTypeLen = SafeConvertToUTF8(ConnectionTypeBuffer, BUFFER_SIZE, *ConnectionParams.ConnectionType);
        const int32 LLMProviderLen = SafeConvertToUTF8(LLMProviderBuffer, BUFFER_SIZE, *ConnectionParams.LLMProvider);
        const int32 BlendshapeProviderLen = SafeConvertToUTF8(BlendshapeProviderBuffer, BUFFER_SIZE, *ConnectionParams.BlendshapeProvider);
        const int32 BlendshapeFormatLen = SafeConvertToUTF8(BlendshapeFormatBuffer, BUFFER_SIZE, *ConnectionParams.BlendshapeFormat);
        const int32 EndUserIDLen = SafeConvertToUTF8(EndUserIDBuffer, BUFFER_SIZE, *ConnectionParams.EndUserID);
        const int32 EndUserMetadataLen = SafeConvertToUTF8(EndUserMetadataBuffer, METADATA_BUFFER_SIZE, *ConnectionParams.EndUserMetadata);
        
        // Validate all conversions succeeded
        if (StreamURLLen < 0 || AuthKeyHeaderLen < 0 || AuthKeyValueLen < 0 || CharIDLen < 0 || ConnectionTypeLen < 0 || LLMProviderLen < 0 || BlendshapeProviderLen < 0 || BlendshapeFormatLen < 0 || EndUserIDLen < 0 || EndUserMetadataLen < 0)
        {
            CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to convert one or more strings to UTF8"));
            return 1;
        }
        
        // Log connection parameters
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Connecting to Convai service with parameters:"));
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("StreamURL: %s"), *StreamURLString);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("CharacterID: %s"), *ConnectionParams.CharacterID);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("ConnectionType: %s"), *ConnectionParams.ConnectionType);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("LLMProvider: %s"), *ConnectionParams.LLMProvider);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("BlendshapeProvider: %s"), *ConnectionParams.BlendshapeProvider);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("BlendshapeFormat: %s"), *ConnectionParams.BlendshapeFormat);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("EndUserID: %s"), *ConnectionParams.EndUserID);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("EndUserMetadata: %s"), *ConnectionParams.EndUserMetadata);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("ChunkSize: %d"), ConnectionParams.ChunkSize);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OutputFPS: %d"), ConnectionParams.OutputFPS);
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("FramesBufferDuration: %f"), ConnectionParams.FramesBufferDuration);
        
        // Create connection config struct for the new Connect API
        convai::ConvaiConnectionConfig ConvaiConnectionConfig;
        ConvaiConnectionConfig.url = StreamURLBuffer;
        ConvaiConnectionConfig.auth_value = AuthKeyValueBuffer;
        ConvaiConnectionConfig.auth_header = AuthKeyHeaderBuffer;
        ConvaiConnectionConfig.character_id = CharIDBuffer;
        ConvaiConnectionConfig.connection_type = ConnectionTypeBuffer;
        ConvaiConnectionConfig.llm_provider = LLMProviderBuffer;
        ConvaiConnectionConfig.blendshape_provider = BlendshapeProviderBuffer;
        ConvaiConnectionConfig.blendshape_format = BlendshapeFormatLen > 0 ? BlendshapeFormatBuffer : nullptr;
        ConvaiConnectionConfig.end_user_id = EndUserIDBuffer;
        ConvaiConnectionConfig.end_user_metadata = EndUserMetadataBuffer;
        ConvaiConnectionConfig.chunk_size = ConnectionParams.ChunkSize;
        ConvaiConnectionConfig.output_fps = ConnectionParams.OutputFPS;
        ConvaiConnectionConfig.frames_buffer_duration = ConnectionParams.FramesBufferDuration;
        
        if (!ConnectionParams.Client->Connect(ConvaiConnectionConfig))
        {
            UConvaiSubsystem::OnConnectionFailed();
            CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to connect to Convai service"));
            return 2;
        }
    }
    else
    {
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
#if ConvaiDebugMode
    ResetLatencyStatistics();
#endif
}

void UConvaiSubsystem::Deinitialize()
{
    // Cleanup the Client client
    CleanupConvaiClient();

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
    }
}

void UConvaiSubsystem::UnregisterChatbotComponent(UConvaiChatbotComponent* ChatbotComponent)
{
    if (RegisteredChatbotComponents.Contains(ChatbotComponent))
    {
        ChatbotComponent->StopSession();
        RegisteredChatbotComponents.Remove(ChatbotComponent);
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
        ConnectionThread = MakeUnique<FConvaiConnectionThread>(MoveTemp(ConnectionParams));
        
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

    ConvaiClient->SendTextMessage(TCHAR_TO_UTF8(*Message));

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

    ConvaiClient->SendTriggerMessage(TCHAR_TO_UTF8(*Trigger_Name), TCHAR_TO_UTF8(*Trigger_Message));
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

    FString TemplateKeysJsonStr;
    {
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&TemplateKeysJsonStr);
        FJsonSerializer::Serialize(KeysJson, Writer);
    }

    FTCHARToUTF8 TempKeyJson(*TemplateKeysJsonStr);
    ConvaiClient->UpdateTemplateKeys(TempKeyJson.Get());
}

void UConvaiSubsystem::UpdateDynamicInfo(const UConvaiConnectionSessionProxy* SessionProxy,const FString& Context_Text) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }

    ConvaiClient->UpdateDynamicInfo(TCHAR_TO_UTF8(*Context_Text));
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
    {
        FScopeLock ClientLock(&ConvaiClientMutex);
        if (ConvaiClient)
        {
            ConvaiClient->Disconnect();
            ConvaiClient->SetConvaiClientListner(nullptr);
            ConvaiClient.Reset();  // Smart pointer cleanup
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

void UConvaiSubsystem::OnConnectedToServer()
{
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnConnectedToServer called"));

    if (!ConvaiClient)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("OnConnectedToServer: ConvaiClient is null"));
        return;
    }

    if (!IsValid(CurrentCharacterSession))
    {
        CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("OnConnectedToServer: CurrentCharacterSession is invalid"));
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
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            // Broadcast connection state change to subsystem level
            Subsystem->CurrentConnectionState = EC_ConnectionState::Connected;
            Subsystem->BroadcastServerConnectionStateChanged(EC_ConnectionState::Connected);
            
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

    // Track the attendee in our connected set
    {
        FScopeLock Lock(&AttendeeMutex);
        ConnectedAttendees.Add(Attendee);
    }

    // Forward to character session
    if (IsValid(CurrentCharacterSession))
    {
		CurrentCharacterSession->SetAttendeeId(Attendee);
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnAttendeeConnected(Attendee);
        }
    }

    // Forward to player session (if any)
    if (IsValid(CurrentPlayerSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnAttendeeConnected(Attendee);
        }
    }
}

void UConvaiSubsystem::OnAttendeeDisconnected(const char* attendee_id)
{
    const FString Attendee = UTF8_TO_TCHAR(attendee_id ? attendee_id : "");
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🔌 Attendee disconnected: %s"), *Attendee);

    // Remove the attendee from our connected set
    {
        FScopeLock Lock(&AttendeeMutex);
        ConnectedAttendees.Remove(Attendee);
    }

    // Forward to character session
    if (IsValid(CurrentCharacterSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnAttendeeDisconnected(Attendee);
        }
    }

    // Forward to player session (if any)
    if (IsValid(CurrentPlayerSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnAttendeeDisconnected(Attendee);
        }
    }
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
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: BotLLMStarted "));
            break;

        case EC_PacketType::BotLLMStopped:
            {
                OnBotLLMStopped(TCHAR_TO_UTF8(*Attendee));
            }
            break;

        case EC_PacketType::BotStartedSpeaking:
            {
                OnBotStartedSpeaking(TCHAR_TO_UTF8(*Attendee));
            }
            break;

        case EC_PacketType::BotStoppedSpeaking:
            {
                OnBotStoppedSpeaking(TCHAR_TO_UTF8(*Attendee));
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
                            if (DataObj.IsValid())
                            {
                                TArray<FString> Actions;
                                DataObj->TryGetStringArrayField(TEXT("actions"), Actions);
                                OnActionsReceived(Actions);
                            }
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
                                CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[Blendshapes] Batch received: %d frames | Total: %d"), BatchFrameCount, ReceivedBlendshapeFrameCount);
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
                                CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("[Blendshapes] Batch received: %d frames | Total: %d"), BatchFrameCount, ReceivedBlendshapeFrameCount);
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
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: BotReady"));
            break;
        
        case EC_PacketType::BotLLMText:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: BotLLMText"));
            break;
    
        case EC_PacketType::UserLLMText:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: UserLLMText"));  
            break;
        case EC_PacketType::BotTTSStarted:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: BotTTSStarted"));
            break;
        case EC_PacketType::BotTTSStopped:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: BotTTSStopped"));
            break;
        case EC_PacketType::BotTTSText:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: BotTTSText"));
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

void UConvaiSubsystem::OnBotStartedSpeaking(const char* attendee_id) const
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
        Interface->OnStartedTalking();
    }
}

void UConvaiSubsystem::OnBotStoppedSpeaking(const char* attendee_id) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }
    
    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnFinishedTalking();
		Interface->OnTranscriptionReceived("", true, true);
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

void UConvaiSubsystem::OnActionsReceived(TArray<FString>& Actions) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }
    
    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        TArray<FConvaiResultAction> SequenceOfActions;
        for (const FString& s : Actions)
        {
            FConvaiResultAction ConvaiResultAction;
            if (UConvaiActions::ParseAction(Interface->GetConvaiEnvironment(), s, ConvaiResultAction))
            {
                SequenceOfActions.Add(ConvaiResultAction);
            }

            CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Action: %s"), *ConvaiResultAction.Action);
        }
        
        Interface->OnActionSequenceReceived(SequenceOfActions);
    }
}

void UConvaiSubsystem::OnError(const FString& ErrorMessage) const
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
        Interface->OnTranscriptionReceived(Transcript, true, final);
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

void UConvaiSubsystem::OnBotLLMStopped(const char* attendee_id) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }

    // Forward to the current character session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnTranscriptionReceived("", true, true);
    }
}
