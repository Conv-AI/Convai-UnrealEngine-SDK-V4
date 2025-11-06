/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ViewModel.h
 *
 * Base ViewModel class for MVVM architecture.
 */

#pragma once

#include "Templates/SharedPointer.h"
#include "Delegates/Delegate.h"
#include "HAL/CriticalSection.h"
#include "ConvaiEditor.h"
#include "Services/ServiceScope.h"

/** Base ViewModel class for MVVM architecture. */
class CONVAIEDITOR_API FViewModelBase : public TSharedFromThis<FViewModelBase>
{
public:
    /** Virtual destructor for proper cleanup of derived classes */
    virtual ~FViewModelBase();

    /** Returns the type name used for registry lookup */
    static FName StaticType() { return TEXT("FViewModelBase"); }

    /** Returns whether this ViewModel is of the specified type */
    virtual bool IsA(const FName &TypeName) const;

    /** Called after all dependencies are set but before UI binding */
    virtual void Initialize();

    /** Called when the ViewModel is no longer needed and can release resources */
    virtual void Shutdown();

    /** Returns whether the ViewModel has been initialized */
    bool IsInitialized() const { return bInitialized; }

    /** Returns whether the ViewModel has been shut down */
    bool IsShutdown() const { return bShutdown; }

    /** Delegate fired when ViewModel requests UI refresh */
    DECLARE_MULTICAST_DELEGATE(FOnInvalidated);
    FOnInvalidated &OnInvalidated() { return InvalidatedDelegate; }

    /** Delegate fired when loading state changes */
    DECLARE_MULTICAST_DELEGATE_TwoParams(FOnLoadingStateChanged, bool /* bIsLoading */, const FText & /* Message */);
    FOnLoadingStateChanged &OnLoadingStateChanged() { return LoadingStateChangedDelegate; }

    /** Returns whether the ViewModel is currently in a loading state */
    bool IsLoading() const { return bIsLoading; }

    /** Returns the current loading message */
    FText GetLoadingMessage() const { return LoadingMessage; }

    /** Set the loading state and notify subscribers */
    void SetLoadingState(bool bInIsLoading, const FText &InMessage = FText::GetEmpty())
    {
        if (bIsLoading != bInIsLoading || !LoadingMessage.EqualTo(InMessage))
        {
            bIsLoading = bInIsLoading;
            LoadingMessage = InMessage;
            LoadingStateChangedDelegate.Broadcast(bIsLoading, LoadingMessage);

            // Also trigger a general invalidation when loading state changes
            BroadcastInvalidated();
        }
    }

    /** Start loading with a message */
    void StartLoading(const FText &InMessage = FText::FromString(TEXT("Loading...")))
    {
        SetLoadingState(true, InMessage);
    }

    /** Stop loading */
    void StopLoading()
    {
        SetLoadingState(false, FText::GetEmpty());
    }

protected:
    /** Track a delegate handle for automatic cleanup during Shutdown() */
    void TrackDelegateHandle(FDelegateHandle Handle)
    {
        if (Handle.IsValid())
        {
            BoundDelegateHandles.Add(Handle);
        }
    }

    /** Track multiple delegate handles at once for automatic cleanup */
    void TrackDelegateHandles(const TArray<FDelegateHandle> &Handles)
    {
        for (const FDelegateHandle &Handle : Handles)
        {
            TrackDelegateHandle(Handle);
        }
    }

    /** Manually untrack a delegate handle (useful if manually cleaning up before Shutdown) */
    bool UntrackDelegateHandle(FDelegateHandle Handle)
    {
        return BoundDelegateHandles.RemoveSingle(Handle) > 0;
    }

    /** Get the number of currently tracked delegate handles */
    int32 GetTrackedDelegateCount() const
    {
        return BoundDelegateHandles.Num();
    }
    /** Notify UI that data has changed and views should refresh */
    void BroadcastInvalidated() { InvalidatedDelegate.Broadcast(); }

    /**
     * Flag indicating initialization state.
     *
     * Tracks whether the ViewModel has been properly initialized
     * and is ready for use. This flag is set to true when Initialize()
     * is called and can be used to prevent operations on uninitialized
     * ViewModels.
     */
    bool bInitialized = false;

    /**
     * Flag indicating shutdown state.
     *
     * Tracks whether the ViewModel has been properly shut down
     * and is no longer usable. This flag is set to true when Shutdown()
     * is called and can be used to prevent operations on shut down
     * ViewModels.
     */
    bool bShutdown = false;

    /**
     * Collection of all delegate handles bound by this ViewModel.
     *
     * This array tracks all delegate bindings made by the ViewModel
     * to ensure they can be properly cleaned up during Shutdown().
     * Use TrackDelegateHandle() to add handles to this collection.
     *
     * CRITICAL: All delegate bindings MUST be tracked here to prevent
     * memory leaks and crashes from dangling delegate references.
     */
    TArray<FDelegateHandle> BoundDelegateHandles;

