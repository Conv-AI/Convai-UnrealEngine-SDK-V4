/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiErrorHandling.h
 *
 * Utilities for consistent error handling using TConvaiResult pattern.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Services/ConvaiDIContainer.h"
#include "Modules/ModuleManager.h"

namespace ConvaiEditor
{
    /** Error handling utilities */
    namespace ErrorHandling
    {
        /** Safe module access with Result pattern */
        template <typename TModule>
        TConvaiResult<TModule *> GetModuleSafe(const FName &ModuleName)
        {
            if (!FModuleManager::Get().IsModuleLoaded(ModuleName))
            {
                return TConvaiResult<TModule *>::Failure(
                    FString::Printf(TEXT("Module '%s' is not loaded"), *ModuleName.ToString()));
            }

            TModule *Module = FModuleManager::GetModulePtr<TModule>(ModuleName);
            if (!Module)
            {
                return TConvaiResult<TModule *>::Failure(
                    FString::Printf(TEXT("Module '%s' cast failed - type mismatch"), *ModuleName.ToString()));
            }

            return TConvaiResult<TModule *>::Success(Module);
        }

        /** Safe module loading with Result pattern */
        template <typename TModule>
        TConvaiResult<TModule *> LoadModuleSafe(const FName &ModuleName)
        {
            if (!FModuleManager::Get().IsModuleLoaded(ModuleName))
            {
                if (!FModuleManager::Get().LoadModule(ModuleName))
                {
                    return TConvaiResult<TModule *>::Failure(
                        FString::Printf(TEXT("Failed to load module '%s'"), *ModuleName.ToString()));
                }
            }

            return GetModuleSafe<TModule>(ModuleName);
        }

        /** Safe window operation wrapper */
        inline TConvaiResult<void> SafeOpenConvaiWindow(bool bShouldBeTopmost = false)
        {
            auto ModuleResult = GetModuleSafe<FConvaiEditorModule>(TEXT("ConvaiEditor"));
            if (ModuleResult.IsFailure())
            {
                return TConvaiResult<void>::Failure(
                    FString::Printf(TEXT("Cannot open Convai window: %s"), *ModuleResult.GetError()));
            }

            ModuleResult.GetValue()->OpenConvaiWindow(bShouldBeTopmost);

            return TConvaiResult<void>::Success();
        }

        /** Execute a function with Result-based error handling */
        template <typename TFunc>
        TConvaiResult<void> ExecuteSafely(TFunc Function, const TCHAR *OperationName)
        {
            try
            {
                Function();
                return TConvaiResult<void>::Success();
            }
            catch (const std::exception &e)
            {
                return TConvaiResult<void>::Failure(
                    FString::Printf(TEXT("%s failed with exception: %s"),
                                    OperationName, UTF8_TO_TCHAR(e.what())));
            }
            catch (...)
            {
                return TConvaiResult<void>::Failure(
                    FString::Printf(TEXT("%s failed with unknown exception"), OperationName));
            }
        }

        /** Execute a function with return value and Result-based error handling */
        template <typename TReturn, typename TFunc>
        TConvaiResult<TReturn> ExecuteSafelyWithReturn(TFunc Function, const TCHAR *OperationName)
        {
            try
            {
                TReturn Result = Function();
                return TConvaiResult<TReturn>::Success(Result);
            }
            catch (const std::exception &e)
            {
                return TConvaiResult<TReturn>::Failure(
                    FString::Printf(TEXT("%s failed with exception: %s"),
                                    OperationName, UTF8_TO_TCHAR(e.what())));
            }
            catch (...)
            {
                return TConvaiResult<TReturn>::Failure(
                    FString::Printf(TEXT("%s failed with unknown exception"), OperationName));
            }
        }

    } // namespace ErrorHandling
} // namespace ConvaiEditor
