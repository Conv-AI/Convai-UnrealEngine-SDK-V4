/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiDIContainer.cpp
 *
 * Implementation of the professional DI container system.
 */

#include "Services/ConvaiDIContainer.h"
#include "ConvaiEditor.h"
#include "Services/ServiceScope.h"

TUniquePtr<IConvaiDIContainer> FConvaiDIContainerManager::Instance = nullptr;
bool FConvaiDIContainerManager::bIsInitialized = false;

FConvaiDIContainer::FConvaiDIContainer()
{
    ScopeManager = MakeUnique<ConvaiEditor::FScopeManager>();
}

FConvaiDIContainer::~FConvaiDIContainer()
{
    Clear();
}

void FConvaiDIContainer::Clear()
{
    FWriteScopeLock WriteLock(ServicesLock);

    UE_LOG(LogConvaiEditor, Warning, TEXT("DI Container clearing %d registered services"), ServiceDescriptors.Num());

    for (auto &ServicePair : ServiceDescriptors)
    {
        if (ServicePair.Value.IsValid() && ServicePair.Value->SingletonInstance.IsValid())
        {
            try
            {
                ServicePair.Value->SingletonInstance->Shutdown();
            }
            catch (const std::exception &e)
            {
                UE_LOG(LogConvaiEditor, Error, TEXT("Standard exception during service shutdown '%s': %s"),
                       *ServicePair.Key.ToString(), UTF8_TO_TCHAR(e.what()));
            }
            catch (...)
            {
                UE_LOG(LogConvaiEditor, Error, TEXT("Unknown exception during service shutdown: %s"),
                       *ServicePair.Key.ToString());
            }

            ServicePair.Value->SingletonInstance.Reset();
        }
    }

    ServiceDescriptors.Empty();
}

IConvaiDIContainer::FContainerStats FConvaiDIContainer::GetStats() const
{
    FReadScopeLock ReadLock(ServicesLock);

    FContainerStats Stats;
    Stats.RegisteredServices = ServiceDescriptors.Num();

    for (const auto &ServicePair : ServiceDescriptors)
    {
        Stats.ServiceTypes.Add(ServicePair.Key);

        if (ServicePair.Value.IsValid())
        {
            if (ServicePair.Value->Lifetime == EConvaiServiceLifetime::Singleton && ServicePair.Value->SingletonInstance.IsValid())
            {
                Stats.SingletonInstances++;
            }
            else if (ServicePair.Value->Lifetime == EConvaiServiceLifetime::Transient)
            {
                Stats.TransientServices++;
            }
        }
    }

    return Stats;
}

TConvaiResult<void> FConvaiDIContainer::RegisterServiceInternal(
    FName ServiceType,
    FServiceFactory Factory,
    EConvaiServiceLifetime Lifetime,
    FName ServiceTypeName)
{
    if (!Factory)
    {
        return TConvaiResult<void>::Failure(
            FString::Printf(TEXT("Cannot register service with null factory: %s"), *ServiceTypeName.ToString()));
    }

    FWriteScopeLock WriteLock(ServicesLock);

    if (ServiceDescriptors.Contains(ServiceType))
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Service registration conflict: replacing existing service '%s'"), *ServiceTypeName.ToString());

        auto ExistingDescriptor = ServiceDescriptors[ServiceType];
        if (ExistingDescriptor.IsValid() && ExistingDescriptor->SingletonInstance.IsValid())
        {
            ExistingDescriptor->SingletonInstance->Shutdown();
        }
    }

    auto Descriptor = MakeShared<FConvaiServiceDescriptor>(
        MoveTemp(Factory),
        Lifetime,
        ServiceTypeName);

    ServiceDescriptors.Add(ServiceType, Descriptor);

    return TConvaiResult<void>::Success();
}

