// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiSubsystem.h"

#include "ConvaiActionUtils.h"
#include "ConvaiUtils.h"
#include "ConvaiAndroid.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "HttpModule.h"
#include "convai/convai_client.h"
#include "../Convai.h"
#include "Interfaces/IHttpResponse.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"

DEFINE_LOG_CATEGORY(ConvaiSubsystemLog);

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
        return EC_ServerPacketType::Unknown;
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

bool FConvaiConnectionThread::GetConnectionInfo(const FConvaiConnectionThread* Caller, FConvaiConnectionParams ConnectionParams,
    FString& OutResponseBody)
{
    if (!Caller)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("Caller is invalid"));
        return false;
    }

    constexpr float TimeoutSeconds = 30.f;
    constexpr uint32 TimeoutMs = static_cast<uint32>(TimeoutSeconds * 1000.0f);

    const FString RequestBody = FString::Printf(
        TEXT("{\"character_id\":\"%s\",\"transport\":\"livekit\",\"connection_type\":\"%s\",\"llm_provider\":\"%s\"}"),
        *ConnectionParams.CharacterID, *ConnectionParams.ConnectionType, *ConnectionParams.LLMProvider);

    const TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
    FString AuthHeader = AuthHeaderAndKey.Key;
    const FString AuthKey = AuthHeaderAndKey.Value;
    if (AuthHeader == ConvaiConstants::API_Key_Header)
    {
        AuthHeader = ConvaiConstants::X_API_KEY_HEADER;
    }

    const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(UConvaiUtils::GetStreamURL());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetHeader(TEXT("Connection"), TEXT("Keep-alive"));
    Request->SetHeader(AuthHeader, AuthKey);
    Request->SetContentAsString(RequestBody);
    Request->SetTimeout(TimeoutSeconds);
    
    TSharedRef<FRequestState> State = MakeShared<FRequestState>();
    
    Request->OnProcessRequestComplete().BindLambda(
        [State](FHttpRequestPtr /*Req*/, FHttpResponsePtr Response, bool bWasSuccessful)
        {
            // Normalize all outcomes to a single completion call.
            if (!bWasSuccessful || !Response.IsValid())
            {
                const FString Body = Response.IsValid() ? Response->GetContentAsString() : TEXT("No response.");
                const int32   Code = Response.IsValid() ? Response->GetResponseCode() : 0;
                State->Complete(false, Code, Body);
                return;
            }

            const int32 Code = Response->GetResponseCode();
            const bool bOK = (Code >= 200 && Code <= 299);
            State->Complete(bOK, Code, Response->GetContentAsString());
        });

    if (!Request->ProcessRequest())
    {
        State->Complete(false, 0, TEXT("Failed to dispatch HTTP request."));
        OutResponseBody = State->ResponseBody;
        return false;
    }

    // Wait loop: use event (no busy wait). Also watch for shutdown.
    const double Start = FPlatformTime::Seconds();
    while (!State->bCompleted)
    {
        if ((Caller && Caller->bShouldStop) || IsEngineExitRequested())
        {
            Request->CancelRequest();
            // Give the callback a short moment to run; if not, complete ourselves.
            if (!State->DoneEvent->Wait(100)) // 100 ms grace
            {
                State->Complete(false, 0, TEXT("Request cancelled due to shutdown."));
            }
            break;
        }

        if (FPlatformTime::Seconds() - Start * 1000.0 > TimeoutMs)
        {
            Request->CancelRequest();
            if (!State->DoneEvent->Wait(200)) // brief grace after cancel
            {
                State->Complete(false, 0, TEXT("Request timed out."));
            }
            break;
        }

        State->DoneEvent->Wait(50); 
    }

    OutResponseBody = State->ResponseBody;
    return State->bSuccess && (State->StatusCode == 200);
}

uint32 FConvaiConnectionThread::Run()
{
    if (ConnectionParams.Client)
    {
        FString ResponseBody;      
        if (!GetConnectionInfo(this, ConnectionParams, ResponseBody))
        {
            UConvaiSubsystem::OnConnectionFailed();
            CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to fetch room details"));
            return 0;
        }

        CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Connect response : %s"), *ResponseBody);
        
        TSharedPtr<FJsonObject> JsonObj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
        if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
        {
            CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to parse JSON response: %s"), *ResponseBody);
            UConvaiSubsystem::OnConnectionFailed();
            return 0;
        }

        FString RoomUrl, Token;
        if (!JsonObj->TryGetStringField(TEXT("room_url"), RoomUrl) || !JsonObj->TryGetStringField(TEXT("token"), Token))
        {
            CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Response missing 'room_url' or 'token': %s"), *ResponseBody);
            UConvaiSubsystem::OnConnectionFailed();
            return 0;
        }
        
        if (bShouldStop || IsEngineExitRequested())
        {
            return 0;
        }
        
        ConnectionParams.Client->Initialize();

        const FTCHARToUTF8 RoomURLStr(*RoomUrl);
        const FTCHARToUTF8 TokenStr(*Token);

        if (!ConnectionParams.Client->Connect(RoomURLStr.Get(), TokenStr.Get()))
        {
            UConvaiSubsystem::OnConnectionFailed();
            CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to connect to Convai service"));
        }
    }
    else
    {
        CONVAI_LOG(LogTemp, Error, TEXT("Client pointer is null; cannot Connect."));
    }
    
    return 0;
}

