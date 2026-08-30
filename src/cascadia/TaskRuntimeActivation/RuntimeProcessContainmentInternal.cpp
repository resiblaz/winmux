// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeProcessContainmentInternal.h"

#include <thread>

namespace Microsoft::Terminal::TaskRuntime::details
{
    namespace
    {
        constexpr DWORD QuarantineExitCode{ 0xe000d2b2 };
        constexpr DWORD QuarantineRetryMilliseconds{ 1000 };
    }

    struct RuntimeProcessContainment::State
    {
        std::shared_ptr<IRuntimeWindowsProcessApi> Api;
        RuntimeWindowsCreatedProcess Process;
    };

    namespace
    {
        void _runQuarantine(const std::shared_ptr<RuntimeProcessContainment::State>& state) noexcept
        {
            for (;;)
            {
                DWORD ignored{};
                (void)state->Api->TerminateProcessBeforeCommit(
                    state->Process.Process,
                    QuarantineExitCode,
                    ignored);
                const auto waited = state->Api->WaitForProcess(
                    state->Process.Process,
                    QuarantineRetryMilliseconds);
                if (waited.Status == RuntimeWindowsWaitStatus::Signaled)
                {
                    break;
                }
                if (waited.Status == RuntimeWindowsWaitStatus::Failed)
                {
                    Sleep(QuarantineRetryMilliseconds);
                }
            }
            state->Api->CloseHandle(state->Process.Process);
            state->Process.Process = nullptr;
        }
    }

    RuntimeProcessContainment::RuntimeProcessContainment(
        std::shared_ptr<IRuntimeWindowsProcessApi> api) :
        _state{ std::make_shared<State>() }
    {
        _state->Api = std::move(api);
    }

    void RuntimeProcessContainment::Quarantine(
        RuntimeWindowsCreatedProcess process) noexcept
    {
        if (process.PrimaryThread)
        {
            _state->Api->CloseHandle(process.PrimaryThread);
            process.PrimaryThread = nullptr;
        }
        _state->Process = process;
        try
        {
            std::thread worker{ [state = _state] {
                _runQuarantine(state);
            } };
            worker.detach();
        }
        catch (...)
        {
            // Failing to create the exceptional-path worker must not abandon
            // a live suspended process. Retry synchronously until contained.
            _runQuarantine(_state);
        }
    }
}
