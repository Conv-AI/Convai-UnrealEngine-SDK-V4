// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Core/ConvaiConnectionManager.h"
#include "ConvaiSubsystem.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiUtils.h"
#include "TimerManager.h"
#include "Async/Async.h"

DEFINE_LOG_CATEGORY(ConvaiConnectionManagerLog);

void UConvaiConnectionManager::Initialize(UConvaiSubsystem* InOwningSubsystem)
{
	OwningSubsystem = InOwningSubsystem;
	ResetManagedState();
}

void UConvaiConnectionManager::Shutdown()
{
	CancelExpiryTimer();

	if (ManagedState != EConnectionEntryState::None && IsValid(ManagedProxy))
	{
		ManagedProxy->Disconnect();
	}

	ResetManagedState();
}

void UConvaiConnectionManager::ResetManagedState()
{
	ManagedProxy = nullptr;
	ManagedCharacterID.Empty();
	ManagedState = EConnectionEntryState::None;
	CurrentOrphanTTL = -1.0f;
	LeaseEpoch = 0;
	ActivePrepListener = nullptr;
}

bool UConvaiConnectionManager::IsOrphanStillLive() const
{
	// The server state alone lags: after the transport drops and before its
	// task runs it still reads Connected, and reusing then handed the new owner
	// a session that was already gone.
	return OwningSubsystem
		&& OwningSubsystem->IsCurrentEpoch(LeaseEpoch)
		&& OwningSubsystem->CurrentCharacterSession == ManagedProxy
		&& OwningSubsystem->GetServerConnectionState() != EC_ConnectionState::Disconnected
		&& !(OwningSubsystem->GetServerConnectionState() == EC_ConnectionState::Connected && !OwningSubsystem->bIsConnected);
}

bool UConvaiConnectionManager::ReclaimForReconnect(UConvaiConnectionSessionProxy* Proxy, const FString& CharacterID)
{
	if (!IsValid(Proxy))
	{
		return false;
	}
	if (ManagedProxy == Proxy && (ManagedState == EConnectionEntryState::Suspended || ManagedState == EConnectionEntryState::Active))
	{
		ManagedState = EConnectionEntryState::Active;
		return true;
	}
	// Released when its chain gave up, and nobody has taken the character
	// since: a player waking it is its owner coming back.
	const UObject* Owner = Proxy->GetConnectionInterface().GetObject();
	if (ManagedState == EConnectionEntryState::None && Owner && !Owner->IsA<UConvaiPrepConnectionListener>())
	{
		ManagedProxy = Proxy;
		ManagedCharacterID = CharacterID;
		ManagedState = EConnectionEntryState::Active;
		return true;
	}
	return false;
}

