// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeSuspendedProcessInternal.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    namespace
    {
        constexpr DWORD SuspendedRollbackExitCode{ 0xe000d2b1 };
        constexpr DWORD SuspendedRollbackWaitMilliseconds{ 5000 };
    }

    RuntimeSuspendedProcess::RuntimeSuspendedProcess(
        std::shared_ptr<IRuntimeWindowsProcessApi> api,
        const RuntimeWindowsCreatedProcess process) noexcept :
        _api{ std::move(api) },
        _process{ process }
    {
    }

    RuntimeSuspendedProcess::~RuntimeSuspendedProcess()
    {
        if (!_committed)
        {
            (void)Rollback();
        }
    }

    HANDLE RuntimeSuspendedProcess::Process() const noexcept
    {
        return _process.Process;
    }

    HANDLE RuntimeSuspendedProcess::PrimaryThread() const noexcept
    {
        return _process.PrimaryThread;
    }

    RuntimeSuspendedRollbackResult RuntimeSuspendedProcess::Rollback() noexcept
    {
        if (_committed || !_process.Process)
        {
            return { true, ERROR_SUCCESS };
        }
        DWORD terminateError{};
        const auto terminated = _api->TerminateProcessBeforeCommit(
            _process.Process,
            SuspendedRollbackExitCode,
            terminateError);
        const auto waited = _api->WaitForProcess(
            _process.Process,
            SuspendedRollbackWaitMilliseconds);
        const auto succeeded = waited.Status == RuntimeWindowsWaitStatus::Signaled;
        const auto error = succeeded ? ERROR_SUCCESS :
                           waited.SystemError != ERROR_SUCCESS ? waited.SystemError :
                           !terminated && terminateError != ERROR_SUCCESS ? terminateError :
                           WAIT_TIMEOUT;
        if (succeeded)
        {
            _close();
        }
        return { succeeded, error };
    }

    RuntimeWindowsCreatedProcess RuntimeSuspendedProcess::TakeForContainment() noexcept
    {
        _committed = true;
        return std::exchange(_process, {});
    }

    RuntimeWindowsCreatedProcess RuntimeSuspendedProcess::Commit() noexcept
    {
        _committed = true;
        if (_process.PrimaryThread)
        {
            _api->CloseHandle(_process.PrimaryThread);
            _process.PrimaryThread = nullptr;
        }
        return std::exchange(_process, {});
    }

    void RuntimeSuspendedProcess::_close() noexcept
    {
        if (_process.PrimaryThread)
        {
            _api->CloseHandle(_process.PrimaryThread);
        }
        if (_process.Process)
        {
            _api->CloseHandle(_process.Process);
        }
        _process = {};
    }
}
