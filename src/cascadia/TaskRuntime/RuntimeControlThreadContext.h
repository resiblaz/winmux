// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <atomic>
#include <windows.h>

namespace Microsoft::Terminal::TaskRuntime
{
    // Marks threads owned by the control server. A synchronous Stop from one
    // of these threads may request shutdown, but must never join the acceptor
    // or sessions because that would join the caller through the acceptor.
    class RuntimeControlThreadScope final
    {
    public:
        RuntimeControlThreadScope() noexcept;
        ~RuntimeControlThreadScope();

        RuntimeControlThreadScope(const RuntimeControlThreadScope&) = delete;
        RuntimeControlThreadScope& operator=(const RuntimeControlThreadScope&) = delete;

    private:
        bool _previous{};
    };

    class RuntimeControlThreadCompletionScope final
    {
    public:
        RuntimeControlThreadCompletionScope(
            std::atomic_bool& completed,
            HANDLE notificationEvent) noexcept;
        ~RuntimeControlThreadCompletionScope();

        RuntimeControlThreadCompletionScope(const RuntimeControlThreadCompletionScope&) = delete;
        RuntimeControlThreadCompletionScope& operator=(const RuntimeControlThreadCompletionScope&) = delete;

    private:
        std::atomic_bool& _completed;
        HANDLE _notificationEvent{};
    };

    [[nodiscard]] bool IsRuntimeControlWorkerThread() noexcept;
}
