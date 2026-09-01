// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Debug/ConvaiDebugSubsystem.h"
#include "Debug/ConvaiDebugOverlay.h"
#include "../Convai.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"

// One global registration, routed to the right game instance by the world —
// per-subsystem registration would collide on the name with two PIE clients.
static FAutoConsoleCommandWithWorld GConvaiDebugOverlayCmd(
	TEXT("Convai.DebugOverlay"),
	TEXT("Toggles the Convai debug overlay (markers, state panel, action queue)."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		if (UConvaiDebugSubsystem* Sub = GI ? GI->GetSubsystem<UConvaiDebugSubsystem>() : nullptr)
		{
			Sub->ToggleOverlay();
		}
	}));

UConvaiDebugSubsystem* UConvaiDebugSubsystem::GetActive(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UConvaiDebugSubsystem* Subsystem = GameInstance
		? GameInstance->GetSubsystem<UConvaiDebugSubsystem>() : nullptr;
	return (Subsystem && Subsystem->bFeedActive) ? Subsystem : nullptr;
}

void UConvaiDebugSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (FSlateApplication::IsInitialized())
	{
		InputProcessor = MakeShared<FConvaiDebugInputProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
	}
}

void UConvaiDebugSubsystem::Deinitialize()
{
	if (Overlay.IsValid())
	{
		ToggleOverlay(); // removes from viewport + deactivates the feed
	}
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	Super::Deinitialize();
}

void UConvaiDebugSubsystem::ToggleOverlay()
{
#if UE_BUILD_SHIPPING || UE_BUILD_TEST
	// The overlay exposes prompt state, tracked values, and action traffic —
	// releases must opt in explicitly.
	const UConvaiSettings* Settings = Convai::Get().GetConvaiSettings();
	if (!Settings || !Settings->bAllowDebugOverlayInShipping)
	{
		return;
	}
#endif

	UGameInstance* GI = GetGameInstance();
	UGameViewportClient* Viewport = GI ? GI->GetGameViewportClient() : nullptr;
	if (!Viewport)
	{
		return;
	}

	if (Overlay.IsValid())
	{
		Viewport->RemoveViewportWidgetContent(Overlay.ToSharedRef());
		Overlay.Reset();
		SetFeedActive(false);
	}
	else
	{
		SetFeedActive(true); // before Construct so the overlay's bindings see an active feed
		Overlay = SNew(SConvaiDebugOverlay, this);
		Viewport->AddViewportWidgetContent(Overlay.ToSharedRef(), /*ZOrder*/ 1000);
	}
}
