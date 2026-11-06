/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * EventTypes.h
 *
 * Event type definitions for the Event Aggregator system.
 */

#pragma once

#include "CoreMinimal.h"
#include "Events/IEvent.h"

namespace ConvaiEditor
{
    /** Event fired when network connectivity is restored */
    struct FNetworkRestoredEvent : IEvent
    {
        double DisconnectionDuration;
        int32 CircuitBreakersReset;

        FNetworkRestoredEvent()
            : DisconnectionDuration(0.0), CircuitBreakersReset(0)
        {
        }

        FNetworkRestoredEvent(double InDuration, int32 InCBReset)
            : DisconnectionDuration(InDuration), CircuitBreakersReset(InCBReset)
        {
        }

        virtual FString GetEventName() const override { return TEXT("NetworkRestored"); }
    };

    /** Event fired when network connectivity is lost */
    struct FNetworkDisconnectedEvent : IEvent
    {
        FString Reason;

        FNetworkDisconnectedEvent(const FString &InReason = TEXT("Unknown"))
            : Reason(InReason)
        {
        }

        virtual FString GetEventName() const override { return TEXT("NetworkDisconnected"); }
    };

    /** Event fired when a configuration value changes */
    struct FConfigValueChangedEvent : IEvent
    {
        FString Key;
        FString Value;
        FString OldValue;

        FConfigValueChangedEvent() = default;

        FConfigValueChangedEvent(const FString &InKey, const FString &InValue, const FString &InOldValue = TEXT(""))
            : Key(InKey), Value(InValue), OldValue(InOldValue)
        {
        }

        virtual FString GetEventName() const override { return TEXT("ConfigValueChanged"); }
    };

    /** Event fired when API key changes */
    struct FApiKeyChangedEvent : IEvent
    {
        FString MaskedApiKey;
        bool bIsValid;

        FApiKeyChangedEvent()
            : bIsValid(false)
        {
        }

        FApiKeyChangedEvent(const FString &InMaskedKey, bool bInValid)
            : MaskedApiKey(InMaskedKey), bIsValid(bInValid)
        {
        }

        virtual FString GetEventName() const override { return TEXT("ApiKeyChanged"); }
    };

    /** Event fired when auth token changes */
    struct FAuthTokenChangedEvent : IEvent
    {
        bool bHasToken;
        TOptional<double> ExpirationTime;

        FAuthTokenChangedEvent()
            : bHasToken(false)
        {
        }

        FAuthTokenChangedEvent(bool bInHasToken, TOptional<double> InExpiration = TOptional<double>())
            : bHasToken(bInHasToken), ExpirationTime(InExpiration)
        {
        }

        virtual FString GetEventName() const override { return TEXT("AuthTokenChanged"); }
    };

    /** Event fired when authentication state changes */
    struct FAuthenticationStateChangedEvent : IEvent
    {
        bool bIsAuthenticated;
        FString AuthMethod;
        FString UserIdentifier;

        FAuthenticationStateChangedEvent(bool bInAuthenticated, const FString &InMethod = TEXT(""), const FString &InUser = TEXT(""))
            : bIsAuthenticated(bInAuthenticated), AuthMethod(InMethod), UserIdentifier(InUser)
        {
        }

        virtual FString GetEventName() const override { return TEXT("AuthenticationStateChanged"); }
    };

    /** Event fired when a ViewModel's data is invalidated */
    struct FViewModelInvalidatedEvent : IEvent
    {
        FString ViewModelTypeName;
        TWeakPtr<class FViewModel> ViewModelWeak;
        FString Reason;

        FViewModelInvalidatedEvent() = default;

        FViewModelInvalidatedEvent(const FString &InTypeName, TWeakPtr<class FViewModel> InViewModel, const FString &InReason = TEXT(""))
            : ViewModelTypeName(InTypeName), ViewModelWeak(InViewModel), Reason(InReason)
        {
        }

        virtual FString GetEventName() const override { return TEXT("ViewModelInvalidated"); }
    };

    /** Event fired when a ViewModel's loading state changes */
    struct FViewModelLoadingStateChangedEvent : IEvent
    {
        FString ViewModelTypeName;
        TWeakPtr<class FViewModel> ViewModelWeak;
        bool bIsLoading;
        FText Message;

        FViewModelLoadingStateChangedEvent()
            : bIsLoading(false)
        {
        }

        FViewModelLoadingStateChangedEvent(const FString &InTypeName, TWeakPtr<class FViewModel> InViewModel, bool bInLoading, const FText &InMessage)
            : ViewModelTypeName(InTypeName), ViewModelWeak(InViewModel), bIsLoading(bInLoading), Message(InMessage)
        {
        }

        virtual FString GetEventName() const override { return TEXT("ViewModelLoadingStateChanged"); }
    };

    /** Event fired when a service is started */
    struct FServiceStartedEvent : IEvent
    {
        FString ServiceName;
        FString ServiceTypeName;

        FServiceStartedEvent() = default;

        FServiceStartedEvent(const FString &InName, const FString &InType)
            : ServiceName(InName), ServiceTypeName(InType)
        {
        }

        virtual FString GetEventName() const override { return TEXT("ServiceStarted"); }
    };

    /** Event fired when a service encounters an error */
    struct FServiceErrorEvent : IEvent
    {
        FString ServiceName;
        FString ErrorMessage;

        enum class ESeverity
        {
            Info,
            Warning,
            Error,
            Critical
        } Severity;

        FServiceErrorEvent()
            : Severity(ESeverity::Error)
        {
        }

        FServiceErrorEvent(const FString &InName, const FString &InError, ESeverity InSeverity = ESeverity::Error)
            : ServiceName(InName), ErrorMessage(InError), Severity(InSeverity)
        {
        }

        virtual FString GetEventName() const override { return TEXT("ServiceError"); }
    };

    /** Event fired when a plugin update is available */
    struct FUpdateAvailableEvent : IEvent
    {
        FString CurrentVersion;
        FString AvailableVersion;
        FString ReleaseNotesUrl;
        bool bIsCritical;

        FUpdateAvailableEvent()
            : bIsCritical(false)
        {
        }

        FUpdateAvailableEvent(const FString &InCurrent, const FString &InAvailable, const FString &InUrl, bool bInCritical)
            : CurrentVersion(InCurrent), AvailableVersion(InAvailable), ReleaseNotesUrl(InUrl), bIsCritical(bInCritical)
        {
        }

        virtual FString GetEventName() const override { return TEXT("UpdateAvailable"); }
    };

    /** Event fired when API validation completes */
    struct FApiValidationCompletedEvent : IEvent
    {
        FString ValidationType;
        bool bSuccess;
        FString ErrorMessage;

        FApiValidationCompletedEvent()
            : bSuccess(false)
        {
        }

        FApiValidationCompletedEvent(const FString &InType, bool bInSuccess, const FString &InError = TEXT(""))
            : ValidationType(InType), bSuccess(bInSuccess), ErrorMessage(InError)
        {
        }

        virtual FString GetEventName() const override { return TEXT("ApiValidationCompleted"); }
    };

} // namespace ConvaiEditor
