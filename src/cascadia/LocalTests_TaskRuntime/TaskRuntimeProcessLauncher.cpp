// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeProcessTestFixtures.h"
#include "TaskRuntimeProcessWin32Support.h"

#include <atomic>

namespace TaskRuntimeLocalTests::ProcessFixtures
{
    namespace
    {
        constexpr DWORD ForcedCleanupExitCode{ 0xe0000001 };
    }

    struct TaskRuntimeProcess::Impl
    {
        Impl(
            const ProductRuntimeTestIdentity& identity,
            TaskRuntimeProcessOptions options)
        {
            auto launched = details::LaunchTaskRuntimeProcess(
                identity,
                std::move(options));
            ReadyEvent = std::move(launched.ReadyEvent);
            ShutdownEvent = std::move(launched.ShutdownEvent);
            Process = std::move(launched.Process);
            ProcessId = launched.ProcessId;
            StderrCapture = std::move(launched.StderrCapture);
        }

        ~Impl()
        {
            Cleanup();
        }

        [[nodiscard]] std::string StandardError() const
        {
            return StderrCapture ? StderrCapture->Snapshot() : std::string{};
        }

        [[nodiscard]] bool WaitForExit(const DWORD timeoutMilliseconds, DWORD& exitCode) noexcept
        {
            if (!Process || WaitForSingleObject(Process.Get(), timeoutMilliseconds) != WAIT_OBJECT_0)
            {
                return false;
            }
            if (!GetExitCodeProcess(Process.Get(), &exitCode))
            {
                return false;
            }
            return true;
        }

        void Cleanup() noexcept
        {
            if (CleanupStarted.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }

            if (Process && WaitForSingleObject(Process.Get(), 0) == WAIT_TIMEOUT)
            {
                (void)SetEvent(ShutdownEvent.Get());
                const auto stopped = WaitForSingleObject(
                    Process.Get(),
                    ProductProcessExitTimeoutMilliseconds);
                if (stopped != WAIT_OBJECT_0)
                {
                    ForcedCleanup.store(true, std::memory_order_release);
                    details::LogProcessErrorNoThrow(
                        L"TaskRuntime forced cleanup after graceful-shutdown timeout",
                        ProcessId,
                        StandardError());
                    if (TerminateProcess(Process.Get(), ForcedCleanupExitCode))
                    {
                        (void)WaitForSingleObject(
                            Process.Get(),
                            ProductProcessExitTimeoutMilliseconds);
                    }
                }
            }

            Process.Reset();
            ReadyEvent.Reset();
            ShutdownEvent.Reset();
        }

        details::HandleOwner ReadyEvent;
        details::HandleOwner ShutdownEvent;
        details::HandleOwner Process;
        DWORD ProcessId{};
        std::shared_ptr<details::StderrCaptureFile> StderrCapture;
        std::atomic_bool CleanupStarted{};
        std::atomic_bool ForcedCleanup{};
    };

    TaskRuntimeProcess::TaskRuntimeProcess(
        const ProductRuntimeTestIdentity& identity,
        TaskRuntimeProcessOptions options) :
        _impl{ std::make_unique<Impl>(identity, std::move(options)) }
    {
    }

    TaskRuntimeProcess::~TaskRuntimeProcess() = default;

    bool TaskRuntimeProcess::WaitUntilReady(const DWORD timeoutMilliseconds)
    {
        const std::array<HANDLE, 2> waits{ _impl->ReadyEvent.Get(), _impl->Process.Get() };
        const auto waited = WaitForMultipleObjects(
            static_cast<DWORD>(waits.size()),
            waits.data(),
            FALSE,
            timeoutMilliseconds);
        if (waited == WAIT_OBJECT_0 && WaitForSingleObject(_impl->Process.Get(), 0) == WAIT_TIMEOUT)
        {
            return true;
        }

        DWORD exitCode{};
        (void)_impl->WaitForExit(0, exitCode);
        details::LogProcessDiagnostic(
            waited == WAIT_OBJECT_0 + 1 ?
                L"TaskRuntime exited before signaling ready" :
                L"TaskRuntime did not signal ready",
            _impl->ProcessId,
            _impl->StandardError());
        return false;
    }

    bool TaskRuntimeProcess::SignalShutdown() noexcept
    {
        return _impl->ShutdownEvent && SetEvent(_impl->ShutdownEvent.Get());
    }

    bool TaskRuntimeProcess::WaitForExit(
        const DWORD timeoutMilliseconds,
        DWORD& exitCode) noexcept
    {
        return _impl->WaitForExit(timeoutMilliseconds, exitCode);
    }

    bool TaskRuntimeProcess::Running() const noexcept
    {
        return _impl->Process && WaitForSingleObject(_impl->Process.Get(), 0) == WAIT_TIMEOUT;
    }

    DWORD TaskRuntimeProcess::ProcessId() const noexcept
    {
        return _impl->ProcessId;
    }

    std::string TaskRuntimeProcess::StandardError() const
    {
        return _impl->StandardError();
    }

    bool TaskRuntimeProcess::ForcedCleanup() const noexcept
    {
        return _impl->ForcedCleanup.load(std::memory_order_acquire);
    }
}