// Convai Subsystem Implementation
UConvaiSubsystem::UConvaiSubsystem()
    : ConvaiClient(nullptr)
    , bIsConnected(false)
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
    
    // For player sessions, just store the reference
    if (SessionProxy->IsPlayerSession())
    {
        // Store the player session
        CurrentPlayerSession = SessionProxy;
        return true;
    }
    
    // For character sessions, we need to connect to the Convai service
    
    // If we already have a character session, disconnect it first
    if (IsValid(CurrentCharacterSession))
    {
        // Disconnect the current character session
        DisconnectSession(CurrentCharacterSession);
    }
    
    // Clean up and reinitialize the Client 
    CleanupConvaiClient();
    
    if (!InitializeConvaiClient())
    {
        CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Failed to initialize Client client"));
        return false;
    }

    if (ConvaiClient)
    {
        CurrentCharacterSession = SessionProxy;
        
        // Check if the proxy supports vision and set connection type accordingly
        FString ConnectionType = UConvaiUtils::GetConnectionType();
        if (UConvaiUtils::IsAlwaysAllowVisionEnabled())
        {
            ConnectionType = TEXT("video");
            CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Always allow vision is enabled, using video connection type for character ID: %s"), *CharacterID);
        }
        else
        {
            if (const TScriptInterface<IConvaiConnectionInterface> Interface = SessionProxy->GetConnectionInterface(); Interface.GetObject())
            {
                if (Interface->IsVisionSupported())
                {
                    ConnectionType = TEXT("video");
                    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Vision is supported by proxy, using video connection type for character ID: %s"), *CharacterID);
                }
            }
        }
        
        FConvaiConnectionParams ConnectionParams(ConvaiClient, CharacterID, 
            UConvaiUtils::GetLLMProvider(), ConnectionType);
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
        return;
    }
    
    // If this is a player session, and it's the current one, clear it
    if (SessionProxy->IsPlayerSession() && SessionProxy == CurrentPlayerSession)
    {
        CurrentPlayerSession = nullptr;
    }
    // If this is a character session, and it's the current one, disconnect the client
    else if (!SessionProxy->IsPlayerSession() && SessionProxy == CurrentCharacterSession)
    {
        if (ConvaiClient)
        {
            ConvaiClient->Disconnect();
            bIsConnected = false;
        }
        
        // Clear the current character session
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
    
    // Ensure delegate broadcast happens on game thread since this callback may come from WebRTC thread
    TWeakObjectPtr<UConvaiSubsystem> WeakSubsystem(Subsystem);
    AsyncTask(ENamedThreads::GameThread, [WeakSubsystem]()
    {
        if (UConvaiSubsystem* ValidSubsystem = WeakSubsystem.Get())
        {
            // Broadcast that connection failed (treat as disconnected)
            ValidSubsystem->OnServerConnectionStateChangedEvent.Broadcast(EC_ConnectionState::Disconnected);
        }
    });
    
    Subsystem->CleanupConvaiClient();
}

bool UConvaiSubsystem::InitializeConvaiClient()
{
    const FString ConvaiLogPath = FConvaiLogger::Get().GetLogFilePath();

    // Extract parts
    FString Directory   = FPaths::GetPath(ConvaiLogPath);
    const FString FileBase    = FPaths::GetBaseFilename(ConvaiLogPath);
    const FString Extension   = FPaths::GetExtension(ConvaiLogPath, /*bIncludeDot=*/true);

    // Append suffix
    FString NewFileName = FileBase + TEXT("_Client") + Extension;

    // Rebuild full path
    FTCHARToUTF8 ConvaiClientLogPath(*FPaths::Combine(Directory, NewFileName));
    ConvaiClient = new convai::ConvaiClient(ConvaiClientLogPath.Get(), true);
    
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
        //ConnectionThread.Reset();
    }
    
    if (ConvaiClient)
    {
        ConvaiClient->Disconnect();
        ConvaiClient->SetConvaiClientListner(nullptr);
        delete ConvaiClient;
        ConvaiClient = nullptr;
    }
    
    // Clear the current character session
    CurrentCharacterSession = nullptr;
}

void UConvaiSubsystem::SetupClientCallbacks()
{
    if (!ConvaiClient)
    {
        return;
    }
    ConvaiClient->SetConvaiClientListner(this);
}

