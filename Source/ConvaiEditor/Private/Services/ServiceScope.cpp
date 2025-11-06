/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ServiceScope.cpp
 *
 * Implementation of service scope management system
 */

#include "Services/ServiceScope.h"
#include "ConvaiEditor.h"
#include "MVVM/ViewModel.h"

namespace ConvaiEditor
{

	FServiceScope::FServiceScope(const FString &InScopeName)
		: ScopeName(InScopeName), ScopeId(FGuid::NewGuid()), bIsActive(true)
	{
	}

	FServiceScope::~FServiceScope()
	{
		ClearAllServices();
		ClearAllViewModels();

		bIsActive = false;
	}

	TSharedPtr<IConvaiService> FServiceScope::GetScopedService(FName ServiceType) const
	{
		FScopeLock Lock(&ServicesMutex);

		const TSharedPtr<IConvaiService> *Found = ScopedServices.Find(ServiceType);
		return Found ? *Found : nullptr;
	}

	void FServiceScope::AddScopedService(FName ServiceType, TSharedPtr<IConvaiService> Service)
	{
		FScopeLock Lock(&ServicesMutex);

		if (ScopedServices.Contains(ServiceType))
		{
			UE_LOG(LogConvaiEditor, Warning,
				   TEXT("ServiceScope: replacing existing scoped service '%s' in scope '%s'"),
				   *ServiceType.ToString(), *ScopeName);
		}

		ScopedServices.Add(ServiceType, Service);
	}

	bool FServiceScope::RemoveScopedService(FName ServiceType)
	{
		FScopeLock Lock(&ServicesMutex);

		if (TSharedPtr<IConvaiService> *Found = ScopedServices.Find(ServiceType))
		{
			if (Found->IsValid())
			{
				(*Found)->Shutdown();
			}

			ScopedServices.Remove(ServiceType);

			return true;
		}

		return false;
	}

	int32 FServiceScope::GetScopedServiceCount() const
	{
		FScopeLock Lock(&ServicesMutex);
		return ScopedServices.Num();
	}

	void FServiceScope::ClearAllServices()
	{
		FScopeLock Lock(&ServicesMutex);

		if (ScopedServices.Num() == 0)
		{
			return;
		}

		for (auto &Pair : ScopedServices)
		{
			if (Pair.Value.IsValid())
			{
				try
				{
					Pair.Value->Shutdown();
				}
				catch (const std::exception &e)
				{
					UE_LOG(LogConvaiEditor, Error,
						   TEXT("ServiceScope: exception during service shutdown '%s': %s"),
						   *Pair.Key.ToString(), UTF8_TO_TCHAR(e.what()));
				}
				catch (...)
				{
					UE_LOG(LogConvaiEditor, Error,
						   TEXT("ServiceScope: unknown exception during service shutdown: %s"),
						   *Pair.Key.ToString());
				}
			}
		}

		ScopedServices.Empty();
	}

	TSharedPtr<FViewModelBase> FServiceScope::GetScopedViewModel(FName ViewModelType) const
	{
		FScopeLock Lock(&ViewModelsMutex);

		const TSharedPtr<FViewModelBase> *Found = ScopedViewModels.Find(ViewModelType);
		return Found ? *Found : nullptr;
	}

	void FServiceScope::AddScopedViewModel(FName ViewModelType, TSharedPtr<FViewModelBase> ViewModel)
	{
		FScopeLock Lock(&ViewModelsMutex);

		if (ScopedViewModels.Contains(ViewModelType))
		{
			UE_LOG(LogConvaiEditor, Warning,
				   TEXT("ServiceScope: replacing existing scoped ViewModel '%s' in scope '%s'"),
				   *ViewModelType.ToString(), *ScopeName);
		}

		ScopedViewModels.Add(ViewModelType, ViewModel);
	}

	bool FServiceScope::RemoveScopedViewModel(FName ViewModelType)
	{
		FScopeLock Lock(&ViewModelsMutex);

		if (TSharedPtr<FViewModelBase> *Found = ScopedViewModels.Find(ViewModelType))
		{
			if (Found->IsValid())
			{
				(*Found)->Shutdown();
			}

			ScopedViewModels.Remove(ViewModelType);

			return true;
		}

		return false;
	}

	int32 FServiceScope::GetScopedViewModelCount() const
	{
		FScopeLock Lock(&ViewModelsMutex);
		return ScopedViewModels.Num();
	}

