// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "Misc/EngineVersionComparison.h"
#include "Templates/SharedPointer.h"

class USoundSubmix;

/**
 * Spellings that changed between the engine versions this plugin ships against.
 *
 * One source tree builds for 5.3 through 5.8, and 5.4 reshaped the submix
 * listener API in three ways at once: ownership moved to a shared reference,
 * the submix stopped defaulting to null, and the interface gained a name. The
 * differences are mechanical, so they live here rather than as an #if around
 * every call in the audio path.
 */

// TArray's EAllowShrinking overloads arrived in 5.4; before that the same
// argument is a bool.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
	#define CONVAI_ALLOW_SHRINKING_NO false
#else
	#define CONVAI_ALLOW_SHRINKING_NO EAllowShrinking::No
#endif

// ISubmixBufferListener::GetListenerName is 5.4+. On 5.3 the override has no
// base method to override, so the whole function has to go.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
	#define CONVAI_SUBMIX_LISTENER_NAME(NameLiteral)
#else
	#define CONVAI_SUBMIX_LISTENER_NAME(NameLiteral)                \
		virtual const FString& GetListenerName() const override     \
		{                                                           \
			static const FString ListenerName(TEXT(NameLiteral));   \
			return ListenerName;                                    \
		}
#endif

/**
 * Registers a submix buffer listener. A null Submix means the main output
 * submix - which is what 5.3 spells as a null argument and what 5.4+ spells as
 * GetMainSubmixObject(). Templated on the device so this header does not drag
 * AudioMixer into every translation unit that only wants the macros above.
 */
template <typename MixerDeviceType, typename ListenerType>
void ConvaiRegisterSubmixListener(MixerDeviceType* Device,
                                  const TSharedRef<ListenerType, ESPMode::ThreadSafe>& Listener,
                                  USoundSubmix* Submix)
{
#if UE_VERSION_OLDER_THAN(5, 4, 0)
	Device->RegisterSubmixBufferListener(&Listener.Get(), Submix);
#else
	Device->RegisterSubmixBufferListener(Listener, Submix ? *Submix : Device->GetMainSubmixObject());
#endif
}

template <typename MixerDeviceType, typename ListenerType>
void ConvaiUnregisterSubmixListener(MixerDeviceType* Device,
                                    const TSharedRef<ListenerType, ESPMode::ThreadSafe>& Listener,
                                    USoundSubmix* Submix)
{
#if UE_VERSION_OLDER_THAN(5, 4, 0)
	Device->UnregisterSubmixBufferListener(&Listener.Get(), Submix);
#else
	Device->UnregisterSubmixBufferListener(Listener, Submix ? *Submix : Device->GetMainSubmixObject());
#endif
}
