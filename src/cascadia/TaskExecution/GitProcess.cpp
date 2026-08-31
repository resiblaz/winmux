// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitProcessInternal.h"

namespace Microsoft::Terminal::TaskExecution::details
{
    namespace
    {
        constexpr DWORD ProcessCleanupMilliseconds{ 2000 };

        struct HandleOwner final
        {
            HandleOwner() noexcept = default;
            explicit HandleOwner(const HANDLE value) noexcept :
                Value{ value }
            {
            }

            ~HandleOwner()
            {
                Reset();
            }

            HandleOwner(const HandleOwner&) = delete;
            HandleOwner& operator=(const HandleOwner&) = delete;

            void Reset() noexcept
            {
                if (Value && Value != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(Value);
                }
                Value = nullptr;
            }

            HANDLE Value{};
        };

        struct AttributeListOwner final
        {
            ~AttributeListOwner()
            {
                if (Value)
                {
                    DeleteProcThreadAttributeList(Value);
                }
            }

            PPROC_THREAD_ATTRIBUTE_LIST Value{};
        };

        GitCommandResult _failure(
            const GitCommandStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitCommandResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _strictUtf8(const std::string_view value) noexcept
        {
            if (value.empty())
            {
                return true;
            }
            if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return false;
            }
            return MultiByteToWideChar(
                       CP_UTF8,
                       MB_ERR_INVALID_CHARS,
                       value.data(),
                       static_cast<int>(value.size()),
                       nullptr,
                       0) > 0;
        }

        DWORD _terminationCode(const GitCommandStatus status) noexcept
        {
            switch (status)
            {
            case GitCommandStatus::Cancelled:
                return ERROR_OPERATION_ABORTED;
            case GitCommandStatus::TimedOut:
                return ERROR_TIMEOUT;
            case GitCommandStatus::OutputLimitExceeded:
                return ERROR_BUFFER_OVERFLOW;
            default:
                return ERROR_PROCESS_ABORTED;
            }
        }
    }

    GitCommandResult RunGitProcess(
        const GitCommandRequest& request,
        const std::filesystem::path& executable,
        const std::filesystem::path& workingDirectory,
        std::wstring commandLine,
        std::vector<wchar_t> environment) noexcept
    {
        try
        {
            const auto deadline = GetTickCount64() + request.TimeoutMilliseconds;
            SECURITY_ATTRIBUTES inheritable{
                sizeof(SECURITY_ATTRIBUTES),
                nullptr,
                TRUE,
            };
            HandleOwner stdoutRead;
            HandleOwner stdoutWrite;
            HandleOwner stderrRead;
            HandleOwner stderrWrite;
            HandleOwner nullInput{ CreateFileW(
                L"NUL",
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                &inheritable,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr) };
            if (!nullInput.Value || nullInput.Value == INVALID_HANDLE_VALUE ||
                !CreatePipe(
                    &stdoutRead.Value,
                    &stdoutWrite.Value,
                    &inheritable,
                    0) ||
                !CreatePipe(
                    &stderrRead.Value,
                    &stderrWrite.Value,
                    &inheritable,
                    0) ||
                !SetHandleInformation(stdoutRead.Value, HANDLE_FLAG_INHERIT, 0) ||
                !SetHandleInformation(stderrRead.Value, HANDLE_FLAG_INHERIT, 0))
            {
                return _failure(
                    GitCommandStatus::IoFailure,
                    GetLastError(),
                    "Git command pipe setup failed");
            }

            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = nullInput.Value;
            startup.StartupInfo.hStdOutput = stdoutWrite.Value;
            startup.StartupInfo.hStdError = stderrWrite.Value;

            SIZE_T attributeBytes{};
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            if (attributeBytes == 0)
            {
                return _failure(
                    GitCommandStatus::SystemError,
                    GetLastError(),
                    "Git command handle-list size is unavailable");
            }
            std::vector<std::byte> attributeStorage(attributeBytes);
            AttributeListOwner attributeList;
            attributeList.Value = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
                attributeStorage.data());
            startup.lpAttributeList = attributeList.Value;
            if (!InitializeProcThreadAttributeList(
                    attributeList.Value,
                    1,
                    0,
                    &attributeBytes))
            {
                attributeList.Value = nullptr;
                return _failure(
                    GitCommandStatus::SystemError,
                    GetLastError(),
                    "Git command handle list could not be initialized");
            }
            std::array<HANDLE, 3> inherited{
                nullInput.Value,
                stdoutWrite.Value,
                stderrWrite.Value,
            };
            if (!UpdateProcThreadAttribute(
                    attributeList.Value,
                    0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inherited.data(),
                    sizeof(HANDLE) * inherited.size(),
                    nullptr,
                    nullptr))
            {
                return _failure(
                    GitCommandStatus::SystemError,
                    GetLastError(),
                    "Git command inherited-handle list is invalid");
            }

            HandleOwner job{ CreateJobObjectW(nullptr, nullptr) };
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job.Value ||
                !SetInformationJobObject(
                    job.Value,
                    JobObjectExtendedLimitInformation,
                    &limits,
                    sizeof(limits)))
            {
                return _failure(
                    GitCommandStatus::SystemError,
                    GetLastError(),
                    "Git command Job could not be configured");
            }

