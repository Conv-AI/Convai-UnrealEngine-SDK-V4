/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ScopedDelegateHandle.h
 *
 * RAII wrapper for FDelegateHandle that automatically removes delegates.
 */

#pragma once

#include "CoreMinimal.h"

namespace ConvaiEditor
{
    /** RAII wrapper for FDelegateHandle that automatically removes the delegate */
    template <typename DelegateType>
    class TScopedDelegateHandle
    {
    public:
        TScopedDelegateHandle()
            : Delegate(nullptr), Handle()
        {
        }

        TScopedDelegateHandle(DelegateType &InDelegate, FDelegateHandle InHandle)
            : Delegate(&InDelegate), Handle(InHandle)
        {
        }

        /** Destructor automatically removes the delegate */
        ~TScopedDelegateHandle()
        {
            Reset();
        }

        /** Move constructor */
        TScopedDelegateHandle(TScopedDelegateHandle &&Other)
            : Delegate(Other.Delegate), Handle(Other.Handle)
        {
            Other.Delegate = nullptr;
            Other.Handle.Reset();
        }

        /** Move assignment */
        TScopedDelegateHandle &operator=(TScopedDelegateHandle &&Other)
        {
            if (this != &Other)
            {
                Reset();
                Delegate = Other.Delegate;
                Handle = Other.Handle;
                Other.Delegate = nullptr;
                Other.Handle.Reset();
            }
            return *this;
        }

        TScopedDelegateHandle(const TScopedDelegateHandle &) = delete;
        TScopedDelegateHandle &operator=(const TScopedDelegateHandle &) = delete;

        /** Manually remove the delegate before destruction */
        void Reset()
        {
            if (Delegate && Handle.IsValid())
            {
                Delegate->Remove(Handle);
                Handle.Reset();
            }
            Delegate = nullptr;
        }

        /** Check if the handle is valid */
        bool IsValid() const
        {
            return Delegate != nullptr && Handle.IsValid();
        }

        /** Get the underlying delegate handle */
        FDelegateHandle GetHandle() const
        {
            return Handle;
        }

    private:
        DelegateType *Delegate;
        FDelegateHandle Handle;
    };

    /** Helper function to create a scoped delegate handle */
    template <typename DelegateType>
    TScopedDelegateHandle<DelegateType> MakeScopedDelegateHandle(DelegateType &Delegate, FDelegateHandle Handle)
    {
        return TScopedDelegateHandle<DelegateType>(Delegate, Handle);
    }

} // namespace ConvaiEditor
