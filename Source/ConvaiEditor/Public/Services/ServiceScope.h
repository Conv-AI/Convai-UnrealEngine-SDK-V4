/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ServiceScope.h
 *
 * Service scope management for scoped lifetime services.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Misc/Guid.h"
#include "Templates/SharedPointer.h"
#include "HAL/CriticalSection.h"

// Forward declarations
class IConvaiService;
class FViewModelBase;

namespace ConvaiEditor
{
	/**
	 * Represents a service lifetime scope with automatic cleanup.
	 */
	class CONVAIEDITOR_API FServiceScope : public TSharedFromThis<FServiceScope>
	{
	public:
		explicit FServiceScope(const FString &InScopeName);
		~FServiceScope();

		/** Returns scope name */
		FString GetScopeName() const { return ScopeName; }

		/** Returns unique scope ID */
		FGuid GetScopeId() const { return ScopeId; }

		/** Returns true if scope is active */
		bool IsActive() const { return bIsActive; }

		/** Gets scoped service instance */
		TSharedPtr<IConvaiService> GetScopedService(FName ServiceType) const;

		/** Adds scoped service instance */
		void AddScopedService(FName ServiceType, TSharedPtr<IConvaiService> Service);

		/** Removes scoped service */
		bool RemoveScopedService(FName ServiceType);

		/** Returns number of scoped services */
		int32 GetScopedServiceCount() const;

		/** Clears all scoped services */
		void ClearAllServices();

		/** Gets scoped ViewModel instance */
		TSharedPtr<FViewModelBase> GetScopedViewModel(FName ViewModelType) const;

		/** Adds scoped ViewModel instance */
		void AddScopedViewModel(FName ViewModelType, TSharedPtr<FViewModelBase> ViewModel);

		/** Removes scoped ViewModel */
		bool RemoveScopedViewModel(FName ViewModelType);

		/** Returns number of scoped ViewModels */
		int32 GetScopedViewModelCount() const;

		/** Clears all scoped ViewModels */
		void ClearAllViewModels();

	private:
		FString ScopeName;
		FGuid ScopeId;
		bool bIsActive;
		TMap<FName, TSharedPtr<IConvaiService>> ScopedServices;
		TMap<FName, TSharedPtr<FViewModelBase>> ScopedViewModels;
		mutable FCriticalSection ServicesMutex;
		mutable FCriticalSection ViewModelsMutex;
	};

	/**
	 * Manages service scopes and their lifecycles.
	 */
	class CONVAIEDITOR_API FScopeManager
	{
	public:
		FScopeManager();
		~FScopeManager();

		/** Creates a new scope */
		TSharedPtr<FServiceScope> CreateScope(const FString &ScopeName);

		/** Returns current active scope */
		TSharedPtr<FServiceScope> GetCurrentScope() const;

		/** Pushes scope onto stack */
		void PushScope(TSharedPtr<FServiceScope> Scope);

		/** Pops current scope from stack */
		TSharedPtr<FServiceScope> PopScope();

		/** Destroys scope and shuts down its services */
		void DestroyScope(TSharedPtr<FServiceScope> Scope);

		/** Returns number of active scopes */
		int32 GetActiveScopeCount() const;

		/** Returns true if scope is active */
		bool IsScopeActive(const TSharedPtr<FServiceScope> &Scope) const;

		/** Clears all active scopes */
		void ClearAllScopes();

		/** Scope usage statistics */
		struct FScopeStats
		{
			int32 ActiveScopes;
			int32 TotalScopedServices;
			int32 TotalScopedViewModels;
			TArray<FString> ScopeNames;
		};

		FScopeStats GetScopeStats() const;

	private:
		TArray<TSharedPtr<FServiceScope>> ScopeStack;
		TArray<TWeakPtr<FServiceScope>> AllScopes;
		mutable FCriticalSection ScopeMutex;
	};

} // namespace ConvaiEditor
