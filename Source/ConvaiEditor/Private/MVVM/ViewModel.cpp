/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ViewModel.cpp
 *
 * Implementation of the base ViewModel class.
 */

#include "MVVM/ViewModel.h"
#include "ConvaiEditor.h"
#include "Services/ConvaiDIContainer.h"

FViewModelBase::~FViewModelBase() = default;

bool FViewModelBase::IsA(const FName &TypeName) const
{
    return TypeName == StaticType();
}

void FViewModelBase::Initialize()
{
    bInitialized = true;
}

void FViewModelBase::Shutdown()
{
    if (BoundDelegateHandles.Num() > 0)
    {
        BoundDelegateHandles.Empty();
    }

    bShutdown = true;
}

TUniquePtr<FViewModelRegistry> FViewModelRegistry::Instance = nullptr;

FViewModelRegistry &FViewModelRegistry::Get()
{
    check(Instance);
    return *Instance;
}

void FViewModelRegistry::Initialize()
{
    check(!Instance);
    Instance = MakeUnique<FViewModelRegistry>();
}

void FViewModelRegistry::Shutdown()
{
    if (Instance.IsValid())
    {
        Instance->UnregisterAllViewModels();
        Instance.Reset();
    }
}

void FViewModelRegistry::RegisterViewModel(const FName &TypeName, TSharedPtr<FViewModelBase> ViewModel)
{
    if (!ViewModel.IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Attempted to register null ViewModel for type: %s"), *TypeName.ToString());
        return;
    }

    FScopeLock Lock(&ViewModelMutex);

    // Check if a ViewModel of this type already exists
    TSharedPtr<FViewModelBase> *ExistingViewModel = ViewModelMap.Find(TypeName);
    if (ExistingViewModel && ExistingViewModel->IsValid())
    {
        (*ExistingViewModel)->Shutdown();
    }

    ViewModelMap.Add(TypeName, ViewModel);

    if (!ViewModel->IsInitialized())
    {
        ViewModel->Initialize();
    }
}

void FViewModelRegistry::UnregisterViewModel(const FName &TypeName)
{
    FScopeLock Lock(&ViewModelMutex);

    TSharedPtr<FViewModelBase> *ViewModel = ViewModelMap.Find(TypeName);
    if (ViewModel && ViewModel->IsValid())
    {
        (*ViewModel)->Shutdown();
        ViewModelMap.Remove(TypeName);
    }
}

void FViewModelRegistry::UnregisterAllViewModels()
{
    FScopeLock Lock(&ViewModelMutex);

    for (auto &Pair : ViewModelMap)
    {
        if (Pair.Value.IsValid())
        {
            Pair.Value->Shutdown();
        }
    }

    ViewModelMap.Empty();
}

TSharedPtr<ConvaiEditor::FServiceScope> FViewModelRegistry::GetCurrentServiceScope() const
{
    if (!FConvaiDIContainerManager::IsInitialized())
    {
        return nullptr;
    }

    return FConvaiDIContainerManager::GetCurrentScope();
}
