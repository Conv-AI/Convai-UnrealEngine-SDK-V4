/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * CancellationToken.cpp
 *
 * Implementation of cancellation tokens for async operations.
 */

#include "Async/CancellationToken.h"
#include "ConvaiEditor.h"

namespace ConvaiEditor
{
	FCancellationToken::FCancellationToken()
		: bIsCancellationRequested(false)
	{
	}

	FCancellationToken::~FCancellationToken()
	{
		FScopeLock Lock(&DelegateMutex);
		OnCancellationRequestedDelegate.Clear();
	}

	bool FCancellationToken::IsCancellationRequested() const
	{
		return bIsCancellationRequested.Load(EMemoryOrder::Relaxed);
	}

	FDelegateHandle FCancellationToken::RegisterCancellationCallback(TFunction<void()> Callback)
	{
		if (!Callback)
		{
			UE_LOG(LogConvaiEditor, Warning, TEXT("Invalid cancellation callback registration"));
			return FDelegateHandle();
		}

		FScopeLock Lock(&DelegateMutex);

		if (bIsCancellationRequested.Load(EMemoryOrder::Relaxed))
		{
			Callback();
			return FDelegateHandle();
		}

		FDelegateHandle Handle = OnCancellationRequestedDelegate.AddLambda(MoveTemp(Callback));
		return Handle;
	}

	void FCancellationToken::UnregisterCancellationCallback(FDelegateHandle Handle)
	{
		if (!Handle.IsValid())
		{
			return;
		}

		FScopeLock Lock(&DelegateMutex);
		OnCancellationRequestedDelegate.Remove(Handle);
	}

	TSharedPtr<FCancellationToken> FCancellationToken::CreateLinkedToken()
	{
		TSharedPtr<FCancellationToken> LinkedToken = MakeShared<FCancellationToken>();

		{
			FScopeLock Lock(&LinkedTokensMutex);
			LinkedTokens.Add(LinkedToken);
		}

		if (bIsCancellationRequested.Load(EMemoryOrder::Relaxed))
		{
			LinkedToken->RequestCancellation();
		}
		else
		{
			TWeakPtr<FCancellationToken> WeakLinkedToken = LinkedToken;
			RegisterCancellationCallback([WeakLinkedToken]()
										 {
				if (TSharedPtr<FCancellationToken> PinnedToken = WeakLinkedToken.Pin())
				{
					PinnedToken->RequestCancellation();
				} });
		}

		return LinkedToken;
	}

	void FCancellationToken::RequestCancellation()
	{
		bool bExpected = false;
		if (!bIsCancellationRequested.CompareExchange(bExpected, true))
		{
			return;
		}

		{
			FScopeLock Lock(&DelegateMutex);
			OnCancellationRequestedDelegate.Broadcast();
		}

		{
			FScopeLock Lock(&LinkedTokensMutex);
			for (TWeakPtr<FCancellationToken> &WeakToken : LinkedTokens)
			{
				if (TSharedPtr<FCancellationToken> LinkedToken = WeakToken.Pin())
				{
					LinkedToken->RequestCancellation();
				}
			}
			LinkedTokens.Empty();
		}
	}

	FCancellationTokenSource::FCancellationTokenSource()
		: Token(MakeShared<FCancellationToken>())
	{
	}

	FCancellationTokenSource::~FCancellationTokenSource()
	{
		Cancel();
	}

	void FCancellationTokenSource::Cancel()
	{
		if (Token.IsValid())
		{
			Token->RequestCancellation();
		}
	}

	bool FCancellationTokenSource::IsCancellationRequested() const
	{
		return Token.IsValid() ? Token->IsCancellationRequested() : false;
	}

} // namespace ConvaiEditor
