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
 * Outcome of UConvaiConnectionManager::PrepareConnection. Maps directly to
 * the HTTP status of POST /connection/prep.
 */
UENUM(BlueprintType)
enum class EC_PrepResult : uint8
{
	Accepted        UMETA(DisplayName = "Accepted"),
	AlreadyWarm     UMETA(DisplayName = "Already Warm"),
	Rejected        UMETA(DisplayName = "Rejected"),
	InvalidInput    UMETA(DisplayName = "Invalid Input"),
	Disabled        UMETA(DisplayName = "Disabled"),
	InternalError   UMETA(DisplayName = "Internal Error"),
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

	/**
	 * Start a "warm" session for CharacterID with no owner — the proxy is
	 * connected and parked directly in the Orphaned state for TTLSeconds, so
	 * the first real Acquire of the same character will reuse it without a
	 * handshake. See docs/adr/0001-prepared-connections.md.
	 *
	 * TTLSeconds is clamped to [1, 600]. This TTL only applies to the initial
	 * warm window — once a real owner Acquires and later Releases, the
	 * subsequent Orphaned window uses the global ConnectionProxyTTL.
	 */
	EC_PrepResult PrepareConnection(const FString& CharacterID, float TTLSeconds);

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
	void ResetManagedState();

	UPROPERTY()
	UConvaiSubsystem* OwningSubsystem;

	UPROPERTY()
	UConvaiConnectionSessionProxy* ManagedProxy;

	UPROPERTY()
	UConvaiPrepConnectionListener* ActivePrepListener;

	FString ManagedCharacterID;
	EConnectionEntryState ManagedState = EConnectionEntryState::None;
	FTimerHandle ExpiryTimerHandle;
	float CurrentOrphanTTL = -1.0f;
	friend class UConvaiSubsystem;
};

/** No-op IConvaiConnectionInterface placeholder used as the owner of a
 *  prepared (warm) session until the real chatbot Acquires. */
UCLASS()
class UConvaiPrepConnectionListener : public UObject, public IConvaiConnectionInterface
{
	GENERATED_BODY()
	
public:
	virtual bool IsVisionSupported() override { return true; }
	virtual EC_LipSyncMode GetLipSyncMode() override { return EC_LipSyncMode::BS_MHA; }
};