UConvaiConnectionSessionProxy* UConvaiConnectionManager::AcquireConnection(
	const FString& CharacterID,
	const TScriptInterface<IConvaiConnectionInterface> ConnectionInterface)
{
	if (ManagedState == EConnectionEntryState::Orphaned
		&& IsValid(ManagedProxy)
		&& ManagedCharacterID == CharacterID
		&& IsOrphanStillLive())
	{
		CancelExpiryTimer();
		ManagedProxy->RebindConnectionInterface(ConnectionInterface);
		ManagedState = EConnectionEntryState::Active;
		CurrentOrphanTTL = -1.0f;
		ActivePrepListener = nullptr;

		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: Reusing existing connection that was waiting for a new owner"), *CharacterID);

		TWeakObjectPtr<UConvaiConnectionSessionProxy> WeakProxy(ManagedProxy);
		TWeakObjectPtr<UConvaiSubsystem> WeakSubsystem(OwningSubsystem);
		const uint64 Epoch = LeaseEpoch;
		AsyncTask(ENamedThreads::GameThread, [WeakProxy, WeakSubsystem, Epoch]()
		{
			if (!WeakProxy.IsValid() || !WeakSubsystem.IsValid() || !WeakSubsystem->IsCurrentEpoch(Epoch))
			{
				return;
			}

			const EC_ConnectionState ServerState = WeakSubsystem->GetServerConnectionState();
			const EC_ConnectionState SessionState = WeakSubsystem->GetSessionConnectionState(WeakProxy.Get());
			const FString AttendeeId = WeakProxy->GetAttendeeId();

			if (const TScriptInterface<IConvaiConnectionInterface> Interface = WeakProxy->GetConnectionInterface(); Interface.GetObject())
			{
				if (ServerState == EC_ConnectionState::Connected)
				{
					Interface->OnConnectedToServer();
				}

				if (SessionState == EC_ConnectionState::Connected)
				{
					Interface->OnAttendeeConnected(AttendeeId);
				}

				// The new owner configures the session it inherited: its keys,
				// its dynamic info and its action context are not the old one's.
				if (WeakSubsystem->IsSessionReady(WeakProxy.Get()))
				{
					Interface->OnSessionReady();
				}
				else if (WeakSubsystem->HasHandshakeGivenUp())
				{
					Interface->OnReadyTimedOut();
				}
			}
		});

		return ManagedProxy;
	}

	if (ManagedState == EConnectionEntryState::Suspended && IsValid(ManagedProxy) && ManagedCharacterID != CharacterID)
	{
		// A restart from inside a disconnect event is a game re-running its own
		// wiring, not a player choosing another character: it must not take the
		// lease from a character whose retry is pending, or two chatbots with
		// the same wiring displace each other on every drop.
		if (OwningSubsystem && (OwningSubsystem->IsInsideDisconnectBroadcast() || OwningSubsystem->bStartDeferredFromDisconnect))
		{
			CONVAI_LOG(ConvaiConnectionManagerLog, Log,
				TEXT("Character [%s]: not taking the connection from [%s], whose reconnect is pending, from inside a disconnect event"),
				*CharacterID, *ManagedCharacterID);
			return nullptr;
		}

		// A game starting another character: the pending reconnect is over.
		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: taking the connection from [%s], whose reconnect is abandoned"), *CharacterID, *ManagedCharacterID);
		UConvaiConnectionSessionProxy* Displaced = ManagedProxy;
		if (OwningSubsystem)
		{
			OwningSubsystem->StopReconnect(Displaced);
		}
		Displaced->ClearConnectionInterface();
		ResetManagedState();
	}

	if ((ManagedState == EConnectionEntryState::Active || ManagedState == EConnectionEntryState::Suspended) && IsValid(ManagedProxy))
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Warning,
			TEXT("Character [%s]: Cannot acquire connection — another component already owns the active connection for character [%s]. "
				 "Make sure the previous owner calls ReturnSessionProxy before a new component tries to acquire."),
			*CharacterID, *ManagedCharacterID);
		return nullptr;
	}

	if (ManagedState == EConnectionEntryState::Orphaned && IsValid(ManagedProxy))
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: Closing previous connection for character [%s] to make room for the new one"),
			*CharacterID, *ManagedCharacterID);
		ExpireOrphanedConnection();
	}

	UConvaiConnectionSessionProxy* NewProxy = NewObject<UConvaiConnectionSessionProxy>(OwningSubsystem);
	if (!IsValid(NewProxy))
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Error,
			TEXT("Character [%s]: Failed to create session proxy — internal allocation error"), *CharacterID);
		return nullptr;
	}

	if (!NewProxy->Initialize(ConnectionInterface, false))
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Error,
			TEXT("Character [%s]: Failed to initialize session proxy — the provided ConnectionInterface may be invalid"), *CharacterID);
		return nullptr;
	}

	if (!NewProxy->Connect(CharacterID))
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Error,
			TEXT("Character [%s]: Failed to establish connection — check your API key, network connectivity, and that the CharacterID is valid"), *CharacterID);
		return nullptr;
	}

	ManagedProxy = NewProxy;
	ManagedCharacterID = CharacterID;
	ManagedState = EConnectionEntryState::Active;

	// A connection acquired fresh - rather than reclaimed from the orphan
	// grace - is a player arriving, so the Visit starts here too.
	if (OwningSubsystem)
	{
		OwningSubsystem->BeginVisit();
	}

	// Notify the connection interface that the attendee is now in connecting state
	if (const TScriptInterface<IConvaiConnectionInterface> Interface = NewProxy->GetConnectionInterface(); Interface.GetObject())
	{
		Interface->OnAttendeeConnecting();
	}

	CONVAI_LOG(ConvaiConnectionManagerLog, Log,
		TEXT("Character [%s]: Establishing new connection"), *CharacterID);

	return NewProxy;
}

