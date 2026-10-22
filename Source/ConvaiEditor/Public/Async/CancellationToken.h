/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * CancellationToken.h
 *
 * Thread-safe cancellation token for async operations.
 */

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

namespace ConvaiEditor
{
	/**
	 * Thread-safe token for signaling cancellation of async operations.
	 */
	class CONVAIEDITOR_API FCancellationToken : public TSharedFromThis<FCancellationToken>
	{
	public:
		/** Delegate called when cancellation is requested */
		DECLARE_MULTICAST_DELEGATE(FOnCancellationRequested);

		FCancellationToken();
		~FCancellationToken();

		/**
		 * Checks if cancellation has been requested.
		 *
		 * @return true if cancellation was requested
		 */
		bool IsCancellationRequested() const;

		/**
		 * Registers a callback to be invoked when cancellation is requested.
		 *
		 * @param Callback Function to call on cancellation
		 * @return Handle that can be used to unregister the callback
		 */
		FDelegateHandle RegisterCancellationCallback(TFunction<void()> Callback);

		/**
		 * Unregisters a previously registered cancellation callback.
		 *
		 * @param Handle The handle returned from RegisterCancellationCallback
		 */
		void UnregisterCancellationCallback(FDelegateHandle Handle);

		/**
		 * Creates a linked token that will be cancelled when this token is cancelled.
		 *
		 * @return A new token that is linked to this one
		 */
		TSharedPtr<FCancellationToken> CreateLinkedToken();

		/** Returns the cancellation delegate for advanced scenarios */
		FOnCancellationRequested &OnCancellationRequested() { return OnCancellationRequestedDelegate; }

	private:
		friend class FCancellationTokenSource;

		/** Requests cancellation. Only callable by FCancellationTokenSource. */
		void RequestCancellation();

		/** Indicates whether cancellation has been requested */
		TAtomic<bool> bIsCancellationRequested;

		/** Delegate broadcast when cancellation is requested */
		FOnCancellationRequested OnCancellationRequestedDelegate;

		/** Protects delegate operations */
		mutable FCriticalSection DelegateMutex;

		/** Linked child tokens that should be cancelled when this token is cancelled */
		TArray<TWeakPtr<FCancellationToken>> LinkedTokens;

		/** Protects LinkedTokens array */
		mutable FCriticalSection LinkedTokensMutex;
	};

	/**
	 * Provides a cancellation token and controls when cancellation is signaled.
	 */
	class CONVAIEDITOR_API FCancellationTokenSource
	{
	public:
		FCancellationTokenSource();
		~FCancellationTokenSource();

		/**
		 * Gets the cancellation token associated with this source.
		 *
		 * @return The cancellation token
		 */
		TSharedPtr<FCancellationToken> GetToken() const { return Token; }

		/** Signals cancellation to all operations using this token */
		void Cancel();

		/**
		 * Checks if cancellation has been requested.
		 *
		 * @return true if Cancel() has been called
		 */
		bool IsCancellationRequested() const;

	private:
		/** The cancellation token */
		TSharedPtr<FCancellationToken> Token;
	};

} // namespace ConvaiEditor
