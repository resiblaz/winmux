// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlThreadContext.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        thread_local bool _runtimeControlWorkerThread{};
    }

    RuntimeControlThreadScope::RuntimeControlThreadScope() noexcept :
        _previous{ std::exchange(_runtimeControlWorkerThread, true) }
    {
    }

    RuntimeControlThreadScope::~RuntimeControlThreadScope()
    {
        _runtimeControlWorkerThread = _previous;
    }

    RuntimeControlThreadCompletionScope::RuntimeControlThreadCompletionScope(
        std::atomic_bool& completed,
        const HANDLE notificationEvent) noexcept :
        _completed{ completed },
        _notificationEvent{ notificationEvent }
    {
    }

    RuntimeControlThreadCompletionScope::~RuntimeControlThreadCompletionScope()
    {
        _completed.store(true, std::memory_order_release);
        if (_notificationEvent)
        {
            SetEvent(_notificationEvent);
        }
    }

    bool IsRuntimeControlWorkerThread() noexcept
    {
        return _runtimeControlWorkerThread;
    }
}