void UConvaiConnectionManager::ReleaseConnection(
	const FString& CharacterID,
	UConvaiConnectionSessionProxy* Proxy)
{
	if (!IsValid(Proxy))
	{
		return;
	}

	if (Proxy != ManagedProxy)
	{
		// Nothing managed at all is the ordinary case of a session whose chain
		// already ended and released it; another proxy managed is not.
		if (ManagedState == EConnectionEntryState::None)
		{
			CONVAI_LOG(ConvaiConnectionManagerLog, Log,
				TEXT("Character [%s]: Released a connection whose session had already ended"), *CharacterID);
		}
		else
		{
			CONVAI_LOG(ConvaiConnectionManagerLog, Warning,
				TEXT("Character [%s]: Attempted to release a connection that is not managed by this manager — disconnecting it directly. "
					 "This can happen if the connection was created outside of the ConnectionManager."),
				*CharacterID);
		}
		Proxy->Disconnect();
		return;
	}

	// Everything this release changes happens BEFORE the owner is told. The
	// owner's handler is where a game restarts the session — the wiring
	// Convai's own guidance taught — and a handler that runs while the lease is
	// still Active and the interface still bound re-enters this function
	// through AcquireConnection instead of opening a session. Snapshot, settle,
	// then notify.
	const TScriptInterface<IConvaiConnectionInterface> Interface = Proxy->GetConnectionInterface();
	const FString AttendeeId = Proxy->GetAttendeeId();

	Proxy->ClearConnectionInterface();

	const float TTL = UConvaiUtils::GetConnectionProxyTTL();
	if (ManagedState == EConnectionEntryState::Suspended)
	{
		// No transport is up, so there is nothing to keep warm - and a grace
		// timer would outlive the chain it belonged to.
		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: Connection released while its reconnect was pending"), *CharacterID);
		ResetManagedState();
	}
	else if (TTL <= 0.0f)
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: Connection released — TTL is 0, disconnecting immediately"), *CharacterID);

		Proxy->Disconnect();
		ResetManagedState();
	}
	else
	{
		ManagedState = EConnectionEntryState::Orphaned;
		CurrentOrphanTTL = TTL;
		LeaseEpoch = OwningSubsystem ? OwningSubsystem->ConnectEpoch.load(std::memory_order_acquire) : 0;
		StartExpiryTimer();

		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: Connection released — keeping it alive for %.1fs in case another component reconnects to the same character"),
			*CharacterID, TTL);
	}

	if (Interface.GetObject())
	{
		// A restart asked for from inside these handlers opens its session on
		// the next tick rather than on this stack.
		const UConvaiSubsystem::FDisconnectBroadcastScope BroadcastScope(OwningSubsystem);

		if (!AttendeeId.IsEmpty())
		{
			Interface->OnAttendeeDisconnected(AttendeeId);
		}
		Interface->OnDisconnectedFromServer();
	}
}

EC_PrepResult UConvaiConnectionManager::PrepareConnection(
	const FString& CharacterID,
	float TTLSeconds)
{
	if (CharacterID.IsEmpty())
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Warning,
			TEXT("PrepareConnection rejected — CharacterID is empty"));
		return EC_PrepResult::InvalidInput;
	}

	const float ClampedTTL = FMath::Clamp(TTLSeconds, 1.0f, 600.0f);

	if ((ManagedState == EConnectionEntryState::Active || ManagedState == EConnectionEntryState::Suspended) && IsValid(ManagedProxy))
	{
		if (ManagedCharacterID == CharacterID)
		{
			CONVAI_LOG(ConvaiConnectionManagerLog, Log,
				TEXT("Character [%s]: PrepareConnection no-op — character is already Active"), *CharacterID);
			return EC_PrepResult::AlreadyWarm;
		}

		CONVAI_LOG(ConvaiConnectionManagerLog, Warning,
			TEXT("Character [%s]: PrepareConnection rejected — character [%s] is currently Active. "
				 "Will not displace a live owner."),
			*CharacterID, *ManagedCharacterID);
		return EC_PrepResult::Rejected;
	}

	if (ManagedState == EConnectionEntryState::Orphaned && IsValid(ManagedProxy))
	{
		if (ManagedCharacterID == CharacterID)
		{
			CurrentOrphanTTL = ClampedTTL;
			StartExpiryTimer();

			CONVAI_LOG(ConvaiConnectionManagerLog, Log,
				TEXT("Character [%s]: PrepareConnection — already Orphaned, restarted TTL to %.1fs"),
				*CharacterID, ClampedTTL);
			return EC_PrepResult::AlreadyWarm;
		}

		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: PrepareConnection — closing previous Orphaned connection for character [%s] to make room"),
			*CharacterID, *ManagedCharacterID);
		ExpireOrphanedConnection();
	}

	UConvaiConnectionSessionProxy* NewProxy = NewObject<UConvaiConnectionSessionProxy>(OwningSubsystem);
	if (!IsValid(NewProxy))
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Error,
			TEXT("Character [%s]: PrepareConnection failed — internal allocation error"), *CharacterID);
		return EC_PrepResult::InternalError;
	}

	ActivePrepListener = NewObject<UConvaiPrepConnectionListener>(this);
	if (!NewProxy->Initialize(ActivePrepListener, false))
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Error,
			TEXT("Character [%s]: PrepareConnection failed — proxy initialize returned false"), *CharacterID);
		return EC_PrepResult::InternalError;
	}

	if (!NewProxy->Connect(CharacterID))
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Error,
			TEXT("Character [%s]: PrepareConnection failed — Connect() returned false. "
				 "Check API key, network, and that the CharacterID is valid."),
			*CharacterID);
		return EC_PrepResult::InternalError;
	}

	ManagedProxy = NewProxy;
	ManagedCharacterID = CharacterID;
	ManagedState = EConnectionEntryState::Orphaned;
	CurrentOrphanTTL = ClampedTTL;
	LeaseEpoch = OwningSubsystem ? OwningSubsystem->ConnectEpoch.load(std::memory_order_acquire) : 0;
	StartExpiryTimer();

	CONVAI_LOG(ConvaiConnectionManagerLog, Log,
		TEXT("Character [%s]: PrepareConnection — warm session started, parking in Orphaned for %.1fs"),
		*CharacterID, ClampedTTL);

	return EC_PrepResult::Accepted;
}

