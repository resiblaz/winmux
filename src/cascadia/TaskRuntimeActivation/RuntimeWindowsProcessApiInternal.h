// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>
#include <span>

namespace Microsoft::Terminal::TaskRuntime
{
    class IRuntimeProcessLauncher;
}

namespace Microsoft::Terminal::TaskRuntime::details
{
    enum class RuntimeWindowsWaitStatus
    {
        Signaled,
        NotSignaled,
        Failed,
    };

    struct RuntimeWindowsWaitResult
    {
        RuntimeWindowsWaitStatus Status{ RuntimeWindowsWaitStatus::Failed };
        DWORD SystemError{};
    };

    struct RuntimeWindowsCreatedProcess
    {
        HANDLE Process{};
        HANDLE PrimaryThread{};
        DWORD ProcessId{};
    };

    struct RuntimeWindowsProcessCreateRequest
    {
        std::filesystem::path ApplicationPath;
        std::span<wchar_t> MutableCommandLine;
        std::filesystem::path CurrentDirectory;
        DWORD CreationFlags{};
        bool InheritHandles{};
        bool UseStandardHandles{};
        bool UseHandleList{};
    };

    // Internal Win32 seam. Termination is intentionally available only to the
    // suspended pre-commit guard and quarantine implementation.
    class IRuntimeWindowsProcessApi
    {
    public:
        virtual ~IRuntimeWindowsProcessApi() = default;

        [[nodiscard]] virtual bool GetPathAttributes(
            const std::filesystem::path& path,
            DWORD& attributes,
            DWORD& systemError) noexcept = 0;
        [[nodiscard]] virtual bool CurrentProcessInJob(
            bool& inJob,
            DWORD& systemError) noexcept = 0;
        [[nodiscard]] virtual bool CurrentJobLimitFlags(
            DWORD& limitFlags,
            DWORD& systemError) noexcept = 0;
        [[nodiscard]] virtual bool CreateProcess(
            const RuntimeWindowsProcessCreateRequest& request,
            RuntimeWindowsCreatedProcess& process,
            DWORD& systemError) noexcept = 0;
        [[nodiscard]] virtual bool ProcessInAnyJob(
            HANDLE process,
            bool& inJob,
            DWORD& systemError) noexcept = 0;
        [[nodiscard]] virtual bool ResumePrimaryThread(
            HANDLE thread,
            DWORD& previousSuspendCount,
            DWORD& systemError) noexcept = 0;
        [[nodiscard]] virtual bool TerminateProcessBeforeCommit(
            HANDLE process,
            DWORD exitCode,
            DWORD& systemError) noexcept = 0;
        [[nodiscard]] virtual RuntimeWindowsWaitResult PollHandle(HANDLE handle) noexcept = 0;
        [[nodiscard]] virtual RuntimeWindowsWaitResult WaitForProcess(
            HANDLE process,
            DWORD timeoutMilliseconds) noexcept = 0;
        [[nodiscard]] virtual bool ProcessExitCode(
            HANDLE process,
            DWORD& exitCode,
            DWORD& systemError) noexcept = 0;
        virtual void CloseHandle(HANDLE handle) noexcept = 0;
    };

    [[nodiscard]] std::shared_ptr<IRuntimeWindowsProcessApi>
        MakeRuntimeWindowsProcessApi();

    [[nodiscard]] std::shared_ptr<IRuntimeProcessLauncher>
        MakeRuntimeProcessLauncherForTesting(
            std::shared_ptr<IRuntimeWindowsProcessApi> api);
}
