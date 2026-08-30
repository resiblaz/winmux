// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeWindowsProcessApiInternal.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    namespace
    {
        DWORD _errorOrFailure() noexcept
        {
            const auto error = GetLastError();
            return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
        }

        class RuntimeWindowsProcessApi final : public IRuntimeWindowsProcessApi
        {
        public:
            bool GetPathAttributes(
                const std::filesystem::path& path,
                DWORD& attributes,
                DWORD& systemError) noexcept override
            {
                SetLastError(ERROR_SUCCESS);
                attributes = GetFileAttributesW(path.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    systemError = _errorOrFailure();
                    return false;
                }
                systemError = ERROR_SUCCESS;
                return true;
            }

            bool CurrentProcessInJob(bool& inJob, DWORD& systemError) noexcept override
            {
                BOOL value{};
                if (!IsProcessInJob(GetCurrentProcess(), nullptr, &value))
                {
                    systemError = _errorOrFailure();
                    return false;
                }
                inJob = value != FALSE;
                systemError = ERROR_SUCCESS;
                return true;
            }

            bool CurrentJobLimitFlags(DWORD& limitFlags, DWORD& systemError) noexcept override
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                if (!QueryInformationJobObject(
                        nullptr,
                        JobObjectExtendedLimitInformation,
                        &limits,
                        sizeof(limits),
                        nullptr))
                {
                    systemError = _errorOrFailure();
                    return false;
                }
                limitFlags = limits.BasicLimitInformation.LimitFlags;
                systemError = ERROR_SUCCESS;
                return true;
            }

            bool CreateProcess(
                const RuntimeWindowsProcessCreateRequest& request,
                RuntimeWindowsCreatedProcess& process,
                DWORD& systemError) noexcept override
            {
                process = {};
                if (request.ApplicationPath.empty() || request.CurrentDirectory.empty() ||
                    request.MutableCommandLine.empty() ||
                    request.MutableCommandLine.back() != L'\0' ||
                    request.InheritHandles || request.UseStandardHandles || request.UseHandleList)
                {
                    systemError = ERROR_INVALID_PARAMETER;
                    return false;
                }

                STARTUPINFOW startup{};
                startup.cb = sizeof(startup);
                PROCESS_INFORMATION created{};
                if (!::CreateProcessW(
                        request.ApplicationPath.c_str(),
                        request.MutableCommandLine.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        request.CreationFlags,
                        nullptr,
                        request.CurrentDirectory.c_str(),
                        &startup,
                        &created))
                {
                    systemError = _errorOrFailure();
                    return false;
                }
                process.Process = created.hProcess;
                process.PrimaryThread = created.hThread;
                process.ProcessId = created.dwProcessId;
                systemError = ERROR_SUCCESS;
                return true;
            }

            bool ProcessInAnyJob(
                const HANDLE process,
                bool& inJob,
                DWORD& systemError) noexcept override
            {
                BOOL value{};
                if (!IsProcessInJob(process, nullptr, &value))
                {
                    systemError = _errorOrFailure();
                    return false;
                }
                inJob = value != FALSE;
                systemError = ERROR_SUCCESS;
                return true;
            }

            bool ResumePrimaryThread(
                const HANDLE thread,
                DWORD& previousSuspendCount,
                DWORD& systemError) noexcept override
            {
                previousSuspendCount = ResumeThread(thread);
                if (previousSuspendCount == static_cast<DWORD>(-1))
                {
                    systemError = _errorOrFailure();
                    return false;
                }
                systemError = ERROR_SUCCESS;
                return true;
            }

            bool TerminateProcessBeforeCommit(
                const HANDLE process,
                const DWORD exitCode,
                DWORD& systemError) noexcept override
            {
                if (!TerminateProcess(process, exitCode))
                {
                    systemError = _errorOrFailure();
                    return false;
                }
                systemError = ERROR_SUCCESS;
                return true;
            }

            RuntimeWindowsWaitResult PollHandle(const HANDLE handle) noexcept override
            {
                return WaitForProcess(handle, 0);
            }

            RuntimeWindowsWaitResult WaitForProcess(
                const HANDLE process,
                const DWORD timeoutMilliseconds) noexcept override
            {
                const auto waited = WaitForSingleObject(process, timeoutMilliseconds);
                if (waited == WAIT_OBJECT_0)
                {
                    return { RuntimeWindowsWaitStatus::Signaled, ERROR_SUCCESS };
                }
                if (waited == WAIT_TIMEOUT)
                {
                    return { RuntimeWindowsWaitStatus::NotSignaled, ERROR_SUCCESS };
                }
                return {
                    RuntimeWindowsWaitStatus::Failed,
                    waited == WAIT_FAILED ? _errorOrFailure() : ERROR_INVALID_HANDLE,
                };
            }

            bool ProcessExitCode(
                const HANDLE process,
                DWORD& exitCode,
                DWORD& systemError) noexcept override
            {
                if (!GetExitCodeProcess(process, &exitCode))
                {
                    systemError = _errorOrFailure();
                    return false;
                }
                systemError = ERROR_SUCCESS;
                return true;
            }

            void CloseHandle(const HANDLE handle) noexcept override
            {
                if (handle && handle != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(handle);
                }
            }
        };
    }

    std::shared_ptr<IRuntimeWindowsProcessApi> MakeRuntimeWindowsProcessApi()
    {
        try
        {
            return std::make_shared<RuntimeWindowsProcessApi>();
        }
        catch (...)
        {
            return {};
        }
    }
}