    /**
     * Flag indicating whether the ViewModel is currently loading data.
     */
    bool bIsLoading = false;

    /**
     * Message describing what is currently being loaded.
     */
    FText LoadingMessage;

private:
    /**
     * Event fired when the ViewModel's data changes.
     *
     * This delegate is used internally to notify subscribers about
     * data changes. Views and other components can subscribe to
     * this delegate to receive notifications about ViewModel updates.
     */
    FOnInvalidated InvalidatedDelegate;

    /**
     * Event fired when loading state changes.
     */
    FOnLoadingStateChanged LoadingStateChangedDelegate;
};

/** FViewModelRegistry - Centralized Registry for ViewModels */
class CONVAIEDITOR_API FViewModelRegistry
{
public:
    /** Singleton accessor for the ViewModel registry */
    static FViewModelRegistry &Get();

    /** Initialize the registry system - called during module startup */
    static void Initialize();

    /** Shutdown the registry system - called during module shutdown */
    static void Shutdown();

    /** Register a ViewModel instance by type */
    void RegisterViewModel(const FName &TypeName, TSharedPtr<FViewModelBase> ViewModel);

    /** Create and register a ViewModel of the specified type (Singleton) */
    template <typename ViewModelType, typename... Args>
    TSharedPtr<ViewModelType> CreateViewModel(Args &&...InArgs)
    {
        FScopeLock Lock(&ViewModelMutex);

        // Create the ViewModel with the provided arguments
        TSharedPtr<ViewModelType> ViewModel = MakeShared<ViewModelType>(Forward<Args>(InArgs)...);

        // Register it in the global registry using the ViewModel's static type
        ViewModelMap.Add(ViewModelType::StaticType(), StaticCastSharedPtr<FViewModelBase>(ViewModel));

        return ViewModel;
    }

    /** Create and register a scoped ViewModel (Per-Window) */
    template <typename ViewModelType, typename... Args>
    TSharedPtr<ViewModelType> CreateScopedViewModel(Args &&...InArgs)
    {
        // Get current scope from DI container
        TSharedPtr<ConvaiEditor::FServiceScope> CurrentScope = GetCurrentServiceScope();
        if (!CurrentScope.IsValid())
        {
            UE_LOG(LogConvaiEditor, Error,
                   TEXT("Cannot create scoped ViewModel '%s' - no active service scope"),
                   *ViewModelType::StaticType().ToString());
            return nullptr;
        }

        // Check if instance already exists in this scope
        FName ViewModelKey = ViewModelType::StaticType();
        TSharedPtr<FViewModelBase> ExistingViewModel = CurrentScope->GetScopedViewModel(ViewModelKey);
        if (ExistingViewModel.IsValid())
        {
            return StaticCastSharedPtr<ViewModelType>(ExistingViewModel);
        }

        // Create new instance for this scope
        TSharedPtr<ViewModelType> ViewModel = MakeShared<ViewModelType>(Forward<Args>(InArgs)...);

        // Cache in scope (auto-cleanup when scope destroyed)
        CurrentScope->AddScopedViewModel(ViewModelKey, StaticCastSharedPtr<FViewModelBase>(ViewModel));

        return ViewModel;
    }

    /** Resolve a scoped ViewModel from current scope */
    template <typename ViewModelType>
    TSharedPtr<ViewModelType> ResolveScopedViewModel()
    {
        TSharedPtr<ConvaiEditor::FServiceScope> CurrentScope = GetCurrentServiceScope();
        if (!CurrentScope.IsValid())
        {
            return nullptr;
        }

        TSharedPtr<FViewModelBase> ViewModel = CurrentScope->GetScopedViewModel(ViewModelType::StaticType());
        if (ViewModel.IsValid())
        {
            return StaticCastSharedPtr<ViewModelType>(ViewModel);
        }

        return nullptr;
    }

    /** Resolve a ViewModel by type */
    template <typename ViewModelType>
    TSharedPtr<ViewModelType> ResolveViewModel()
    {
        FScopeLock Lock(&ViewModelMutex);

        TSharedPtr<FViewModelBase> *FoundViewModel = ViewModelMap.Find(ViewModelType::StaticType());
        if (FoundViewModel && FoundViewModel->IsValid())
        {
            return StaticCastSharedPtr<ViewModelType>(*FoundViewModel);
        }

        return nullptr;
    }

    /** Unregister and shutdown a ViewModel */
    void UnregisterViewModel(const FName &TypeName);

    /** Unregister and shutdown all ViewModels */
    void UnregisterAllViewModels();

private:
    /** Get current service scope from DI container */
    TSharedPtr<ConvaiEditor::FServiceScope> GetCurrentServiceScope() const;

    /** Thread-safe storage of all ViewModels */
    FCriticalSection ViewModelMutex;

    /** Map of ViewModel instances by type name */
    TMap<FName, TSharedPtr<FViewModelBase>> ViewModelMap;

    /** Singleton instance of the ViewModel registry */
    static TUniquePtr<FViewModelRegistry> Instance;
};