void UConvaiConnectionManager::InvalidateOrphanedConnection()
{
	if (ManagedState != EConnectionEntryState::Orphaned)
	{
		return;
	}

	CONVAI_LOG(ConvaiConnectionManagerLog, Log,
		TEXT("Character [%s]: Orphaned connection invalidated by request"), *ManagedCharacterID);

	ExpireOrphanedConnection();
}

void UConvaiConnectionManager::OnServerDisconnected(const UConvaiConnectionSessionProxy* DroppedProxy, EConvaiReconnectState After)
{
	// A handler may already have released this session and acquired another;
	// that lease is not this drop's.
	if (!IsValid(ManagedProxy) || ManagedProxy != DroppedProxy)
	{
		return;
	}

	if (ManagedState == EConnectionEntryState::Active || ManagedState == EConnectionEntryState::Suspended)
	{
		// The owner is alive and its session is coming back, now or on its next
		// word: the lease is kept for it, and only for it.
		if (After == EConvaiReconnectState::Backoff || After == EConvaiReconnectState::Dormant)
		{
			CancelExpiryTimer();
			CurrentOrphanTTL = -1.0f;
			ManagedState = EConnectionEntryState::Suspended;
			return;
		}
		if (After == EConvaiReconnectState::Connecting || After == EConvaiReconnectState::Ready)
		{
			return;
		}
	}

	if (ManagedState == EConnectionEntryState::Orphaned)
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: Server disconnected while connection was orphaned — cleaning up"),
			*ManagedCharacterID);

		CancelExpiryTimer();
		ResetManagedState();
	}
	else
	{
		// The owner of a session that really connected was already told the
		// attendee left, one call earlier on the same stack; telling it again
		// gave every owner two Disconnecteds for one departure. A connect that
		// never reached a session has no attendee loop, so its owner is told
		// here or not at all.
		if (ManagedProxy->GetAttendeeId().IsEmpty())
		{
			if (const TScriptInterface<IConvaiConnectionInterface> Interface = ManagedProxy->GetConnectionInterface(); Interface.GetObject())
			{
				const UConvaiSubsystem::FDisconnectBroadcastScope BroadcastScope(OwningSubsystem);
				Interface->OnDisconnectedFromServer();
			}
		}
		// The chain ended: stopped, gave up, or never retried. A lease left
		// Active on a dead session refused every other character for the rest
		// of the level. The owner keeps its proxy; a wake from a transient
		// failure re-adopts it (ReclaimForReconnect).
		ResetManagedState();
	}
}

void UConvaiConnectionManager::ExpireOrphanedConnection()
{
	CancelExpiryTimer();

	if (IsValid(ManagedProxy))
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: Grace period expired — no component reconnected, tearing down the connection"), *ManagedCharacterID);
		ManagedProxy->Disconnect();
	}

	ResetManagedState();
}

void UConvaiConnectionManager::StartExpiryTimer()
{
	CancelExpiryTimer();

	float EffectiveTTL = CurrentOrphanTTL;
	if (EffectiveTTL <= 0.0f)
	{
		EffectiveTTL = UConvaiUtils::GetConnectionProxyTTL();
		if (EffectiveTTL <= 0.0f)
		{
			CONVAI_LOG(ConvaiConnectionManagerLog, Log,
				TEXT("StartExpiryTimer: both CurrentOrphanTTL and global ConnectionProxyTTL are non-positive — expiring now."));
			ExpireOrphanedConnection();
			return;
		}
		CurrentOrphanTTL = EffectiveTTL;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			ExpiryTimerHandle,
			this,
			&UConvaiConnectionManager::ExpireOrphanedConnection,
			EffectiveTTL,
			false
		);
	}
	else
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Warning,
			TEXT("No valid world for expiry timer. Connection will persist until next acquire or shutdown."));
	}
}

void UConvaiConnectionManager::CancelExpiryTimer()
{
	if (ExpiryTimerHandle.IsValid())
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(ExpiryTimerHandle);
		}
		ExpiryTimerHandle.Invalidate();
	}
}
