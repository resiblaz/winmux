// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

namespace Microsoft::Terminal::TaskRuntime
{
    class RuntimeConsoleStop final
    {
    public:
        RuntimeConsoleStop() noexcept;
        ~RuntimeConsoleStop();

        RuntimeConsoleStop(const RuntimeConsoleStop&) = delete;
        RuntimeConsoleStop& operator=(const RuntimeConsoleStop&) = delete;
        RuntimeConsoleStop(RuntimeConsoleStop&&) = delete;
        RuntimeConsoleStop& operator=(RuntimeConsoleStop&&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] HANDLE Handle() const noexcept;
        // Releases console-close handlers after the Runtime has completed all
        // server/session cleanup. Safe and idempotent on every exit path.
        void Complete() noexcept;

    private:
        HANDLE _event{};
        HANDLE _cleanupComplete{};
        HANDLE _handlersIdle{};
        bool _installed{};
    };
}