TConvaiResult<TSharedPtr<IConvaiService>> FConvaiDIContainer::ResolveServiceInternal(FName ServiceType)
{
    FThreadResolutionContext &ThreadContext = GetThreadContext();

    if (++ThreadContext.Depth > MaxResolutionDepth)
    {
        --ThreadContext.Depth;
        return TConvaiResult<TSharedPtr<IConvaiService>>::Failure(
            FString::Printf(TEXT("Max resolution depth exceeded for: %s\nResolution Stack: %s"),
                            *ServiceType.ToString(), *ThreadContext.GetStackTrace()));
    }

    if (ThreadContext.IsResolving(ServiceType))
    {
        --ThreadContext.Depth;
        return TConvaiResult<TSharedPtr<IConvaiService>>::Failure(
            FString::Printf(TEXT("Circular dependency detected for: %s\nResolution Stack: %s"),
                            *ServiceType.ToString(), *ThreadContext.GetStackTrace()));
    }

    ThreadContext.ResolutionStack.Add(ServiceType);

    FReadScopeLock ReadLock(ServicesLock);

    TSharedPtr<FConvaiServiceDescriptor> *FoundDescriptor = ServiceDescriptors.Find(ServiceType);

    if (!FoundDescriptor || !FoundDescriptor->IsValid())
    {
        ThreadContext.ResolutionStack.RemoveAt(ThreadContext.ResolutionStack.Num() - 1);
        --ThreadContext.Depth;
        return TConvaiResult<TSharedPtr<IConvaiService>>::Failure(
            FString::Printf(TEXT("Service not registered: %s"), *ServiceType.ToString()));
    }

    auto Descriptor = *FoundDescriptor;
    TSharedPtr<IConvaiService> ServiceInstance;

    switch (Descriptor->Lifetime)
    {
    case EConvaiServiceLifetime::Singleton:
    {
        if (!Descriptor->SingletonInstance.IsValid())
        {
            ServiceInstance = Descriptor->Factory(this);
            if (!ServiceInstance.IsValid())
            {
                ThreadContext.ResolutionStack.RemoveAt(ThreadContext.ResolutionStack.Num() - 1);
                --ThreadContext.Depth;
                return TConvaiResult<TSharedPtr<IConvaiService>>::Failure(
                    FString::Printf(TEXT("Factory failed to create instance for: %s"), *ServiceType.ToString()));
            }

            ServiceInstance->Startup();

            Descriptor->SingletonInstance = ServiceInstance;
            Descriptor->bIsInitialized = true;
        }
        else
        {
            ServiceInstance = Descriptor->SingletonInstance;
        }
    }
    break;

    case EConvaiServiceLifetime::Transient:
    {
        ServiceInstance = Descriptor->Factory(this);
        if (!ServiceInstance.IsValid())
        {
            ThreadContext.ResolutionStack.RemoveAt(ThreadContext.ResolutionStack.Num() - 1);
            --ThreadContext.Depth;
            return TConvaiResult<TSharedPtr<IConvaiService>>::Failure(
                FString::Printf(TEXT("Factory failed to create transient instance for: %s"), *ServiceType.ToString()));
        }

        ServiceInstance->Startup();
    }
    break;

    case EConvaiServiceLifetime::Scoped:
    {
        TSharedPtr<ConvaiEditor::FServiceScope> CurrentScope = ScopeManager->GetCurrentScope();
        if (!CurrentScope.IsValid())
        {
            ThreadContext.ResolutionStack.RemoveAt(ThreadContext.ResolutionStack.Num() - 1);
            --ThreadContext.Depth;
            return TConvaiResult<TSharedPtr<IConvaiService>>::Failure(
                FString::Printf(TEXT("No active scope for scoped service resolution: %s\n"
                                     "Hint: Create a scope using FConvaiDIContainerManager::CreateScope() before resolving scoped services"),
                                *ServiceType.ToString()));
        }

        ServiceInstance = CurrentScope->GetScopedService(ServiceType);

        if (!ServiceInstance.IsValid())
        {
            ServiceInstance = Descriptor->Factory(this);
            if (!ServiceInstance.IsValid())
            {
                ThreadContext.ResolutionStack.RemoveAt(ThreadContext.ResolutionStack.Num() - 1);
                --ThreadContext.Depth;
                return TConvaiResult<TSharedPtr<IConvaiService>>::Failure(
                    FString::Printf(TEXT("Factory failed to create scoped instance for: %s"), *ServiceType.ToString()));
            }

            ServiceInstance->Startup();

            CurrentScope->AddScopedService(ServiceType, ServiceInstance);
        }
        else
        {
        }
    }
    break;
    }

    if (ThreadContext.ResolutionStack.Num() >= 2)
    {
        FName ParentServiceType = ThreadContext.ResolutionStack[ThreadContext.ResolutionStack.Num() - 2];
    }

    ThreadContext.ResolutionStack.RemoveAt(ThreadContext.ResolutionStack.Num() - 1);
    --ThreadContext.Depth;

    return TConvaiResult<TSharedPtr<IConvaiService>>::Success(ServiceInstance);
}

bool FConvaiDIContainer::IsServiceRegisteredInternal(FName ServiceType) const
{
    FReadScopeLock ReadLock(ServicesLock);
    return ServiceDescriptors.Contains(ServiceType);
}

