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
        Unknown
    };

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
        if (In == TEXT("bot-emotion"))             return EC_ServerPacketType::BotEmotion;        
        if (In == TEXT("action-response"))         return EC_ServerPacketType::ActionResponse;    
        if (In == TEXT("behavior-tree-response"))  return EC_ServerPacketType::BTResponse;        
        if (In == TEXT("moderation-response"))     return EC_ServerPacketType::ModerationResponse;
        if (In == TEXT("visemes"))                 return EC_ServerPacketType::Visemes;
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
        if (AECTypeStr.Equals(TEXT("Internal"), ESearchCase::IgnoreCase))
        {
            Config.aec_type = convai::AECType::Internal;
        }
        else if (AECTypeStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
        {
            Config.aec_type = convai::AECType::None;
        }
        else // Default to External
        {
            Config.aec_type = convai::AECType::External;
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
        constexpr int32 BUFFER_SIZE = 512;
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
        
        // Convert all connection parameters to UTF8
        char StreamURLBuffer[BUFFER_SIZE];
        char AuthKeyHeaderBuffer[BUFFER_SIZE];
        char AuthKeyValueBuffer[BUFFER_SIZE];
        char CharIDBuffer[BUFFER_SIZE];
        char ConnectionTypeBuffer[BUFFER_SIZE];
        char LLMProviderBuffer[BUFFER_SIZE];
        char BlendshapeProviderBuffer[BUFFER_SIZE];
        char SpeakerIDBuffer[BUFFER_SIZE];

        const int32 StreamURLLen = SafeConvertToUTF8(StreamURLBuffer, BUFFER_SIZE, *StreamURLString);
        const int32 AuthKeyHeaderLen = SafeConvertToUTF8(AuthKeyHeaderBuffer, BUFFER_SIZE, *AuthKeyHeader);
        const int32 AuthKeyValueLen = SafeConvertToUTF8(AuthKeyValueBuffer, BUFFER_SIZE, *AuthKeyValue);
        const int32 CharIDLen = SafeConvertToUTF8(CharIDBuffer, BUFFER_SIZE, *ConnectionParams.CharacterID);
        const int32 ConnectionTypeLen = SafeConvertToUTF8(ConnectionTypeBuffer, BUFFER_SIZE, *ConnectionParams.ConnectionType);
        const int32 LLMProviderLen = SafeConvertToUTF8(LLMProviderBuffer, BUFFER_SIZE, *ConnectionParams.LLMProvider);
        const int32 BlendshapeProviderLen = SafeConvertToUTF8(BlendshapeProviderBuffer, BUFFER_SIZE, *ConnectionParams.BlendshapeProvider);
        const int32 SpeakerIDLen = SafeConvertToUTF8(SpeakerIDBuffer, BUFFER_SIZE, *ConnectionParams.SpeakerID);
        
        // Validate all conversions succeeded
        if (StreamURLLen < 0 || AuthKeyHeaderLen < 0 || AuthKeyValueLen < 0 || CharIDLen < 0 || ConnectionTypeLen < 0 || LLMProviderLen < 0 || BlendshapeProviderLen < 0 || SpeakerIDLen < 0)
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
        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("SpeakerID: %s"), *ConnectionParams.SpeakerID);
        
        // Create connection config struct for the new Connect API
        convai::ConvaiConnectionConfig config;
        config.url = StreamURLBuffer;
        config.auth_value = AuthKeyValueBuffer;
        config.auth_header = AuthKeyHeaderBuffer;
        config.character_id = CharIDBuffer;
        config.connection_type = ConnectionTypeBuffer;
        config.llm_provider = LLMProviderBuffer;
        config.blendshape_provider = BlendshapeProviderBuffer;
        config.speaker_id = SpeakerIDBuffer;
        
        if (!ConnectionParams.Client->Connect(config))
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
    , CurrentCharacterSession(nullptr)
    , CurrentPlayerSession(nullptr)
{
}

void UConvaiSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
}

void UConvaiSubsystem::Deinitialize()
{
    // Cleanup the Client client
    CleanupConvaiClient();
    
    Super::Deinitialize();
}

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
    }
    else
    {        
        ConvaiClient->SendImage(Width, Height, Data.GetData());
    }
}

