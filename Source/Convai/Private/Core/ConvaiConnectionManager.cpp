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
	ActivePrepListener = nullptr;
}

UConvaiConnectionSessionProxy* UConvaiConnectionManager::AcquireConnection(
	const FString& CharacterID,
	const TScriptInterface<IConvaiConnectionInterface> ConnectionInterface)
{
	if (ManagedState == EConnectionEntryState::Orphaned
		&& IsValid(ManagedProxy)
		&& ManagedCharacterID == CharacterID
		&& OwningSubsystem
		&& OwningSubsystem->GetServerConnectionState() != EC_ConnectionState::Disconnected)
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
		AsyncTask(ENamedThreads::GameThread, [WeakProxy, WeakSubsystem]()
		{
			if (!WeakProxy.IsValid() || !WeakSubsystem.IsValid())
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
			}
		});

		return ManagedProxy;
	}

	if (ManagedState == EConnectionEntryState::Active && IsValid(ManagedProxy))
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
		CONVAI_LOG(ConvaiConnectionManagerLog, Warning,
			TEXT("Character [%s]: Attempted to release a connection that is not managed by this manager — disconnecting it directly. "
				 "This can happen if the connection was created outside of the ConnectionManager."),
			*CharacterID);
		Proxy->Disconnect();
		return;
	}

	if (const TScriptInterface<IConvaiConnectionInterface> Interface = Proxy->GetConnectionInterface(); Interface.GetObject())
	{
		const FString AttendeeId = Proxy->GetAttendeeId();
		if (!AttendeeId.IsEmpty())
		{
			Interface->OnAttendeeDisconnected(AttendeeId);
		}
		Interface->OnDisconnectedFromServer();
	}

	Proxy->ClearConnectionInterface();

	const float TTL = UConvaiUtils::GetConnectionProxyTTL();
	if (TTL <= 0.0f)
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
		StartExpiryTimer();

		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: Connection released — keeping it alive for %.1fs in case another component reconnects to the same character"),
			*CharacterID, TTL);
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

	if (ManagedState == EConnectionEntryState::Active && IsValid(ManagedProxy))
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

void UConvaiConnectionManager::OnServerDisconnected()
{
	if (ManagedState == EConnectionEntryState::Orphaned)
	{
		CONVAI_LOG(ConvaiConnectionManagerLog, Log,
			TEXT("Character [%s]: Server disconnected while connection was orphaned — cleaning up"),
			*ManagedCharacterID);

		CancelExpiryTimer();
		ResetManagedState();
	}
	else if (ManagedState == EConnectionEntryState::Active && IsValid(ManagedProxy))
	{
		// Get the current session connection state and notify if not already disconnected
		if (OwningSubsystem)
		{

			// Notify the connection interface that the attendee is disconnected
			if (const TScriptInterface<IConvaiConnectionInterface> Interface = ManagedProxy->GetConnectionInterface(); Interface.GetObject())
			{
				Interface->OnAttendeeDisconnected(ManagedProxy->GetAttendeeId());
			}
		}
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
