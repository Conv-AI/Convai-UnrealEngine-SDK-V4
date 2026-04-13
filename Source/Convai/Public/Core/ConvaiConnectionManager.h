// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiConnectionManager.generated.h"

class UConvaiConnectionSessionProxy;
class UConvaiSubsystem;

DECLARE_LOG_CATEGORY_EXTERN(ConvaiConnectionManagerLog, Log, All);

UENUM()
enum class EConnectionEntryState : uint8
{
	None,
	Active,
	Orphaned
};

/**
 * Manages the lifecycle of character connections with lease-based ownership.
 * 
 * When a chatbot component releases its connection, the underlying WebRTC session
 * is kept alive for a configurable grace period. If a new chatbot component with
 * the same CharacterID acquires during that window, it reuses the existing connection
 * instead of establishing a new one.
 * 
 * Ownership model is exclusive: only one component can own a connection per CharacterID.
 */
UCLASS()
class CONVAI_API UConvaiConnectionManager : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Acquire a connection for the given CharacterID.
	 * If an orphaned connection for the same CharacterID exists, it is reused.
	 * Otherwise a new connection is created.
	 * @return The session proxy, or nullptr on failure.
	 */
	UConvaiConnectionSessionProxy* AcquireConnection(
		const FString& CharacterID,
		TScriptInterface<IConvaiConnectionInterface> ConnectionInterface);

	/**
	 * Release ownership of a connection. The connection enters an orphaned state
	 * and will be kept alive for ConnectionTimeoutSeconds before being torn down.
	 */
	void ReleaseConnection(const FString& CharacterID, UConvaiConnectionSessionProxy* Proxy);

	void InvalidateOrphanedConnection();

	bool IsCharacterConnectionActive() const { return ManagedState == EConnectionEntryState::Active; }
	EConnectionEntryState GetManagedState() const { return ManagedState; }
	const UConvaiConnectionSessionProxy* GetManagedProxy() const { return ManagedProxy; }

private:
	void Initialize(UConvaiSubsystem* InOwningSubsystem);
	void Shutdown();
	void OnServerDisconnected();

	void ExpireOrphanedConnection();
	void CancelExpiryTimer();
	void StartExpiryTimer();

	UPROPERTY()
	UConvaiSubsystem* OwningSubsystem;

	UPROPERTY()
	UConvaiConnectionSessionProxy* ManagedProxy;

	FString ManagedCharacterID;
	EConnectionEntryState ManagedState = EConnectionEntryState::None;
	FTimerHandle ExpiryTimerHandle;
	friend class UConvaiSubsystem;
};