void UConvaiSubsystem::SendTextMessage(const UConvaiConnectionSessionProxy* SessionProxy,const FString& Message) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }
    
    ConvaiClient->SendTextMessage(TCHAR_TO_ANSI(*Message));
}

void UConvaiSubsystem::SendTriggerMessage(const UConvaiConnectionSessionProxy* SessionProxy,const FString& Trigger_Name, const FString& Trigger_Message) const
{
    if (!IsValid(SessionProxy) || !ConvaiClient || !bIsConnected)
    {
        return;
    }
    
    ConvaiClient->SendTriggerMessage(TCHAR_TO_ANSI(*Trigger_Name), TCHAR_TO_ANSI(*Trigger_Message));
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
    
    ConvaiClient->UpdateDynamicInfo(TCHAR_TO_ANSI(*Context_Text));
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
            ValidSubsystem->OnServerConnectionStateChangedEvent.Broadcast(EC_ConnectionState::Disconnected);
            
            // Cleanup on game thread to avoid race conditions
            ValidSubsystem->CleanupConvaiClient();
        }
    });
}

bool UConvaiSubsystem::InitializeConvaiClient()
{
    FScopeLock ClientLock(&ConvaiClientMutex);
    
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
            Subsystem->OnServerConnectionStateChangedEvent.Broadcast(EC_ConnectionState::Connected);
            
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
    
    // Ensure delegate broadcast and cleanup happen on game thread since this callback comes from WebRTC thread
    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis]()
    {
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            // Stop reference audio capture when disconnected (must be on game thread)
            if (Subsystem->ReferenceAudioThread.IsValid())
            {
                Subsystem->ReferenceAudioThread->StopCapture();
                CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Stopped reference audio capture on disconnect"));
            }
            
            // Broadcast connection state change to subsystem level
            Subsystem->OnServerConnectionStateChangedEvent.Broadcast(EC_ConnectionState::Disconnected);
            
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

    // Forward to character session
    if (IsValid(CurrentCharacterSession))
    {
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
    
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Attendee ID: %s, Data: %s"), *Attendee, *JsonStr);
    
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
                OnUserStartedSpeaking(TCHAR_TO_ANSI(*Attendee));
            }
            break;

        case EC_PacketType::UserStoppedSpeaking:
            {
                OnUserStoppedSpeaking(TCHAR_TO_ANSI(*Attendee));
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
                    TCHAR_TO_ANSI(*Text),
                    TCHAR_TO_ANSI(*Attendee),
                    bFinal,
                    TCHAR_TO_ANSI(*Timestamp)
                );
            }
        }
            break;

        case EC_PacketType::BotLLMStarted:
            //CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("OnDataPacketReceived: BotLLMStarted "));
            break;

        case EC_PacketType::BotLLMStopped:
            {
                OnBotLLMStopped(TCHAR_TO_ANSI(*Attendee));
            }
            break;

        case EC_PacketType::BotStartedSpeaking:
            {
                OnBotStartedSpeaking(TCHAR_TO_ANSI(*Attendee));
            }
            break;

        case EC_PacketType::BotStoppedSpeaking:
            {
                OnBotStoppedSpeaking(TCHAR_TO_ANSI(*Attendee));
            }
            break;

        case EC_PacketType::BotTranscription:
        {
            if (DataObj.IsValid())
            {
                FString Text;
                if (GetStringSafe(DataObj, TEXT("text"), Text) && !Text.IsEmpty())
                {
                    OnBotTranscript(TCHAR_TO_ANSI(*Text), TCHAR_TO_ANSI(*Attendee));
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
    const FString LogStr      = UTF8_TO_TCHAR(log_message);
    CONVAI_LOG(ConvaiClientLog, Verbose, TEXT("%s"), *LogStr);
}

void UConvaiSubsystem::OnBotStartedSpeaking(const char* attendee_id) const
{
    if (!IsValid(CurrentCharacterSession))
    {
        return;
    }
    
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
        Interface->OnTranscriptionReceived(UTF8_TO_TCHAR(text), true, false);
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

    const FString Transcript = UTF8_TO_TCHAR(text);
    
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
}

void UConvaiSubsystem::OnUserStoppedSpeaking(const char* attendee_id) const
{
    if (!IsValid(CurrentPlayerSession))
    {
        return;
    }
    
    // Forward to the current player session
    if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
    {
        Interface->OnFinishedTalking();
		Interface->OnTranscriptionReceived("", true, true);
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
