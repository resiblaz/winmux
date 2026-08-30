// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeWindowsProcessApiInternal.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    struct RuntimeSuspendedRollbackResult
    {
        bool Succeeded{};
        DWORD SystemError{};
    };

    class RuntimeSuspendedProcess final
    {
    public:
        RuntimeSuspendedProcess(
            std::shared_ptr<IRuntimeWindowsProcessApi> api,
            RuntimeWindowsCreatedProcess process) noexcept;
        ~RuntimeSuspendedProcess();

        RuntimeSuspendedProcess(const RuntimeSuspendedProcess&) = delete;
        RuntimeSuspendedProcess& operator=(const RuntimeSuspendedProcess&) = delete;

        [[nodiscard]] HANDLE Process() const noexcept;
        [[nodiscard]] HANDLE PrimaryThread() const noexcept;
        [[nodiscard]] RuntimeSuspendedRollbackResult Rollback() noexcept;
        [[nodiscard]] RuntimeWindowsCreatedProcess TakeForContainment() noexcept;
        [[nodiscard]] RuntimeWindowsCreatedProcess Commit() noexcept;

    private:
        void _close() noexcept;

        std::shared_ptr<IRuntimeWindowsProcessApi> _api;
        RuntimeWindowsCreatedProcess _process;
        bool _committed{};
    };
}
