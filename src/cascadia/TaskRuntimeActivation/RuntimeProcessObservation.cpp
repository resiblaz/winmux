// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeProcessObservation.h"
#include "RuntimeWindowsProcessApiInternal.h"

namespace Microsoft::Terminal::TaskRuntime
{
    RuntimeProcessObservation::RuntimeProcessObservation(
        std::shared_ptr<details::IRuntimeWindowsProcessApi> api) noexcept :
        _api{ std::move(api) }
    {
    }

    RuntimeProcessObservation::~RuntimeProcessObservation()
    {
        Close();
    }

    HANDLE RuntimeProcessObservation::WaitHandle() const noexcept
    {
        return _process;
    }

    DWORD RuntimeProcessObservation::ProcessId() const noexcept
    {
        return _process ? _processId : 0;
    }

    RuntimeProcessExitResult RuntimeProcessObservation::QueryExit() noexcept
    {
        if (!_api || !_process)
        {
            return { RuntimeProcessExitStatus::Closed, 0, ERROR_INVALID_HANDLE };
        }
        const auto waited = _api->PollHandle(_process);
        if (waited.Status == details::RuntimeWindowsWaitStatus::NotSignaled)
        {
            return { RuntimeProcessExitStatus::Running, STILL_ACTIVE, ERROR_SUCCESS };
        }
        if (waited.Status != details::RuntimeWindowsWaitStatus::Signaled)
        {
            return { RuntimeProcessExitStatus::Failed, 0, waited.SystemError };
        }

        DWORD exitCode{};
        DWORD error{};
        if (!_api->ProcessExitCode(_process, exitCode, error) || exitCode == STILL_ACTIVE)
        {
            return {
                RuntimeProcessExitStatus::Failed,
                exitCode,
                error == ERROR_SUCCESS ? ERROR_INVALID_STATE : error,
            };
        }
        return { RuntimeProcessExitStatus::Exited, exitCode, ERROR_SUCCESS };
    }

    void RuntimeProcessObservation::Close() noexcept
    {
        if (_process && _api)
        {
            _api->CloseHandle(_process);
        }
        _process = nullptr;
        _processId = 0;
    }

    void RuntimeProcessObservation::_adopt(
        const HANDLE process,
        const DWORD processId) noexcept
    {
        _process = process;
        _processId = processId;
    }
}