void UConvaiSubsystem::OnConnectedToRoom()
{
    CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("OnConnectedToRoom called"));

    if (!ConvaiClient)
    {
        CONVAI_LOG(ConvaiSubsystemLog, Warning, TEXT("OnConnectedToRoom: ConvaiClient is null"));
        return;
    }

    if (!IsValid(CurrentCharacterSession))
    {
        CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("OnConnectedToRoom: CurrentCharacterSession is invalid"));
        CleanupConvaiClient();
        return;
    }
    
    bIsConnected = true;
    ConvaiClient->StartAudioPublishing();
    
    // Ensure delegate broadcast happens on game thread since this callback comes from WebRTC thread
    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis]()
    {
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
            // Broadcast connection state change to subsystem level
            Subsystem->OnServerConnectionStateChangedEvent.Broadcast(EC_ConnectionState::Connected);
            
            if (const TScriptInterface<IConvaiConnectionInterface> Interface = Subsystem->CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
            {
                Interface->OnConnectedToServer();
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

void UConvaiSubsystem::OnDisconnectedFromRoom()
{
    CONVAI_LOG(ConvaiSubsystemLog, Error, TEXT("Disconnected from room"));
    bIsConnected = false;
    
    // Ensure delegate broadcast happens on game thread since this callback comes from WebRTC thread
    TWeakObjectPtr<UConvaiSubsystem> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis]()
    {
        if (UConvaiSubsystem* Subsystem = WeakThis.Get())
        {
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
        }
    });
    
    CleanupConvaiClient();
}

void UConvaiSubsystem::OnAudioData(const char* participant_id, const int16_t* audio_data, size_t num_frames, 
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

void UConvaiSubsystem::OnParticipantConnected(const char* participant_id)
{
    const FString Participant  = UTF8_TO_TCHAR(participant_id);
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🔌 Participant connected: %s"), *Participant);

    // Forward to character session
    if (IsValid(CurrentCharacterSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnParticipantConnected(Participant);
        }
    }

    // Forward to player session (if any)
    if (IsValid(CurrentPlayerSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnParticipantConnected(Participant);
        }
    }
}

void UConvaiSubsystem::OnParticipantDisconnected(const char* participant_id)
{
    const FString Participant = UTF8_TO_TCHAR(participant_id ? participant_id : "");
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🔌 Participant disconnected: %s"), *Participant);

    // Forward to character session
    if (IsValid(CurrentCharacterSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentCharacterSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnParticipantDisconnected(Participant);
        }
    }

    // Forward to player session (if any)
    if (IsValid(CurrentPlayerSession))
    {
        if (const TScriptInterface<IConvaiConnectionInterface> Interface = CurrentPlayerSession->GetConnectionInterface(); Interface.GetObject())
        {
            Interface->OnParticipantDisconnected(Participant);
        }
    }
}

void UConvaiSubsystem::OnActiveSpeakerChanged(const char* Speaker)
{
    const FString SpeakerStr  = UTF8_TO_TCHAR(Speaker);
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("🎤 Active speaker changed: %s"), *SpeakerStr);
}

void UConvaiSubsystem::OnDataPacketReceived(const char* JsonData, const char* participant_id)
{
    const FString JsonStr      = UTF8_TO_TCHAR(JsonData);
    const FString Participant  = UTF8_TO_TCHAR(participant_id);
    
    CONVAI_LOG(ConvaiSubsystemLog, Log, TEXT("Participant ID: %s, Data: %s"), *Participant, *JsonStr);
    
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
                OnUserStartedSpeaking(TCHAR_TO_ANSI(*Participant));
            }
            break;

        case EC_PacketType::UserStoppedSpeaking:
            {
                OnUserStoppedSpeaking(TCHAR_TO_ANSI(*Participant));
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
                    TCHAR_TO_ANSI(*Participant),
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
                OnBotLLMStopped(TCHAR_TO_ANSI(*Participant));
            }
            break;

        case EC_PacketType::BotStartedSpeaking:
            {
                OnBotStartedSpeaking(TCHAR_TO_ANSI(*Participant));
            }
            break;

        case EC_PacketType::BotStoppedSpeaking:
            {
                OnBotStoppedSpeaking(TCHAR_TO_ANSI(*Participant));
            }
            break;

        case EC_PacketType::BotTranscription:
        {
            if (DataObj.IsValid())
            {
                FString Text;
                if (GetStringSafe(DataObj, TEXT("text"), Text) && !Text.IsEmpty())
                {
                    OnBotTranscript(TCHAR_TO_ANSI(*Text), TCHAR_TO_ANSI(*Participant));
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

void UConvaiSubsystem::OnBotStartedSpeaking(const char* participant_id) const
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

void UConvaiSubsystem::OnBotStoppedSpeaking(const char* participant_id) const
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

void UConvaiSubsystem::OnBotTranscript(const char* text, const char* participant_id) const
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

void UConvaiSubsystem::OnUserTranscript(const char* text, const char* participant_id, bool final, const char* timestamp) const
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

void UConvaiSubsystem::OnUserStartedSpeaking(const char* participant_id) const
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

void UConvaiSubsystem::OnUserStoppedSpeaking(const char* participant_id) const
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

void UConvaiSubsystem::OnBotLLMStopped(const char* participant_id) const
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
