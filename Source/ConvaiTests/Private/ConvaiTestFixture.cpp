// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiTestFixture.h"

#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestEventSink.h"
#include "ConvaiTests.h"
#include "ConvaiUtils.h"
#include "ConvaiVirtualMicComponent.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

bool FConvaiTestFixture::HasCredentials(FString& OutError, bool bRequireCharacter)
{
    if (UConvaiUtils::GetAPI_Key().IsEmpty())
    {
        OutError = TEXT("no Convai API key configured");
        return false;
    }
    if (bRequireCharacter && UConvaiUtils::GetTestCharacterID().IsEmpty())
    {
        OutError = TEXT("no test character; set TestCharacterID in "
                        "[/Script/Convai.ConvaiSettings] or pass -ConvaiTestCharacterID=");
        return false;
    }
    return true;
}

bool FConvaiTestFixture::HasConfiguredActorClass(FString& OutPath, FString& OutError)
{
    OutPath = UConvaiUtils::GetCustomParam(TEXT("ConvaiTestActorClass"));
    if (OutPath.IsEmpty())
    {
        OutError = TEXT("no character Blueprint; pass -ConvaiTestActorClass=/Game/....BP_X_C");
        return false;
    }
    return true;
}

bool FConvaiTestFixture::Spawn(UWorld* World, FConvaiTestEventRecorder& Recorder,
                               const FOptions& Options, FString& OutError)
{
    if (!World)
    {
        OutError = TEXT("no world");
        return false;
    }

    CharacterID = Options.CharacterID.IsEmpty() ? UConvaiUtils::GetTestCharacterID()
                                                : Options.CharacterID;

    AActor* SpawnedOwner = World->SpawnActor<AActor>();
    if (!SpawnedOwner)
    {
        OutError = TEXT("could not spawn owner actor");
        return false;
    }
    Owner = SpawnedOwner;

    USceneComponent* Root = NewObject<USceneComponent>(SpawnedOwner, TEXT("Root"));
    SpawnedOwner->SetRootComponent(Root);
    Root->RegisterComponent();

    // Always, and registered before the player component: the player adopts
    // whichever IConvaiAudioCaptureInterface it finds on the actor during its
    // own registration, and with none it captures the host's real microphone,
    // so whatever is said in the room barges in on the character. A mic
    // registered afterwards is never discovered either. Scenarios that never
    // speak simply never start it.
    UConvaiVirtualMicComponent* SpawnedMic =
        NewObject<UConvaiVirtualMicComponent>(SpawnedOwner, TEXT("VirtualMic"));
    SpawnedMic->RegisterComponent();
    Mic = SpawnedMic;

    UConvaiPlayerComponent* SpawnedPlayer =
        NewObject<UConvaiPlayerComponent>(SpawnedOwner, TEXT("ConvaiPlayer"));
    SpawnedPlayer->RegisterComponent();
    Player = SpawnedPlayer;

    UConvaiChatbotComponent* SpawnedChatbot = nullptr;
    if (Options.ActorClassPath.IsEmpty())
    {
        SpawnedChatbot = NewObject<UConvaiChatbotComponent>(SpawnedOwner, TEXT("ConvaiChatbot"));
        SpawnedChatbot->CharacterID = CharacterID;
        SpawnedChatbot->RegisterComponent();
    }
    else
    {
        UClass* CharacterClass = LoadClass<AActor>(nullptr, *Options.ActorClassPath);
        if (!CharacterClass)
        {
            OutError = FString::Printf(TEXT("cannot load actor class %s"), *Options.ActorClassPath);
            return false;
        }
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* SpawnedCharacter =
            World->SpawnActor<AActor>(CharacterClass, FTransform::Identity, Params);
        if (!SpawnedCharacter)
        {
            OutError = FString::Printf(TEXT("could not spawn %s"), *Options.ActorClassPath);
            return false;
        }
        Character = SpawnedCharacter;

        SpawnedChatbot = SpawnedCharacter->FindComponentByClass<UConvaiChatbotComponent>();
        if (!SpawnedChatbot)
        {
            OutError = FString::Printf(TEXT("%s has no ConvaiChatbotComponent"),
                                       *Options.ActorClassPath);
            return false;
        }
        // The Blueprint's components began play inside SpawnActor with no
        // character set. Its own auto-start is deferred a tick and finds the
        // session the scenario opens first, so this only names the character.
        SpawnedChatbot->LoadCharacter(CharacterID);
    }
    Chatbot = SpawnedChatbot;

    Sink.Reset(NewObject<UConvaiTestEventSink>());
    SpawnedPlayer->OnTranscriptionReceivedDelegate.AddDynamic(
        Sink.Get(), &UConvaiTestEventSink::HandleTranscription);
    if (Options.bListenToBotTranscript)
    {
        SpawnedChatbot->OnTranscriptionReceivedDelegate.AddDynamic(
            Sink.Get(), &UConvaiTestEventSink::HandleTranscription);
    }
    SpawnedChatbot->OnFailureEvent.AddDynamic(Sink.Get(), &UConvaiTestEventSink::HandleFailure);
    SpawnedChatbot->OnCharacterDataLoadEvent_V2.AddDynamic(
        Sink.Get(), &UConvaiTestEventSink::HandleCharacterDataLoaded);
    SpawnedChatbot->OnServerErrorEvent.AddDynamic(Sink.Get(),
                                                  &UConvaiTestEventSink::HandleServerError);
    SpawnedChatbot->OnBotTurnCompletedEvent.AddDynamic(
        Sink.Get(), &UConvaiTestEventSink::HandleBotTurnCompleted);
    SpawnedChatbot->OnInterruptedEvent.AddDynamic(Sink.Get(),
                                                  &UConvaiTestEventSink::HandleInterrupted);
    SpawnedChatbot->OnEmotionStateChangedEvent.AddDynamic(
        Sink.Get(), &UConvaiTestEventSink::HandleEmotionStateChanged);
    SpawnedChatbot->OnFacialDataReadyDelegate.AddDynamic(
        Sink.Get(), &UConvaiTestEventSink::HandleFacialDataReady);

    Recorder.Record(TEXT("components_registered"),
                    FString::Printf(TEXT("character=%s chatbot=%s class=%s"),
                                    *CharacterID.Left(8), *SpawnedChatbot->GetName(),
                                    Options.ActorClassPath.IsEmpty() ? TEXT("<bare>")
                                                                     : *Options.ActorClassPath));
    return true;
}

void FConvaiTestFixture::Destroy()
{
    if (AActor* OwnerActor = Owner.Get())
    {
        OwnerActor->Destroy();
    }
    if (AActor* CharacterActor = Character.Get())
    {
        CharacterActor->Destroy();
    }
    Owner.Reset();
    Character.Reset();
    Mic.Reset();
    Player.Reset();
    Chatbot.Reset();
    Sink.Reset();
}

bool FConvaiTestFixture::IsChatbotConnected() const
{
    const UConvaiChatbotComponent* Bot = Chatbot.Get();
    return Bot && Bot->GetChatbotConnectionState() == EC_ConnectionState::Connected;
}
