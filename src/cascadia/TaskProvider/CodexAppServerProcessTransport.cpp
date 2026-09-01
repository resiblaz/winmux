// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CodexAppServerTransport.h"

#include <wil/resource.h>

namespace Microsoft::Terminal::TaskProvider
{
    namespace
    {
        constexpr size_t MaximumTransportQueuedStdoutBytes{ 8 * 1024 * 1024 };
        constexpr size_t MaximumTransportStderrBytes{ 64 * 1024 };
        constexpr size_t MaximumTransportWriteBytes{ 1024 * 1024 };

        TaskProviderResult _failure(
            const TaskProviderStatus status,
            std::string error)
        {
            return { status, std::move(error) };
        }

        bool _cancelled(const HANDLE cancellationEvent) noexcept
        {
            return cancellationEvent &&
                   WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0;
        }

        bool _safeExecutable(const std::filesystem::path& path) noexcept
        {
            if (path.empty() || !path.is_absolute() ||
                path.extension() == L".cmd" || path.extension() == L".bat")
            {
                return false;
            }
            const auto attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DEVICE)) == 0;
        }

        std::wstring _quote(const std::wstring_view value)
        {
            std::wstring result{ L"\"" };
            size_t backslashes{};
            for (const auto character : value)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == L'\"')
                {
                    result.append(backslashes * 2 + 1, L'\\');
                    result.push_back(L'\"');
                    backslashes = 0;
                    continue;
                }
                result.append(backslashes, L'\\');
                backslashes = 0;
                result.push_back(character);
            }
            result.append(backslashes * 2, L'\\');
            result.push_back(L'\"');
            return result;
        }

        std::optional<std::vector<wchar_t>> _environment()
        {
            const auto environment = GetEnvironmentStringsW();
            if (!environment)
            {
                return std::nullopt;
            }
            const auto release = wil::scope_exit([&]() noexcept {
                FreeEnvironmentStringsW(environment);
            });
            std::vector<std::wstring> entries;
            for (auto cursor = environment; *cursor; cursor += wcslen(cursor) + 1)
            {
                const std::wstring_view entry{ cursor };
                const auto separator = entry.find(L'=');
                if (separator == std::wstring_view::npos || separator == 0)
                {
                    continue;
                }
                auto name = std::wstring{ entry.substr(0, separator) };
                std::ranges::transform(name, name.begin(), towupper);
                if (name == L"GIT_DIR" || name == L"GIT_WORK_TREE" ||
                    name == L"GIT_INDEX_FILE" || name == L"GIT_OBJECT_DIRECTORY" ||
                    name.starts_with(L"RICHER_TASK_"))
                {
                    continue;
                }
                entries.emplace_back(entry);
            }
            std::ranges::sort(entries, [](const auto& left, const auto& right) {
                return CompareStringOrdinal(
                           left.data(), static_cast<int>(left.size()),
                           right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
            });
            std::vector<wchar_t> block;
            for (const auto& entry : entries)
            {
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back(L'\0');
            }
            block.push_back(L'\0');
            return block;
        }

        class CodexAppServerProcessTransport final : public ICodexAppServerTransport
        {
        public:
            explicit CodexAppServerProcessTransport(std::filesystem::path executable) :
                _executable{ std::move(executable) }
            {
            }

            ~CodexAppServerProcessTransport() override
            {
                Stop();
            }

            TaskProviderResult Start() noexcept override
            {
                try
                {
                    if (_running.load(std::memory_order_acquire))
                    {
                        return _failure(TaskProviderStatus::Conflict, "Codex App Server is already running");
                    }
                    if (!_safeExecutable(_executable))
                    {
                        return _failure(TaskProviderStatus::Unavailable, "A directly executable Codex CLI is unavailable");
                    }
                    SECURITY_ATTRIBUTES attributes{
                        sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE
                    };
                    wil::unique_handle childStdin;
                    wil::unique_handle parentStdin;
                    wil::unique_handle parentStdout;
                    wil::unique_handle childStdout;
                    wil::unique_handle parentStderr;
                    wil::unique_handle childStderr;
                    if (!CreatePipe(childStdin.addressof(), parentStdin.addressof(), &attributes, 0) ||
                        !CreatePipe(parentStdout.addressof(), childStdout.addressof(), &attributes, 0) ||
                        !CreatePipe(parentStderr.addressof(), childStderr.addressof(), &attributes, 0) ||
                        !SetHandleInformation(parentStdin.get(), HANDLE_FLAG_INHERIT, 0) ||
                        !SetHandleInformation(parentStdout.get(), HANDLE_FLAG_INHERIT, 0) ||
                        !SetHandleInformation(parentStderr.get(), HANDLE_FLAG_INHERIT, 0))
                    {
                        return _failure(TaskProviderStatus::ResourceFailure, "Codex App Server pipes could not be created");
                    }

                    STARTUPINFOEXW startup{};
                    startup.StartupInfo.cb = sizeof(startup);
                    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                    startup.StartupInfo.hStdInput = childStdin.get();
                    startup.StartupInfo.hStdOutput = childStdout.get();
                    startup.StartupInfo.hStdError = childStderr.get();
                    SIZE_T listBytes{};
                    InitializeProcThreadAttributeList(nullptr, 1, 0, &listBytes);
                    if (listBytes == 0)
                    {
                        return _failure(TaskProviderStatus::ResourceFailure, "Codex process attribute list is unavailable");
                    }
                    std::vector<std::byte> listStorage(listBytes);
                    startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(listStorage.data());
                    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &listBytes))
                    {
                        return _failure(TaskProviderStatus::ResourceFailure, "Codex process attribute list could not initialize");
                    }
                    const auto deleteList = wil::scope_exit([&]() noexcept {
                        DeleteProcThreadAttributeList(startup.lpAttributeList);
                    });
                    HANDLE inherited[]{ childStdin.get(), childStdout.get(), childStderr.get() };
                    if (!UpdateProcThreadAttribute(
                            startup.lpAttributeList, 0,
                            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                            inherited, sizeof(inherited), nullptr, nullptr))
                    {
                        return _failure(TaskProviderStatus::ResourceFailure, "Codex inherited-handle list could not initialize");
                    }
                    wil::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    if (!job || !SetInformationJobObject(
                                    job.get(), JobObjectExtendedLimitInformation,
                                    &limits, sizeof(limits)))
                    {
                        return _failure(TaskProviderStatus::ResourceFailure, "Codex App Server job could not initialize");
                    }
                    auto environment = _environment();
                    if (!environment)
                    {
                        return _failure(TaskProviderStatus::ResourceFailure, "Codex App Server environment could not initialize");
                    }
                    auto commandLine = _quote(_executable.native()) +
                                       L" app-server --listen stdio://";
                    PROCESS_INFORMATION processInfo{};
                    if (!CreateProcessW(
                            _executable.c_str(), commandLine.data(), nullptr, nullptr,
                            TRUE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW |
                                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                            environment->data(), nullptr,
                            &startup.StartupInfo, &processInfo))
                    {
                        return _failure(TaskProviderStatus::Unavailable, "Codex App Server could not launch");
                    }
                    wil::unique_handle process{ processInfo.hProcess };
                    wil::unique_handle thread{ processInfo.hThread };
                    childStdin.reset();
                    childStdout.reset();
                    childStderr.reset();
                    if (!AssignProcessToJobObject(job.get(), process.get()) ||
                        ResumeThread(thread.get()) == static_cast<DWORD>(-1))
                    {
                        TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
                        return _failure(TaskProviderStatus::ResourceFailure, "Codex App Server containment failed");
                    }
                    wil::unique_handle dataEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
                    if (!dataEvent)
                    {
                        TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
                        return _failure(TaskProviderStatus::ResourceFailure, "Codex App Server data event could not initialize");
                    }
                    _job = std::move(job);
                    _process = std::move(process);
                    _stdin = std::move(parentStdin);
                    _stdout = std::move(parentStdout);
                    _stderr = std::move(parentStderr);
                    _dataEvent = std::move(dataEvent);
                    _terminalKind = CodexTransportReadKind::Data;
                    _stdoutEnded = false;
                    _queuedBytes = 0;
                    _running.store(true, std::memory_order_release);
                    try
                    {
                        _stdoutThread = std::thread{ [this] { _pumpStdout(); } };
                        _stderrThread = std::thread{ [this] { _pumpStderr(); } };
                    }
                    catch (...)
                    {
                        Stop();
                        return _failure(TaskProviderStatus::ResourceFailure, "Codex App Server readers could not start");
                    }
                    return { TaskProviderStatus::Succeeded, {} };
                }
                catch (...)
                {
                    Stop();
                    return _failure(TaskProviderStatus::ResourceFailure, "Codex App Server launch exhausted resources");
                }
            }

            TaskProviderResult Write(
                const std::string_view bytes,
                const std::chrono::milliseconds timeout,
                const HANDLE cancellationEvent) noexcept override
            {
                try
                {
                    if (!_running.load(std::memory_order_acquire) || !_stdin ||
                        bytes.empty() || bytes.size() > MaximumTransportWriteBytes)
                    {
                        return _failure(TaskProviderStatus::InvalidRequest, "Codex request write is invalid");
                    }
                    if (_cancelled(cancellationEvent))
                    {
                        return _failure(TaskProviderStatus::Cancelled, "Codex request was cancelled");
                    }
                    const std::string payload{ bytes };
                    wil::unique_handle completed{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
                    if (!completed)
                    {
                        return _failure(TaskProviderStatus::ResourceFailure, "Codex write event is unavailable");
                    }
                    std::atomic_bool succeeded{};
                    std::thread writer{ [&, event = completed.get()] {
                        const auto signal = wil::scope_exit([&]() noexcept { SetEvent(event); });
                        size_t offset{};
                        while (offset < payload.size())
                        {
                            DWORD written{};
                            const auto count = static_cast<DWORD>((std::min)(
                                payload.size() - offset,
                                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
                            if (!WriteFile(_stdin.get(), payload.data() + offset, count, &written, nullptr) || written == 0)
                            {
                                return;
                            }
                            offset += written;
                        }
                        succeeded.store(true, std::memory_order_release);
                    } };
                    HANDLE handles[]{ completed.get(), cancellationEvent };
                    const auto waited = WaitForMultipleObjects(
                        cancellationEvent ? 2u : 1u, handles, FALSE,
                        static_cast<DWORD>((std::clamp<int64_t>)(timeout.count(), 1, 300000)));
                    if (waited != WAIT_OBJECT_0)
                    {
                        CancelSynchronousIo(writer.native_handle());
                        if (_job)
                        {
                            TerminateJobObject(_job.get(), ERROR_PROCESS_ABORTED);
                        }
                    }
                    writer.join();
                    if (waited == WAIT_OBJECT_0 && succeeded.load(std::memory_order_acquire))
                    {
                        return { TaskProviderStatus::Succeeded, {} };
                    }
                    return _failure(
                        cancellationEvent && waited == WAIT_OBJECT_0 + 1 ?
                            TaskProviderStatus::Cancelled : TaskProviderStatus::Timeout,
                        "Codex request write did not complete");
                }
                catch (...)
                {
                    return _failure(TaskProviderStatus::ResourceFailure, "Codex request write exhausted resources");
                }
            }

            CodexTransportReadResult Read(
                const std::chrono::milliseconds timeout,
                const HANDLE cancellationEvent) noexcept override
            {
                try
                {
                    if (!_dataEvent)
                    {
                        return {};
                    }
                    const auto deadline = GetTickCount64() +
                                          static_cast<uint64_t>((std::max<int64_t>)(timeout.count(), 1));
                    for (;;)
                    {
                        if (_cancelled(cancellationEvent))
                        {
                            return { CodexTransportReadKind::Cancelled };
                        }
                        {
                            const std::scoped_lock lock{ _mutex };
                            if (_terminalKind != CodexTransportReadKind::Data)
                            {
                                return { _terminalKind };
                            }
                            if (!_chunks.empty())
                            {
                                auto value = std::move(_chunks.front());
                                _chunks.pop_front();
                                _queuedBytes -= value.size();
                                if (_chunks.empty() && !_stdoutEnded)
                                {
                                    ResetEvent(_dataEvent.get());
                                }
                                return { CodexTransportReadKind::Data, std::move(value) };
                            }
                            if (_stdoutEnded)
                            {
                                return { CodexTransportReadKind::EndOfStream };
                            }
                        }
                        const auto now = GetTickCount64();
                        if (now >= deadline)
                        {
                            return { CodexTransportReadKind::Timeout };
                        }
                        HANDLE handles[]{ _dataEvent.get(), cancellationEvent };
                        const auto waited = WaitForMultipleObjects(
                            cancellationEvent ? 2u : 1u, handles, FALSE,
                            static_cast<DWORD>((std::min<uint64_t>)(deadline - now, UINT32_MAX)));
                        if (waited == WAIT_OBJECT_0 + 1)
                        {
                            return { CodexTransportReadKind::Cancelled };
                        }
                        if (waited == WAIT_TIMEOUT)
                        {
                            return { CodexTransportReadKind::Timeout };
                        }
                        if (waited != WAIT_OBJECT_0)
                        {
                            return { CodexTransportReadKind::Failed, {}, GetLastError() };
                        }
                    }
                }
                catch (...)
                {
                    return {};
                }
            }

            bool Running() const noexcept override
            {
                return _running.load(std::memory_order_acquire);
            }

            void Stop() noexcept override
            {
                _running.store(false, std::memory_order_release);
                _stdin.reset();
                if (_job)
                {
                    TerminateJobObject(_job.get(), ERROR_PROCESS_ABORTED);
                }
                if (_stdoutThread.joinable())
                {
                    CancelSynchronousIo(_stdoutThread.native_handle());
                    _stdoutThread.join();
                }
                if (_stderrThread.joinable())
                {
                    CancelSynchronousIo(_stderrThread.native_handle());
                    _stderrThread.join();
                }
                _job.reset();
                _process.reset();
                _stdout.reset();
                _stderr.reset();
                _dataEvent.reset();
                const std::scoped_lock lock{ _mutex };
                _chunks.clear();
                _queuedBytes = 0;
                _stdoutEnded = true;
            }

        private:
            void _pumpStdout() noexcept
            {
                std::array<char, 4096> bytes{};
                for (;;)
                {
                    DWORD read{};
                    if (!ReadFile(_stdout.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read == 0)
                    {
                        const std::scoped_lock lock{ _mutex };
                        _stdoutEnded = true;
                        SetEvent(_dataEvent.get());
                        return;
                    }
                    const std::scoped_lock lock{ _mutex };
                    if (_queuedBytes > MaximumTransportQueuedStdoutBytes ||
                        read > MaximumTransportQueuedStdoutBytes - _queuedBytes)
                    {
                        _terminalKind = CodexTransportReadKind::OutputLimitExceeded;
                        SetEvent(_dataEvent.get());
                        return;
                    }
                    _chunks.emplace_back(bytes.data(), read);
                    _queuedBytes += read;
                    SetEvent(_dataEvent.get());
                }
            }

            void _pumpStderr() noexcept
            {
                std::array<char, 4096> bytes{};
                size_t total{};
                for (;;)
                {
                    DWORD read{};
                    if (!ReadFile(_stderr.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read == 0)
                    {
                        return;
                    }
                    total += read;
                    if (total > MaximumTransportStderrBytes)
                    {
                        const std::scoped_lock lock{ _mutex };
                        _terminalKind = CodexTransportReadKind::OutputLimitExceeded;
                        SetEvent(_dataEvent.get());
                        return;
                    }
                }
            }

            std::filesystem::path _executable;
            mutable std::mutex _mutex;
            std::deque<std::string> _chunks;
            size_t _queuedBytes{};
            std::atomic_bool _running{};
            CodexTransportReadKind _terminalKind{ CodexTransportReadKind::Data };
            bool _stdoutEnded{};
            wil::unique_handle _dataEvent;
            wil::unique_handle _job;
            wil::unique_handle _process;
            wil::unique_handle _stdin;
            wil::unique_handle _stdout;
            wil::unique_handle _stderr;
            std::thread _stdoutThread;
            std::thread _stderrThread;
        };
    }

    CodexAppServerTransportFactory MakeCodexAppServerProcessTransportFactory(
        std::filesystem::path executable)
    {
        return [executable = std::move(executable)]() {
            return std::make_unique<CodexAppServerProcessTransport>(executable);
        };
    }
}