            PROCESS_INFORMATION processInformation{};
            if (GetTickCount64() >= deadline)
            {
                return _failure(
                    GitCommandStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git command deadline expired during process setup");
            }
            if (!CreateProcessW(
                    executable.c_str(),
                    commandLine.data(),
                    nullptr,
                    nullptr,
                    TRUE,
                    CREATE_SUSPENDED | CREATE_NO_WINDOW |
                        CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                    environment.data(),
                    workingDirectory.c_str(),
                    &startup.StartupInfo,
                    &processInformation))
            {
                return _failure(
                    GitCommandStatus::LaunchFailed,
                    GetLastError(),
                    "Git command process creation failed");
            }
            HandleOwner process{ processInformation.hProcess };
            HandleOwner thread{ processInformation.hThread };
            stdoutWrite.Reset();
            stderrWrite.Reset();
            nullInput.Reset();

            if (!AssignProcessToJobObject(job.Value, process.Value))
            {
                const auto error = GetLastError();
                TerminateProcess(process.Value, ERROR_PROCESS_ABORTED);
                WaitForSingleObject(process.Value, ProcessCleanupMilliseconds);
                return _failure(
                    GitCommandStatus::SystemError,
                    error,
                    "Git command process could not enter its kill-on-close Job");
            }
            if (ResumeThread(thread.Value) == static_cast<DWORD>(-1))
            {
                const auto error = GetLastError();
                TerminateJobObject(job.Value, ERROR_PROCESS_ABORTED);
                WaitForSingleObject(process.Value, ProcessCleanupMilliseconds);
                return _failure(
                    GitCommandStatus::SystemError,
                    error,
                    "Git command primary thread could not resume");
            }

            GitCommandResult result;
            std::atomic_bool outputExceeded{};
            std::atomic_bool ioFailed{};
            std::atomic<DWORD> ioError{};
            const auto pump = [&](
                                  const HANDLE pipe,
                                  std::string& output,
                                  const size_t maximumBytes) noexcept {
                try
                {
                    std::array<char, 4096> buffer{};
                    for (;;)
                    {
                        DWORD bytesRead{};
                        if (!ReadFile(
                                pipe,
                                buffer.data(),
                                static_cast<DWORD>(buffer.size()),
                                &bytesRead,
                                nullptr))
                        {
                            const auto error = GetLastError();
                            if (error != ERROR_BROKEN_PIPE)
                            {
                                ioError.store(error, std::memory_order_release);
                                ioFailed.store(true, std::memory_order_release);
                                TerminateJobObject(job.Value, error);
                            }
                            return;
                        }
                        if (bytesRead == 0)
                        {
                            return;
                        }
                        if (output.size() > maximumBytes ||
                            bytesRead > maximumBytes - output.size())
                        {
                            outputExceeded.store(true, std::memory_order_release);
                            TerminateJobObject(job.Value, ERROR_BUFFER_OVERFLOW);
                            return;
                        }
                        output.append(buffer.data(), bytesRead);
                    }
                }
                catch (...)
                {
                    ioError.store(ERROR_NOT_ENOUGH_MEMORY, std::memory_order_release);
                    ioFailed.store(true, std::memory_order_release);
                    TerminateJobObject(job.Value, ERROR_NOT_ENOUGH_MEMORY);
                }
            };

            std::thread stdoutThread;
            std::thread stderrThread;
            try
            {
                stdoutThread = std::thread{
                    pump,
                    stdoutRead.Value,
                    std::ref(result.Stdout),
                    request.MaximumStdoutBytes
                };
                stderrThread = std::thread{
                    pump,
                    stderrRead.Value,
                    std::ref(result.Stderr),
                    request.MaximumStderrBytes
                };
            }
            catch (...)
            {
                TerminateJobObject(job.Value, ERROR_NOT_ENOUGH_MEMORY);
                WaitForSingleObject(process.Value, ProcessCleanupMilliseconds);
                if (stdoutThread.joinable())
                {
                    CancelSynchronousIo(stdoutThread.native_handle());
                    stdoutThread.join();
                }
                return _failure(
                    GitCommandStatus::ResourceFailure,
                    ERROR_NOT_ENOUGH_MEMORY,
                    "Git command output workers could not start");
            }

            std::array<HANDLE, 2> waits{
                request.CancellationEvent,
                process.Value,
            };
            const char* deferredError{};
            const auto now = GetTickCount64();
            const auto remaining = now < deadline ?
                                       static_cast<DWORD>((std::min<uint64_t>)(
                                           deadline - now,
                                           UINT32_MAX)) :
                                       DWORD{ 0 };
            const auto waited = now >= deadline ?
                                    WAIT_TIMEOUT :
                                    WaitForMultipleObjects(
                                        static_cast<DWORD>(waits.size()),
                                        waits.data(),
                                        FALSE,
                                        remaining);
            if (waited == WAIT_OBJECT_0)
            {
                result.Status = GitCommandStatus::Cancelled;
                result.SystemError = ERROR_OPERATION_ABORTED;
                deferredError = "Git command wait was cancelled";
            }
            else if (waited == WAIT_TIMEOUT)
            {
                result.Status = GitCommandStatus::TimedOut;
                result.SystemError = WAIT_TIMEOUT;
                deferredError = "Git command reached its deadline";
            }
            else if (waited == WAIT_OBJECT_0 + 1)
            {
                result.Status = GitCommandStatus::Completed;
                if (!GetExitCodeProcess(process.Value, &result.ExitCode))
                {
                    result.Status = GitCommandStatus::SystemError;
                    result.SystemError = GetLastError();
                    deferredError = "Git command exit code is unavailable";
                }
                else
                {
                    const auto cancelled = WaitForSingleObject(
                        request.CancellationEvent,
                        0);
                    if (cancelled == WAIT_OBJECT_0)
                    {
                        result.Status = GitCommandStatus::Cancelled;
                        result.SystemError = ERROR_OPERATION_ABORTED;
                        deferredError = "Git command completion raced cancellation";
                    }
                    else if (cancelled != WAIT_TIMEOUT)
                    {
                        result.Status = GitCommandStatus::SystemError;
                        result.SystemError = cancelled == WAIT_FAILED ?
                                                 GetLastError() :
                                                 ERROR_INVALID_HANDLE;
                        deferredError = "Git command cancellation poll failed";
                    }
                }
            }
            else
            {
                result.Status = GitCommandStatus::SystemError;
                result.SystemError = waited == WAIT_FAILED ?
                                         GetLastError() :
                                         ERROR_INVALID_DATA;
                deferredError = "Git command wait failed";
            }

            const auto jobTerminated = TerminateJobObject(
                job.Value,
                _terminationCode(result.Status)) != FALSE;
            const auto terminationError = jobTerminated ? ERROR_SUCCESS : GetLastError();
            const auto processTerminated =
                WaitForSingleObject(process.Value, ProcessCleanupMilliseconds) ==
                WAIT_OBJECT_0;
            const auto cleanupFailed = !jobTerminated || !processTerminated;
            if (!jobTerminated)
            {
                result.Status = GitCommandStatus::SystemError;
                result.SystemError = terminationError;
                deferredError = "Git command process Job could not terminate";
            }
            else if (!processTerminated)
            {
                result.Status = GitCommandStatus::SystemError;
                result.SystemError = WAIT_TIMEOUT;
                deferredError = "Git command process tree did not terminate";
            }
            if (cleanupFailed)
            {
                CancelSynchronousIo(stdoutThread.native_handle());
                CancelSynchronousIo(stderrThread.native_handle());
            }
            stdoutThread.join();
            stderrThread.join();

            if (!cleanupFailed && outputExceeded.load(std::memory_order_acquire))
            {
                result.Status = GitCommandStatus::OutputLimitExceeded;
                result.SystemError = ERROR_BUFFER_OVERFLOW;
                deferredError = "Git command output exceeded its bound";
            }
            else if (!cleanupFailed && ioFailed.load(std::memory_order_acquire))
            {
                result.Status = GitCommandStatus::IoFailure;
                result.SystemError = ioError.load(std::memory_order_acquire);
                deferredError = "Git command output capture failed";
            }
            else if (!cleanupFailed && result.Status == GitCommandStatus::Completed &&
                     (!_strictUtf8(result.Stdout) || !_strictUtf8(result.Stderr)))
            {
                result.Status = GitCommandStatus::InvalidUtf8;
                result.SystemError = ERROR_NO_UNICODE_TRANSLATION;
                deferredError = "Git command output is not strict UTF-8";
            }
            if (deferredError)
            {
                result.Error = deferredError;
            }
            return result;
        }
        catch (...)
        {
            return _failure(
                GitCommandStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git command execution exhausted local resources");
        }
    }
}