	void FServiceScope::ClearAllViewModels()
	{
		FScopeLock Lock(&ViewModelsMutex);

		if (ScopedViewModels.Num() == 0)
		{
			return;
		}

		for (auto &Pair : ScopedViewModels)
		{
			if (Pair.Value.IsValid())
			{
				try
				{
					Pair.Value->Shutdown();
				}
				catch (const std::exception &e)
				{
					UE_LOG(LogConvaiEditor, Error,
						   TEXT("ServiceScope: exception during ViewModel shutdown '%s': %s"),
						   *Pair.Key.ToString(), UTF8_TO_TCHAR(e.what()));
				}
				catch (...)
				{
					UE_LOG(LogConvaiEditor, Error,
						   TEXT("ServiceScope: unknown exception during ViewModel shutdown: %s"),
						   *Pair.Key.ToString());
				}
			}
		}

		ScopedViewModels.Empty();
	}

	FScopeManager::FScopeManager()
	{
	}

	FScopeManager::~FScopeManager()
	{
		ClearAllScopes();
	}

	TSharedPtr<FServiceScope> FScopeManager::CreateScope(const FString &ScopeName)
	{
		FScopeLock Lock(&ScopeMutex);

		TSharedPtr<FServiceScope> NewScope = MakeShared<FServiceScope>(ScopeName);

		AllScopes.Add(NewScope);

		ScopeStack.Add(NewScope);

		return NewScope;
	}

	TSharedPtr<FServiceScope> FScopeManager::GetCurrentScope() const
	{
		FScopeLock Lock(&ScopeMutex);

		if (ScopeStack.Num() > 0)
		{
			return ScopeStack.Last();
		}

		return nullptr;
	}

	void FScopeManager::PushScope(TSharedPtr<FServiceScope> Scope)
	{
		if (!Scope.IsValid())
		{
			UE_LOG(LogConvaiEditor, Warning, TEXT("ScopeManager: invalid scope push attempt"));
			return;
		}

		FScopeLock Lock(&ScopeMutex);

		ScopeStack.Add(Scope);
	}

	TSharedPtr<FServiceScope> FScopeManager::PopScope()
	{
		FScopeLock Lock(&ScopeMutex);

		if (ScopeStack.Num() == 0)
		{
			UE_LOG(LogConvaiEditor, Warning, TEXT("ScopeManager: invalid scope pop attempt - stack empty"));
			return nullptr;
		}

		TSharedPtr<FServiceScope> PoppedScope = ScopeStack.Pop();

		return PoppedScope;
	}

	void FScopeManager::DestroyScope(TSharedPtr<FServiceScope> Scope)
	{
		if (!Scope.IsValid())
		{
			return;
		}

		FScopeLock Lock(&ScopeMutex);

		FString ScopeName = Scope->GetScopeName();

		ScopeStack.Remove(Scope);

		AllScopes.RemoveAll([&Scope](const TWeakPtr<FServiceScope> &WeakScope)
							{
			                    TSharedPtr<FServiceScope> PinnedScope = WeakScope.Pin();
			                    return !PinnedScope.IsValid() || PinnedScope == Scope; });
	}

	int32 FScopeManager::GetActiveScopeCount() const
	{
		FScopeLock Lock(&ScopeMutex);
		return ScopeStack.Num();
	}

	bool FScopeManager::IsScopeActive(const TSharedPtr<FServiceScope> &Scope) const
	{
		if (!Scope.IsValid())
		{
			return false;
		}

		FScopeLock Lock(&ScopeMutex);
		return ScopeStack.Contains(Scope);
	}

	void FScopeManager::ClearAllScopes()
	{
		FScopeLock Lock(&ScopeMutex);

		if (ScopeStack.Num() == 0)
		{
			return;
		}

		while (ScopeStack.Num() > 0)
		{
			TSharedPtr<FServiceScope> Scope = ScopeStack.Pop();
			if (Scope.IsValid())
			{
			}
		}

		AllScopes.Empty();
	}

	FScopeManager::FScopeStats FScopeManager::GetScopeStats() const
	{
		FScopeLock Lock(&ScopeMutex);

		FScopeStats Stats;
		Stats.ActiveScopes = ScopeStack.Num();
		Stats.TotalScopedServices = 0;
		Stats.TotalScopedViewModels = 0;

		for (const TSharedPtr<FServiceScope> &Scope : ScopeStack)
		{
			if (Scope.IsValid())
			{
				Stats.ScopeNames.Add(Scope->GetScopeName());
				Stats.TotalScopedServices += Scope->GetScopedServiceCount();
				Stats.TotalScopedViewModels += Scope->GetScopedViewModelCount();
			}
		}

		return Stats;
	}

} // namespace ConvaiEditor