TConvaiResult<void> FConvaiDIContainer::UnregisterServiceInternal(FName ServiceType)
{
    FWriteScopeLock WriteLock(ServicesLock);

    TSharedPtr<FConvaiServiceDescriptor> *FoundDescriptor = ServiceDescriptors.Find(ServiceType);

    if (!FoundDescriptor || !FoundDescriptor->IsValid())
    {
        return TConvaiResult<void>::Failure(
            FString::Printf(TEXT("Service not found for unregistration: %s"), *ServiceType.ToString()));
    }

    auto Descriptor = *FoundDescriptor;
    if (Descriptor.IsValid() && Descriptor->SingletonInstance.IsValid())
    {
        Descriptor->SingletonInstance->Shutdown();
    }

    ServiceDescriptors.Remove(ServiceType);

    UE_LOG(LogConvaiEditor, Log, TEXT("Unregistered service: %s"), *ServiceType.ToString());
    return TConvaiResult<void>::Success();
}

void FConvaiDIContainerManager::Initialize()
{
    if (bIsInitialized)
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("DI Container initialization attempted multiple times"));
        return;
    }

    Instance = MakeUnique<FConvaiDIContainer>();
    bIsInitialized = true;
}

void FConvaiDIContainerManager::Shutdown()
{
    if (!bIsInitialized)
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("DI Container shutdown attempted without initialization"));
        return;
    }

    if (Instance.IsValid())
    {
        Instance->Clear();
        Instance.Reset();
    }

    bIsInitialized = false;
}

IConvaiDIContainer &FConvaiDIContainerManager::Get()
{
    checkf(bIsInitialized && Instance.IsValid(),
           TEXT("DI Container not initialized. Call FConvaiDIContainerManager::Initialize() first."));

    return *Instance;
}

bool FConvaiDIContainerManager::IsInitialized()
{
    return bIsInitialized && Instance.IsValid();
}

TSharedPtr<ConvaiEditor::FServiceScope> FConvaiDIContainerManager::CreateScope(const FString &ScopeName)
{
    checkf(bIsInitialized && Instance.IsValid(),
           TEXT("DI Container not initialized. Call FConvaiDIContainerManager::Initialize() first."));

    FConvaiDIContainer *Container = static_cast<FConvaiDIContainer *>(Instance.Get());
    if (!Container || !Container->ScopeManager.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("ScopeManager not initialized"));
        return nullptr;
    }

    return Container->ScopeManager->CreateScope(ScopeName);
}

TSharedPtr<ConvaiEditor::FServiceScope> FConvaiDIContainerManager::GetCurrentScope()
{
    if (!bIsInitialized || !Instance.IsValid())
    {
        return nullptr;
    }

    FConvaiDIContainer *Container = static_cast<FConvaiDIContainer *>(Instance.Get());
    if (!Container || !Container->ScopeManager.IsValid())
    {
        return nullptr;
    }

    return Container->ScopeManager->GetCurrentScope();
}

void FConvaiDIContainerManager::PushScope(TSharedPtr<ConvaiEditor::FServiceScope> Scope)
{
    checkf(bIsInitialized && Instance.IsValid(),
           TEXT("DI Container not initialized. Call FConvaiDIContainerManager::Initialize() first."));

    FConvaiDIContainer *Container = static_cast<FConvaiDIContainer *>(Instance.Get());
    if (!Container || !Container->ScopeManager.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("ScopeManager not initialized"));
        return;
    }

    Container->ScopeManager->PushScope(Scope);
}

TSharedPtr<ConvaiEditor::FServiceScope> FConvaiDIContainerManager::PopScope()
{
    checkf(bIsInitialized && Instance.IsValid(),
           TEXT("DI Container not initialized. Call FConvaiDIContainerManager::Initialize() first."));

    FConvaiDIContainer *Container = static_cast<FConvaiDIContainer *>(Instance.Get());
    if (!Container || !Container->ScopeManager.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("ScopeManager not initialized"));
        return nullptr;
    }

    return Container->ScopeManager->PopScope();
}

void FConvaiDIContainerManager::DestroyScope(TSharedPtr<ConvaiEditor::FServiceScope> Scope)
{
    if (!bIsInitialized || !Instance.IsValid())
    {
        return;
    }

    FConvaiDIContainer *Container = static_cast<FConvaiDIContainer *>(Instance.Get());
    if (!Container || !Container->ScopeManager.IsValid())
    {
        return;
    }

    Container->ScopeManager->DestroyScope(Scope);
}

int32 FConvaiDIContainerManager::GetActiveScopeCount()
{
    if (!bIsInitialized || !Instance.IsValid())
    {
        return 0;
    }

    FConvaiDIContainer *Container = static_cast<FConvaiDIContainer *>(Instance.Get());
    if (!Container || !Container->ScopeManager.IsValid())
    {
        return 0;
    }

    return Container->ScopeManager->GetActiveScopeCount();
}
