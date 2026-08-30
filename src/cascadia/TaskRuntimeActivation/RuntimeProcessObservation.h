// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    namespace details
    {
        class IRuntimeWindowsProcessApi;
        class RuntimeProcessLauncherImpl;
    }

    enum class RuntimeProcessExitStatus
    {
        Running,
        Exited,
        Closed,
        Failed,
    };

    struct RuntimeProcessExitResult
    {
        RuntimeProcessExitStatus Status{ RuntimeProcessExitStatus::Failed };
        DWORD ExitCode{};
        DWORD SystemError{};
    };

    // Observation owns only a wait/query handle. Closing or destroying it
    // never terminates the shared Runtime process.
    class RuntimeProcessObservation final
    {
    public:
        ~RuntimeProcessObservation();

        RuntimeProcessObservation(const RuntimeProcessObservation&) = delete;
        RuntimeProcessObservation& operator=(const RuntimeProcessObservation&) = delete;
        RuntimeProcessObservation(RuntimeProcessObservation&&) = delete;
        RuntimeProcessObservation& operator=(RuntimeProcessObservation&&) = delete;

        [[nodiscard]] HANDLE WaitHandle() const noexcept;
        [[nodiscard]] DWORD ProcessId() const noexcept;
        [[nodiscard]] RuntimeProcessExitResult QueryExit() noexcept;
        void Close() noexcept;

    private:
        friend class details::RuntimeProcessLauncherImpl;
        explicit RuntimeProcessObservation(
            std::shared_ptr<details::IRuntimeWindowsProcessApi> api) noexcept;
        void _adopt(HANDLE process, DWORD processId) noexcept;

        std::shared_ptr<details::IRuntimeWindowsProcessApi> _api;
        HANDLE _process{};
        DWORD _processId{};
    };
}
