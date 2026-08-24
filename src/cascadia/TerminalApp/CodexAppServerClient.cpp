// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CodexAppServerClient.h"
#include "WorkspaceAgentResume.h"

#include <winrt/Windows.Data.Json.h>

using namespace winrt::Windows::Data::Json;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr std::wstring_view ClientName{ L"windows_terminal" };
        constexpr std::wstring_view ClientTitle{ L"Windows Terminal" };
        constexpr std::wstring_view ClientVersion{ L"1.0.0" };
        constexpr auto GracefulShutdownTimeout{ std::chrono::seconds{ 1 } };
        constexpr size_t MaximumRpcMessageCharacters{ 1024 };
        constexpr int64_t MaximumUnixSeconds{ 253402300799 }; // 9999-12-31T23:59:59Z

        CodexAppServerError _error(const CodexAppServerErrorKind kind,
                                   const std::wstring_view message = {},
                                   const std::wstring_view method = {},
                                   const int64_t rpcCode = 0)
        {
            return {
                .Kind = kind,
                .RpcCode = rpcCode,
                .Method = winrt::hstring{ method },
                .Message = winrt::hstring{ message },
            };
        }

        bool _isCancelled(const HANDLE cancellationEvent) noexcept
        {
            return cancellationEvent && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0;
        }

        bool _isSafeExecutablePath(const std::wstring_view path) noexcept
        {
            return !path.empty() &&
                   path.find_first_of(L"\"%\r\n") == std::wstring_view::npos;
        }

        class Win32CodexAppServerTransport final : public ICodexAppServerTransport
        {
        public:
            ~Win32CodexAppServerTransport() override
            {
                Stop();
            }

            CodexAppServerError Start() noexcept override
            {
                try
                {
                    if (_started)
                    {
                        return _error(CodexAppServerErrorKind::InternalError, L"Transport was already started");
                    }

                    const auto codexPath = WorkspaceAgentResume::ResolveCodexCliPath();
                    const auto commandProcessorPath = WorkspaceAgentResume::ResolveSystemCommandProcessorPath();
                    if (codexPath.empty() || commandProcessorPath.empty() ||
                        !_isSafeExecutablePath(codexPath) || !_isSafeExecutablePath(commandProcessorPath))
                    {
                        return _error(CodexAppServerErrorKind::CliUnavailable, L"Codex CLI is unavailable");
                    }

                    SECURITY_ATTRIBUTES securityAttributes{
                        .nLength = sizeof(SECURITY_ATTRIBUTES),
                        .lpSecurityDescriptor = nullptr,
                        .bInheritHandle = TRUE,
                    };

                    wil::unique_handle childStdin;
                    wil::unique_handle parentStdin;
                    wil::unique_handle parentStdout;
                    wil::unique_handle childStdout;
                    wil::unique_handle parentStderr;
                    wil::unique_handle childStderr;
                    if (!CreatePipe(childStdin.addressof(), parentStdin.addressof(), &securityAttributes, 0) ||
                        !CreatePipe(parentStdout.addressof(), childStdout.addressof(), &securityAttributes, 0) ||
                        !CreatePipe(parentStderr.addressof(), childStderr.addressof(), &securityAttributes, 0) ||
                        !SetHandleInformation(parentStdin.get(), HANDLE_FLAG_INHERIT, 0) ||
                        !SetHandleInformation(parentStdout.get(), HANDLE_FLAG_INHERIT, 0) ||
                        !SetHandleInformation(parentStderr.get(), HANDLE_FLAG_INHERIT, 0))
                    {
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to create App Server pipes");
                    }

                    STARTUPINFOEXW startupInfo{};
                    startupInfo.StartupInfo.cb = sizeof(startupInfo);
                    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                    startupInfo.StartupInfo.hStdInput = childStdin.get();
                    startupInfo.StartupInfo.hStdOutput = childStdout.get();
                    startupInfo.StartupInfo.hStdError = childStderr.get();

                    SIZE_T attributeListSize{};
                    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);
                    if (attributeListSize == 0)
                    {
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to size the process attribute list");
                    }
                    std::vector<std::byte> attributeListStorage(attributeListSize);
                    startupInfo.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeListStorage.data());
                    if (!InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &attributeListSize))
                    {
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to initialize the process attribute list");
                    }
                    const auto deleteAttributeList = wil::scope_exit([&]() noexcept {
                        DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
                    });

                    HANDLE inheritedHandles[]{ childStdin.get(), childStdout.get(), childStderr.get() };
                    if (!UpdateProcThreadAttribute(startupInfo.lpAttributeList,
                                                   0,
                                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                   inheritedHandles,
                                                   sizeof(inheritedHandles),
                                                   nullptr,
                                                   nullptr))
                    {
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to restrict inherited handles");
                    }

                    wil::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
                    if (!job)
                    {
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to create the App Server job");
                    }
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
                    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)))
                    {
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to configure the App Server job");
                    }

                    std::wstring commandLine{ fmt::format(
                        FMT_COMPILE(LR"("{}" /d /v:off /s /c ""{}" app-server --listen stdio://")"),
                        std::wstring_view{ commandProcessorPath },
                        std::wstring_view{ codexPath }) };

                    PROCESS_INFORMATION processInformation{};
                    if (!CreateProcessW(commandProcessorPath.c_str(),
                                        commandLine.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                        nullptr,
                                        nullptr,
                                        &startupInfo.StartupInfo,
                                        &processInformation))
                    {
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to launch Codex App Server");
                    }

                    wil::unique_handle process{ processInformation.hProcess };
                    wil::unique_handle thread{ processInformation.hThread };
                    // The child inherited these exact ends. Close the parent's
                    // duplicates before any reader thread starts so a later
                    // startup failure cannot keep its own pipes artificially
                    // alive while Stop joins the pumps.
                    childStdin.reset();
                    childStdout.reset();
                    childStderr.reset();
                    if (!AssignProcessToJobObject(job.get(), process.get()))
                    {
                        TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to contain Codex App Server in a job");
                    }
                    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
                    {
                        TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to start Codex App Server");
                    }

                    _dataEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                    if (!_dataEvent)
                    {
                        TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to create the App Server data event");
                    }

                    _job = std::move(job);
                    _process = std::move(process);
                    _stdin = std::move(parentStdin);
                    _stdout = std::move(parentStdout);
                    _stderr = std::move(parentStderr);
                    _started = true;

                    try
                    {
                        _stdoutThread = std::thread{ [this]() noexcept { _pumpStdout(); } };
                        _stderrThread = std::thread{ [this]() noexcept { _pumpStderr(); } };
                    }
                    catch (...)
                    {
                        Stop();
                        return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to start App Server pipe readers");
                    }

                    return {};
                }
                catch (...)
                {
                    Stop();
                    return _error(CodexAppServerErrorKind::LaunchFailed, L"Unable to launch Codex App Server");
                }
            }

            CodexAppServerError Write(const std::string_view bytes,
                                      const std::chrono::milliseconds timeout,
                                      const HANDLE cancellationEvent) noexcept override
            {
                try
                {
                    if (!_started || !_stdin || bytes.empty() || bytes.size() > CodexAppServerClient::MaximumRequestBytes)
                    {
                        return _error(CodexAppServerErrorKind::TransportFailed, L"Unable to write the App Server request");
                    }
                    if (_isCancelled(cancellationEvent))
                    {
                        return _error(CodexAppServerErrorKind::Cancelled, L"App Server operation was cancelled");
                    }

                    const std::string payload{ bytes };
                    wil::unique_handle completed{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
                    if (!completed)
                    {
                        return _error(CodexAppServerErrorKind::TransportFailed, L"Unable to create the App Server write event");
                    }

                    std::atomic_bool succeeded{};
                    std::thread writer;
                    try
                    {
                        writer = std::thread{ [this, &payload, &succeeded, event = completed.get()]() noexcept {
                            const auto signal = wil::scope_exit([&]() noexcept { SetEvent(event); });
                            size_t offset{};
                            while (offset < payload.size())
                            {
                                DWORD written{};
                                const auto remaining = std::min(payload.size() - offset,
                                                                static_cast<size_t>(std::numeric_limits<DWORD>::max()));
                                if (!WriteFile(_stdin.get(), payload.data() + offset, gsl::narrow<DWORD>(remaining), &written, nullptr) || written == 0)
                                {
                                    return;
                                }
                                offset += written;
                            }
                            succeeded.store(true, std::memory_order_release);
                        } };
                    }
                    catch (...)
                    {
                        return _error(CodexAppServerErrorKind::TransportFailed, L"Unable to start the App Server writer");
                    }

                    HANDLE handles[2]{ completed.get(), cancellationEvent };
                    const DWORD handleCount = cancellationEvent ? 2u : 1u;
                    const auto maximumWait = static_cast<int64_t>(INFINITE) - 1;
                    const auto waitMilliseconds = gsl::narrow<DWORD>(std::clamp<int64_t>(timeout.count(), 1, maximumWait));
                    const auto wait = WaitForMultipleObjects(handleCount, handles, FALSE, waitMilliseconds);
                    if (wait != WAIT_OBJECT_0)
                    {
                        // A synchronous anonymous-pipe WriteFile has no native
                        // deadline. Killing this operation's private Job closes
                        // every child read end, which releases the writer even
                        // if the command trampoline stopped consuming stdin.
                        if (_job)
                        {
                            TerminateJobObject(_job.get(), ERROR_PROCESS_ABORTED);
                        }
                        CancelSynchronousIo(writer.native_handle());
                    }
                    writer.join();

                    if (wait == WAIT_OBJECT_0 && succeeded.load(std::memory_order_acquire))
                    {
                        return {};
                    }
                    if (cancellationEvent && wait == WAIT_OBJECT_0 + 1)
                    {
                        return _error(CodexAppServerErrorKind::Cancelled, L"App Server operation was cancelled");
                    }
                    if (wait == WAIT_TIMEOUT)
                    {
                        return _error(CodexAppServerErrorKind::Timeout, L"App Server request write timed out");
                    }
                    return _error(CodexAppServerErrorKind::TransportFailed, L"Unable to write the App Server request");
                }
                catch (...)
                {
                    return _error(CodexAppServerErrorKind::TransportFailed, L"Unable to write the App Server request");
                }
            }

            CodexAppServerTransportReadResult Read(const std::chrono::milliseconds timeout,
                                                    const HANDLE cancellationEvent) noexcept override
            {
                try
                {
                    if (!_started || !_dataEvent)
                    {
                        return { CodexAppServerTransportReadKind::Failed };
                    }

                    const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds{ 1 });
                    for (;;)
                    {
                        if (_isCancelled(cancellationEvent))
                        {
                            return { CodexAppServerTransportReadKind::Cancelled };
                        }

                        {
                            const std::scoped_lock lock{ _mutex };
                            if (_terminalReadKind != CodexAppServerTransportReadKind::Data)
                            {
                                return { _terminalReadKind };
                            }
                            if (!_stdoutChunks.empty())
                            {
                                auto bytes = std::move(_stdoutChunks.front());
                                _stdoutChunks.pop_front();
                                if (_stdoutChunks.empty() && !_stdoutEnded && _terminalReadKind == CodexAppServerTransportReadKind::Data)
                                {
                                    ResetEvent(_dataEvent.get());
                                }
                                return { CodexAppServerTransportReadKind::Data, std::move(bytes) };
                            }
                            if (_stdoutEnded)
                            {
                                return { CodexAppServerTransportReadKind::EndOfStream };
                            }
                        }

                        const auto now = std::chrono::steady_clock::now();
                        if (now >= deadline)
                        {
                            return { CodexAppServerTransportReadKind::Timeout };
                        }
                        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                        HANDLE handles[2]{ _dataEvent.get(), cancellationEvent };
                        const DWORD count = cancellationEvent ? 2u : 1u;
                        const auto wait = WaitForMultipleObjects(count,
                                                                 handles,
                                                                 FALSE,
                                                                 gsl::narrow<DWORD>(std::max<int64_t>(remaining.count(), 1)));
                        if (wait == WAIT_OBJECT_0 + 1)
                        {
                            return { CodexAppServerTransportReadKind::Cancelled };
                        }
                        if (wait == WAIT_TIMEOUT)
                        {
                            return { CodexAppServerTransportReadKind::Timeout };
                        }
                        if (wait != WAIT_OBJECT_0)
                        {
                            return { CodexAppServerTransportReadKind::Failed };
                        }
                    }
                }
                catch (...)
                {
                    return { CodexAppServerTransportReadKind::Failed };
                }
            }

            void Stop() noexcept override
            {
                if (!_started && !_stdoutThread.joinable() && !_stderrThread.joinable())
                {
                    return;
                }

                _started = false;
                _stdin.reset();
                if (_process)
                {
                    const auto wait = WaitForSingleObject(_process.get(), gsl::narrow<DWORD>(GracefulShutdownTimeout.count() * 1000));
                    if (wait != WAIT_OBJECT_0 && _job)
                    {
                        TerminateJobObject(_job.get(), ERROR_PROCESS_ABORTED);
                        WaitForSingleObject(_process.get(), 1000);
                    }
                }

                // Closing a kill-on-close job before joining the readers also
                // covers a command trampoline that exits before a descendant.
                // No descendant may keep one of the pipe write ends alive while
                // this object waits for its pump threads.
                _job.reset();

                if (_stdoutThread.joinable())
                {
                    CancelSynchronousIo(_stdoutThread.native_handle());
                }
                if (_stderrThread.joinable())
                {
                    CancelSynchronousIo(_stderrThread.native_handle());
                }
                if (_stdoutThread.joinable())
                {
                    _stdoutThread.join();
                }
                if (_stderrThread.joinable())
                {
                    _stderrThread.join();
                }

                _stdout.reset();
                _stderr.reset();
                _process.reset();
                _dataEvent.reset();
                {
                    const std::scoped_lock lock{ _mutex };
                    _stdoutChunks.clear();
                    _stdoutEnded = true;
                }
            }

        private:
            bool _recordBytes(const size_t byteCount) noexcept
            {
                const auto previous = _totalBytes.fetch_add(byteCount, std::memory_order_relaxed);
                if (previous > CodexAppServerClient::MaximumTotalOutputBytes ||
                    byteCount > CodexAppServerClient::MaximumTotalOutputBytes - previous)
                {
                    const std::scoped_lock lock{ _mutex };
                    _terminalReadKind = CodexAppServerTransportReadKind::OutputLimitExceeded;
                    SetEvent(_dataEvent.get());
                    return false;
                }
                return true;
            }

            void _pumpStdout() noexcept
            {
                std::array<char, 4096> buffer{};
                for (;;)
                {
                    DWORD bytesRead{};
                    const auto read = ReadFile(_stdout.get(), buffer.data(), gsl::narrow<DWORD>(buffer.size()), &bytesRead, nullptr);
                    if (!read || bytesRead == 0)
                    {
                        const auto error = read ? ERROR_SUCCESS : GetLastError();
                        const std::scoped_lock lock{ _mutex };
                        if (error != ERROR_BROKEN_PIPE && error != ERROR_OPERATION_ABORTED && error != ERROR_SUCCESS)
                        {
                            _terminalReadKind = CodexAppServerTransportReadKind::Failed;
                        }
                        _stdoutEnded = true;
                        SetEvent(_dataEvent.get());
                        return;
                    }
                    if (!_recordBytes(bytesRead))
                    {
                        return;
                    }
                    {
                        const std::scoped_lock lock{ _mutex };
                        _stdoutChunks.emplace_back(buffer.data(), bytesRead);
                        SetEvent(_dataEvent.get());
                    }
                }
            }

            void _pumpStderr() noexcept
            {
                std::array<char, 4096> buffer{};
                size_t stderrBytes{};
                for (;;)
                {
                    DWORD bytesRead{};
                    if (!ReadFile(_stderr.get(), buffer.data(), gsl::narrow<DWORD>(buffer.size()), &bytesRead, nullptr) || bytesRead == 0)
                    {
                        return;
                    }
                    stderrBytes += bytesRead;
                    if (stderrBytes > CodexAppServerClient::MaximumStderrBytes || !_recordBytes(bytesRead))
                    {
                        const std::scoped_lock lock{ _mutex };
                        _terminalReadKind = CodexAppServerTransportReadKind::OutputLimitExceeded;
                        SetEvent(_dataEvent.get());
                        return;
                    }
                }
            }

            std::mutex _mutex;
            std::deque<std::string> _stdoutChunks;
            std::atomic<size_t> _totalBytes{};
            CodexAppServerTransportReadKind _terminalReadKind{ CodexAppServerTransportReadKind::Data };
            bool _stdoutEnded{ false };
            bool _started{ false };
            wil::unique_handle _dataEvent;
            wil::unique_handle _job;
            wil::unique_handle _process;
            wil::unique_handle _stdin;
            wil::unique_handle _stdout;
            wil::unique_handle _stderr;
            std::thread _stdoutThread;
            std::thread _stderrThread;
        };

        bool _isType(const JsonObject& object, const std::wstring_view key, const JsonValueType type)
        {
            return object.HasKey(key) && object.GetNamedValue(key).ValueType() == type;
        }

        bool _isNullOrType(const JsonObject& object, const std::wstring_view key, const JsonValueType type)
        {
            if (!object.HasKey(key))
            {
                return false;
            }
            const auto actual = object.GetNamedValue(key).ValueType();
            return actual == JsonValueType::Null || actual == type;
        }

        bool _hasControlCharacter(const std::wstring_view value) noexcept
        {
            return std::ranges::any_of(value, [](const wchar_t character) {
                return character < L' ' || character == 0x7f;
            });
        }

        bool _validIdentifier(const std::wstring_view value) noexcept
        {
            return !value.empty() &&
                   value.size() <= CodexAppServerClient::MaximumIdentifierCharacters &&
                   !_hasControlCharacter(value);
        }

        std::optional<int64_t> _integer(const JsonObject& object,
                                        const std::wstring_view key,
                                        const int64_t minimum,
                                        const int64_t maximum)
        {
            if (!_isType(object, key, JsonValueType::Number))
            {
                return std::nullopt;
            }
            const auto value = object.GetNamedNumber(key);
            if (!std::isfinite(value) || std::floor(value) != value ||
                value < static_cast<double>(minimum) || value > static_cast<double>(maximum))
            {
                return std::nullopt;
            }
            return static_cast<int64_t>(value);
        }

        bool _boundedString(const JsonObject& object,
                            const std::wstring_view key,
                            const size_t maximum,
                            winrt::hstring& value,
                            const bool allowEmpty = true)
        {
            if (!_isType(object, key, JsonValueType::String))
            {
                return false;
            }
            value = object.GetNamedString(key);
            return value.size() <= maximum && (allowEmpty || !value.empty());
        }

        bool _optionalBoundedString(const JsonObject& object,
                                    const std::wstring_view key,
                                    const size_t maximum,
                                    winrt::hstring& value)
        {
            value.clear();
            if (!object.HasKey(key) || object.GetNamedValue(key).ValueType() == JsonValueType::Null)
            {
                return true;
            }
            return _boundedString(object, key, maximum, value);
        }

        bool _validThreadStatus(const JsonObject& thread)
        {
            if (!_isType(thread, L"status", JsonValueType::Object))
            {
                return false;
            }
            const auto status = thread.GetNamedObject(L"status");
            if (!_isType(status, L"type", JsonValueType::String))
            {
                return false;
            }
            const auto type = status.GetNamedString(L"type");
            return type == L"notLoaded" || type == L"idle" || type == L"systemError" || type == L"active";
        }

        struct RpcResult
        {
            CodexAppServerError Error;
            JsonObject Result{ nullptr };
        };

        class ProtocolSession final
        {
        public:
            explicit ProtocolSession(std::unique_ptr<ICodexAppServerTransport> transport) :
                _transport{ std::move(transport) }
            {
            }

            ~ProtocolSession()
            {
                if (_transport)
                {
                    _transport->Stop();
                }
            }

            CodexAppServerError Initialize(const std::chrono::milliseconds timeout, const HANDLE cancellationEvent)
            {
                if (!_transport)
                {
                    return _error(CodexAppServerErrorKind::InternalError, L"App Server transport factory returned null");
                }
                if (_isCancelled(cancellationEvent))
                {
                    return _error(CodexAppServerErrorKind::Cancelled, L"App Server operation was cancelled", L"initialize");
                }
                if (auto error = _transport->Start())
                {
                    return error;
                }

                const auto initializeDeadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds{ 1 });

                JsonObject clientInfo;
                clientInfo.Insert(L"name", JsonValue::CreateStringValue(ClientName));
                clientInfo.Insert(L"title", JsonValue::CreateStringValue(ClientTitle));
                clientInfo.Insert(L"version", JsonValue::CreateStringValue(ClientVersion));

                JsonObject params;
                params.Insert(L"clientInfo", clientInfo);
                auto response = Request(L"initialize", params, timeout, cancellationEvent);
                if (response.Error)
                {
                    return response.Error;
                }
                if (!_validInitializeResponse(response.Result))
                {
                    return _error(CodexAppServerErrorKind::MalformedResponse,
                                  L"App Server returned an invalid initialize result",
                                  L"initialize");
                }

                JsonObject initialized;
                initialized.Insert(L"method", JsonValue::CreateStringValue(L"initialized"));
                initialized.Insert(L"params", JsonObject{});
                const auto now = std::chrono::steady_clock::now();
                if (now >= initializeDeadline)
                {
                    return _error(CodexAppServerErrorKind::Timeout, L"App Server initialization timed out", L"initialized");
                }
                return _send(initialized,
                             L"initialized",
                             std::chrono::duration_cast<std::chrono::milliseconds>(initializeDeadline - now),
                             cancellationEvent);
            }

            RpcResult Request(const std::wstring_view method,
                              const JsonObject& params,
                              const std::chrono::milliseconds timeout,
                              const HANDLE cancellationEvent)
            {
                try
                {
                    if (_isCancelled(cancellationEvent))
                    {
                        return { _error(CodexAppServerErrorKind::Cancelled, L"App Server operation was cancelled", method) };
                    }

                    const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds{ 1 });
                    const auto id = _nextRequestId++;
                    JsonObject request;
                    request.Insert(L"method", JsonValue::CreateStringValue(method));
                    request.Insert(L"id", JsonValue::CreateNumberValue(id));
                    request.Insert(L"params", params);
                    const auto sendNow = std::chrono::steady_clock::now();
                    if (sendNow >= deadline)
                    {
                        return { _error(CodexAppServerErrorKind::Timeout, L"App Server request timed out", method) };
                    }
                    if (auto error = _send(request,
                                           method,
                                           std::chrono::duration_cast<std::chrono::milliseconds>(deadline - sendNow),
                                           cancellationEvent))
                    {
                        return { std::move(error) };
                    }

                    size_t notifications{};
                    for (;;)
                    {
                        const auto now = std::chrono::steady_clock::now();
                        if (now >= deadline)
                        {
                            return { _error(CodexAppServerErrorKind::Timeout, L"App Server request timed out", method) };
                        }

                        std::string line;
                        if (auto error = _readLine(line,
                                                   std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                                                   cancellationEvent,
                                                   method))
                        {
                            return { std::move(error) };
                        }

                        JsonObject root{ nullptr };
                        try
                        {
                            if (!JsonObject::TryParse(winrt::to_hstring(line), root) || !root)
                            {
                                return { _error(CodexAppServerErrorKind::MalformedResponse,
                                                L"App Server emitted invalid JSON",
                                                method) };
                            }
                        }
                        catch (...)
                        {
                            return { _error(CodexAppServerErrorKind::MalformedResponse,
                                            L"App Server emitted invalid UTF-8 JSON",
                                            method) };
                        }

                        if (!root.HasKey(L"id"))
                        {
                            if (!_isType(root, L"method", JsonValueType::String) || ++notifications > CodexAppServerClient::MaximumNotificationsPerRequest)
                            {
                                return { _error(CodexAppServerErrorKind::UnexpectedResponse,
                                                L"App Server emitted too many or invalid notifications",
                                                method) };
                            }
                            continue;
                        }

                        const auto responseId = _integer(root, L"id", 0, std::numeric_limits<int32_t>::max());
                        if (!responseId || responseId.value() != static_cast<int64_t>(id))
                        {
                            return { _error(CodexAppServerErrorKind::UnexpectedResponse,
                                            L"App Server response ID did not match the request",
                                            method) };
                        }

                        const auto hasResult = _isType(root, L"result", JsonValueType::Object);
                        const auto hasError = _isType(root, L"error", JsonValueType::Object);
                        if (hasResult == hasError)
                        {
                            return { _error(CodexAppServerErrorKind::MalformedResponse,
                                            L"App Server response must contain exactly one result or error",
                                            method) };
                        }
                        if (hasResult)
                        {
                            return { {}, root.GetNamedObject(L"result") };
                        }

                        const auto errorObject = root.GetNamedObject(L"error");
                        const auto code = _integer(errorObject,
                                                   L"code",
                                                   std::numeric_limits<int32_t>::min(),
                                                   std::numeric_limits<int32_t>::max());
                        winrt::hstring message;
                        if (!code || !_boundedString(errorObject, L"message", MaximumRpcMessageCharacters, message))
                        {
                            return { _error(CodexAppServerErrorKind::MalformedResponse,
                                            L"App Server returned an invalid RPC error",
                                            method) };
                        }
                        return { _error(code.value() == -32601 ? CodexAppServerErrorKind::UnsupportedMethod : CodexAppServerErrorKind::RpcError,
                                        std::wstring_view{ message },
                                        method,
                                        code.value()) };
                    }
                }
                catch (...)
                {
                    return { _error(CodexAppServerErrorKind::InternalError, L"App Server request failed", method) };
                }
            }

        private:
            static bool _validInitializeResponse(const JsonObject& result)
            {
                winrt::hstring codexHome;
                winrt::hstring platformFamily;
                winrt::hstring platformOs;
                winrt::hstring userAgent;
                return _boundedString(result, L"codexHome", CodexAppServerClient::MaximumCwdCharacters, codexHome) &&
                       _boundedString(result, L"platformFamily", 64, platformFamily, false) &&
                       _boundedString(result, L"platformOs", 64, platformOs, false) &&
                       _boundedString(result, L"userAgent", 1024, userAgent, false);
            }

            CodexAppServerError _send(const JsonObject& object,
                                      const std::wstring_view method,
                                      const std::chrono::milliseconds timeout,
                                      const HANDLE cancellationEvent)
            {
                try
                {
                    auto bytes = winrt::to_string(object.Stringify());
                    if (bytes.empty() || bytes.size() + 1 > CodexAppServerClient::MaximumRequestBytes)
                    {
                        return _error(CodexAppServerErrorKind::OutputLimitExceeded,
                                      L"App Server request exceeded its size limit",
                                      method);
                    }
                    bytes.push_back('\n');
                    auto error = _transport->Write(bytes, timeout, cancellationEvent);
                    if (error && error.Method.empty())
                    {
                        error.Method = winrt::hstring{ method };
                    }
                    return error;
                }
                catch (...)
                {
                    return _error(CodexAppServerErrorKind::TransportFailed,
                                  L"Unable to encode the App Server request",
                                  method);
                }
            }

            CodexAppServerError _readLine(std::string& line,
                                          const std::chrono::milliseconds timeout,
                                          const HANDLE cancellationEvent,
                                          const std::wstring_view method)
            {
                if (!_pendingLines.empty())
                {
                    line = std::move(_pendingLines.front());
                    _pendingLines.pop_front();
                    return {};
                }

                const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds{ 1 });
                for (;;)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline)
                    {
                        return _error(CodexAppServerErrorKind::Timeout, L"App Server request timed out", method);
                    }
                    auto read = _transport->Read(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), cancellationEvent);
                    switch (read.Kind)
                    {
                    case CodexAppServerTransportReadKind::Data:
                        if (read.Bytes.empty() ||
                            _totalBytesRead > CodexAppServerClient::MaximumTotalOutputBytes ||
                            read.Bytes.size() > CodexAppServerClient::MaximumTotalOutputBytes - _totalBytesRead)
                        {
                            return _error(CodexAppServerErrorKind::OutputLimitExceeded,
                                          L"App Server exceeded its total output limit",
                                          method);
                        }
                        _totalBytesRead += read.Bytes.size();
                        _receiveBuffer.append(read.Bytes);
                        _splitLines();
                        if (_receiveBuffer.size() > CodexAppServerClient::MaximumLineBytes)
                        {
                            return _error(CodexAppServerErrorKind::OutputLimitExceeded,
                                          L"App Server emitted an oversized JSON line",
                                          method);
                        }
                        if (!_pendingLines.empty())
                        {
                            line = std::move(_pendingLines.front());
                            _pendingLines.pop_front();
                            return {};
                        }
                        break;
                    case CodexAppServerTransportReadKind::EndOfStream:
                        return _error(_receiveBuffer.empty() ? CodexAppServerErrorKind::TransportClosed : CodexAppServerErrorKind::MalformedResponse,
                                      _receiveBuffer.empty() ? L"App Server closed stdout" : L"App Server ended with an incomplete JSON line",
                                      method);
                    case CodexAppServerTransportReadKind::Timeout:
                        return _error(CodexAppServerErrorKind::Timeout, L"App Server request timed out", method);
                    case CodexAppServerTransportReadKind::Cancelled:
                        return _error(CodexAppServerErrorKind::Cancelled, L"App Server operation was cancelled", method);
                    case CodexAppServerTransportReadKind::OutputLimitExceeded:
                        return _error(CodexAppServerErrorKind::OutputLimitExceeded, L"App Server exceeded its output limit", method);
                    case CodexAppServerTransportReadKind::Failed:
                    default:
                        return _error(CodexAppServerErrorKind::TransportFailed, L"Unable to read App Server stdout", method);
                    }
                }
            }

            void _splitLines()
            {
                size_t consumed{};
                for (;;)
                {
                    const auto newline = _receiveBuffer.find('\n', consumed);
                    if (newline == std::string::npos)
                    {
                        break;
                    }
                    auto length = newline - consumed;
                    if (length > 0 && _receiveBuffer[consumed + length - 1] == '\r')
                    {
                        --length;
                    }
                    if (length > CodexAppServerClient::MaximumLineBytes)
                    {
                        _receiveBuffer.assign(CodexAppServerClient::MaximumLineBytes + 1, 'x');
                        _pendingLines.clear();
                        return;
                    }
                    _pendingLines.emplace_back(_receiveBuffer.substr(consumed, length));
                    consumed = newline + 1;
                }
                if (consumed > 0)
                {
                    _receiveBuffer.erase(0, consumed);
                }
            }

            std::unique_ptr<ICodexAppServerTransport> _transport;
            uint32_t _nextRequestId{ 1 };
            size_t _totalBytesRead{};
            std::string _receiveBuffer;
            std::deque<std::string> _pendingLines;
        };

        bool _optionalUnixSeconds(const JsonObject& object,
                                  const std::wstring_view key,
                                  std::optional<int64_t>& value)
        {
            value.reset();
            if (!object.HasKey(key) || object.GetNamedValue(key).ValueType() == JsonValueType::Null)
            {
                return true;
            }
            value = _integer(object, key, 0, MaximumUnixSeconds);
            return value.has_value();
        }

        CodexAppServerError _parseThreadSummary(const JsonObject& thread,
                                                CodexAppServerThreadSummary& summary,
                                                const std::wstring_view method)
        {
            try
            {
                winrt::hstring sessionId;
                winrt::hstring cliVersion;
                winrt::hstring projectId;
                if (!_boundedString(thread, L"id", CodexAppServerClient::MaximumIdentifierCharacters, summary.Id, false) ||
                    !_validIdentifier(summary.Id) ||
                    !_boundedString(thread, L"sessionId", CodexAppServerClient::MaximumIdentifierCharacters, sessionId, false) ||
                    !_validIdentifier(sessionId) ||
                    !_boundedString(thread, L"cliVersion", 128, cliVersion, false) ||
                    !_boundedString(thread, L"cwd", CodexAppServerClient::MaximumCwdCharacters, summary.Cwd) ||
                    _hasControlCharacter(summary.Cwd) ||
                    !_boundedString(thread, L"preview", CodexAppServerClient::MaximumSummaryCharacters, summary.Preview) ||
                    !_boundedString(thread, L"modelProvider", 128, summary.ModelProvider, false) ||
                    !_boundedString(thread, L"source", 64, summary.SourceKind, false) ||
                    !_optionalBoundedString(thread, L"name", CodexAppServerClient::MaximumSummaryCharacters, summary.Name) ||
                    !_isType(thread, L"ephemeral", JsonValueType::Boolean) ||
                    !_isNullOrType(thread, L"projectId", JsonValueType::String) ||
                    !_validThreadStatus(thread) ||
                    !_isType(thread, L"turns", JsonValueType::Array))
                {
                    return _error(CodexAppServerErrorKind::MalformedResponse,
                                  L"App Server returned an invalid Thread",
                                  method);
                }
                if (thread.GetNamedValue(L"projectId").ValueType() == JsonValueType::String &&
                    !_boundedString(thread, L"projectId", CodexAppServerClient::MaximumIdentifierCharacters, projectId))
                {
                    return _error(CodexAppServerErrorKind::MalformedResponse,
                                  L"App Server returned an invalid Thread project ID",
                                  method);
                }

                const auto createdAt = _integer(thread, L"createdAt", 0, MaximumUnixSeconds);
                const auto updatedAt = _integer(thread, L"updatedAt", 0, MaximumUnixSeconds);
                const auto turns = thread.GetNamedArray(L"turns");
                if (!createdAt || !updatedAt || turns.Size() > CodexAppServerClient::MaximumTurns)
                {
                    return _error(turns.Size() > CodexAppServerClient::MaximumTurns ?
                                      CodexAppServerErrorKind::OutputLimitExceeded :
                                      CodexAppServerErrorKind::MalformedResponse,
                                  L"App Server returned invalid or excessive Thread metadata",
                                  method);
                }
                summary.CreatedAtUnixSeconds = createdAt.value();
                summary.UpdatedAtUnixSeconds = updatedAt.value();
                return {};
            }
            catch (...)
            {
                return _error(CodexAppServerErrorKind::MalformedResponse,
                              L"App Server returned an invalid Thread",
                              method);
            }
        }

        bool _appendText(winrt::hstring& destination, const winrt::hstring& text)
        {
            const auto separator = destination.empty() || text.empty() ? 0u : 1u;
            if (destination.size() > CodexAppServerClient::MaximumTurnTextCharacters ||
                text.size() > CodexAppServerClient::MaximumTurnTextCharacters - destination.size() ||
                separator > CodexAppServerClient::MaximumTurnTextCharacters - destination.size() - text.size())
            {
                return false;
            }
            if (separator)
            {
                destination = winrt::hstring{ fmt::format(FMT_COMPILE(L"{}\n{}"), std::wstring_view{ destination }, std::wstring_view{ text }) };
            }
            else if (!text.empty())
            {
                destination = winrt::hstring{ std::wstring_view{ text } };
            }
            return true;
        }

        CodexAppServerError _parseTurn(const JsonObject& turn,
                                       const int64_t fallbackUpdatedAtSeconds,
                                       CodexAppServerTurn& parsed,
                                       size_t& itemCount)
        {
            try
            {
                winrt::hstring status;
                if (!_boundedString(turn, L"id", CodexAppServerClient::MaximumIdentifierCharacters, parsed.TurnId, false) ||
                    !_validIdentifier(parsed.TurnId) ||
                    !_boundedString(turn, L"status", 64, status, false) ||
                    !_isType(turn, L"items", JsonValueType::Array) ||
                    !_optionalBoundedString(turn, L"model", 256, parsed.Model))
                {
                    return _error(CodexAppServerErrorKind::MalformedResponse,
                                  L"App Server returned an invalid Turn",
                                  L"thread/read");
                }

                std::optional<int64_t> startedAt;
                std::optional<int64_t> completedAt;
                if (!_optionalUnixSeconds(turn, L"startedAt", startedAt) ||
                    !_optionalUnixSeconds(turn, L"completedAt", completedAt))
                {
                    return _error(CodexAppServerErrorKind::MalformedResponse,
                                  L"App Server returned an invalid Turn timestamp",
                                  L"thread/read");
                }

                const auto items = turn.GetNamedArray(L"items");
                if (items.Size() > CodexAppServerClient::MaximumItems ||
                    itemCount > CodexAppServerClient::MaximumItems - items.Size())
                {
                    return _error(CodexAppServerErrorKind::OutputLimitExceeded,
                                  L"App Server returned too many Turn items",
                                  L"thread/read");
                }
                itemCount += items.Size();

                winrt::hstring fallbackFinal;
                bool hasExplicitFinal{ false };
                for (const auto& value : items)
                {
                    if (value.ValueType() != JsonValueType::Object)
                    {
                        return _error(CodexAppServerErrorKind::MalformedResponse,
                                      L"App Server returned a non-object Turn item",
                                      L"thread/read");
                    }
                    const auto item = value.GetObject();
                    winrt::hstring type;
                    if (!_boundedString(item, L"type", 64, type, false))
                    {
                        return _error(CodexAppServerErrorKind::MalformedResponse,
                                      L"App Server returned a Turn item without a valid type",
                                      L"thread/read");
                    }

                    if (type == L"userMessage")
                    {
                        if (!_isType(item, L"content", JsonValueType::Array))
                        {
                            return _error(CodexAppServerErrorKind::MalformedResponse,
                                          L"App Server returned an invalid user message",
                                          L"thread/read");
                        }
                        for (const auto& contentValue : item.GetNamedArray(L"content"))
                        {
                            if (contentValue.ValueType() != JsonValueType::Object)
                            {
                                return _error(CodexAppServerErrorKind::MalformedResponse,
                                              L"App Server returned invalid user content",
                                              L"thread/read");
                            }
                            const auto content = contentValue.GetObject();
                            winrt::hstring contentType;
                            if (!_boundedString(content, L"type", 64, contentType, false))
                            {
                                return _error(CodexAppServerErrorKind::MalformedResponse,
                                              L"App Server returned user content without a valid type",
                                              L"thread/read");
                            }
                            if (contentType == L"text")
                            {
                                winrt::hstring text;
                                if (!_boundedString(content, L"text", CodexAppServerClient::MaximumTurnTextCharacters, text) ||
                                    !_appendText(parsed.Prompt, text))
                                {
                                    return _error(CodexAppServerErrorKind::OutputLimitExceeded,
                                                  L"App Server user message exceeded its size limit",
                                                  L"thread/read");
                                }
                            }
                        }
                    }
                    else if (type == L"agentMessage")
                    {
                        winrt::hstring text;
                        winrt::hstring phase;
                        if (!_boundedString(item, L"text", CodexAppServerClient::MaximumTurnTextCharacters, text) ||
                            !_optionalBoundedString(item, L"phase", 64, phase))
                        {
                            return _error(CodexAppServerErrorKind::MalformedResponse,
                                          L"App Server returned an invalid agent message",
                                          L"thread/read");
                        }
                        if (phase == L"final_answer")
                        {
                            parsed.FinalMarkdown = text;
                            hasExplicitFinal = true;
                        }
                        else if (phase.empty())
                        {
                            fallbackFinal = text;
                        }
                    }
                    // All other official or future item types are intentionally
                    // ignored. In particular, tool calls, reasoning, commands,
                    // diffs, and their output never enter this bounded model.
                }
                if (!hasExplicitFinal)
                {
                    parsed.FinalMarkdown = fallbackFinal;
                }

                const auto eventSeconds = completedAt.value_or(startedAt.value_or(fallbackUpdatedAtSeconds));
                if (eventSeconds < 0 || eventSeconds > MaximumUnixSeconds)
                {
                    return _error(CodexAppServerErrorKind::MalformedResponse,
                                  L"App Server returned an invalid Turn timestamp",
                                  L"thread/read");
                }
                parsed.EventAtUnixMilliseconds = eventSeconds * 1000;
                return {};
            }
            catch (...)
            {
                return _error(CodexAppServerErrorKind::MalformedResponse,
                              L"App Server returned an invalid Turn",
                              L"thread/read");
            }
        }

        CodexAppServerError _parseThread(const JsonObject& thread,
                                         const bool includeTurns,
                                         CodexAppServerThread& parsed)
        {
            if (auto error = _parseThreadSummary(thread, parsed.Summary, L"thread/read"))
            {
                return error;
            }
            if (!includeTurns)
            {
                return {};
            }

            const auto turns = thread.GetNamedArray(L"turns");
            parsed.Turns.reserve(turns.Size());
            size_t itemCount{};
            for (const auto& value : turns)
            {
                if (value.ValueType() != JsonValueType::Object)
                {
                    return _error(CodexAppServerErrorKind::MalformedResponse,
                                  L"App Server returned a non-object Turn",
                                  L"thread/read");
                }
                CodexAppServerTurn turn;
                if (auto error = _parseTurn(value.GetObject(), parsed.Summary.UpdatedAtUnixSeconds, turn, itemCount))
                {
                    return error;
                }
                parsed.Turns.emplace_back(std::move(turn));
            }
            return {};
        }

        std::unique_ptr<ICodexAppServerTransport> _makeTransport(const CodexAppServerClient::TransportFactory& factory)
        {
            return factory ? factory() : std::make_unique<Win32CodexAppServerTransport>();
        }
    }

    CodexAppServerClient::CodexAppServerClient(TransportFactory transportFactory,
                                               const CodexAppServerTimeouts timeouts) :
        _transportFactory{ std::move(transportFactory) },
        _timeouts{ timeouts }
    {
    }

    CodexAppServerListResult CodexAppServerClient::ListThreads(const std::wstring_view cwd,
                                                               const uint32_t pageLimit,
                                                               const uint32_t maximumPages,
                                                               const HANDLE cancellationEvent) const noexcept
    {
        CodexAppServerListResult result;
        try
        {
            if (cwd.size() > MaximumCwdCharacters || _hasControlCharacter(cwd) ||
                pageLimit == 0 || pageLimit > MaximumPageLimit ||
                maximumPages == 0 || maximumPages > MaximumPages)
            {
                result.Error = _error(CodexAppServerErrorKind::InvalidArgument, L"Invalid thread/list arguments", L"thread/list");
                return result;
            }

            ProtocolSession session{ _makeTransport(_transportFactory) };
            if (auto error = session.Initialize(_timeouts.Initialize, cancellationEvent))
            {
                result.Error = std::move(error);
                return result;
            }

            const auto listDeadline = std::chrono::steady_clock::now() + _timeouts.ListTotal;
            std::optional<winrt::hstring> cursor;
            std::unordered_set<std::wstring> seenCursors;
            for (uint32_t page = 0; page < maximumPages; ++page)
            {
                const auto now = std::chrono::steady_clock::now();
                if (_timeouts.ListTotal <= std::chrono::milliseconds::zero() || now >= listDeadline)
                {
                    result.Threads.clear();
                    result.Error = _error(CodexAppServerErrorKind::Timeout,
                                          L"App Server thread/list exceeded its total time limit",
                                          L"thread/list");
                    return result;
                }
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(listDeadline - now);
                const auto pageTimeout = std::min(_timeouts.ListPage, remaining);

                JsonObject params;
                params.Insert(L"limit", JsonValue::CreateNumberValue(pageLimit));
                params.Insert(L"sortKey", JsonValue::CreateStringValue(L"updated_at"));
                params.Insert(L"sortDirection", JsonValue::CreateStringValue(L"desc"));
                if (!cwd.empty())
                {
                    params.Insert(L"cwd", JsonValue::CreateStringValue(cwd));
                }
                if (cursor)
                {
                    params.Insert(L"cursor", JsonValue::CreateStringValue(cursor.value()));
                }

                auto response = session.Request(L"thread/list", params, pageTimeout, cancellationEvent);
                if (response.Error)
                {
                    result.Threads.clear();
                    result.Error = std::move(response.Error);
                    return result;
                }
                if (!_isType(response.Result, L"data", JsonValueType::Array) ||
                    (response.Result.HasKey(L"nextCursor") && !_isNullOrType(response.Result, L"nextCursor", JsonValueType::String)))
                {
                    result.Threads.clear();
                    result.Error = _error(CodexAppServerErrorKind::MalformedResponse,
                                          L"App Server returned an invalid thread/list result",
                                          L"thread/list");
                    return result;
                }

                const auto data = response.Result.GetNamedArray(L"data");
                if (data.Size() > MaximumThreads || result.Threads.size() > MaximumThreads - data.Size())
                {
                    result.Threads.clear();
                    result.Error = _error(CodexAppServerErrorKind::OutputLimitExceeded,
                                          L"App Server returned too many Threads",
                                          L"thread/list");
                    return result;
                }
                for (const auto& value : data)
                {
                    if (value.ValueType() != JsonValueType::Object)
                    {
                        result.Threads.clear();
                        result.Error = _error(CodexAppServerErrorKind::MalformedResponse,
                                              L"App Server returned a non-object Thread",
                                              L"thread/list");
                        return result;
                    }
                    CodexAppServerThreadSummary summary;
                    if (auto error = _parseThreadSummary(value.GetObject(), summary, L"thread/list"))
                    {
                        result.Threads.clear();
                        result.Error = std::move(error);
                        return result;
                    }
                    result.Threads.emplace_back(std::move(summary));
                }

                if (!response.Result.HasKey(L"nextCursor") ||
                    response.Result.GetNamedValue(L"nextCursor").ValueType() == JsonValueType::Null)
                {
                    return result;
                }

                auto nextCursor = response.Result.GetNamedString(L"nextCursor");
                if (nextCursor.empty() || nextCursor.size() > MaximumSummaryCharacters ||
                    _hasControlCharacter(nextCursor))
                {
                    result.Threads.clear();
                    result.Error = _error(CodexAppServerErrorKind::MalformedResponse,
                                          L"App Server returned an invalid pagination cursor",
                                          L"thread/list");
                    return result;
                }
                std::wstring cursorKey{ nextCursor };
                if (!seenCursors.emplace(cursorKey).second)
                {
                    result.Threads.clear();
                    result.Error = _error(CodexAppServerErrorKind::PaginationLoop,
                                          L"App Server repeated a pagination cursor",
                                          L"thread/list");
                    return result;
                }
                cursor = std::move(nextCursor);
            }

            result.Threads.clear();
            result.Error = _error(CodexAppServerErrorKind::OutputLimitExceeded,
                                  L"App Server thread/list exceeded the page limit",
                                  L"thread/list");
            return result;
        }
        catch (...)
        {
            result.Threads.clear();
            result.Error = _error(CodexAppServerErrorKind::InternalError, L"thread/list failed", L"thread/list");
            return result;
        }
    }

    CodexAppServerReadResult CodexAppServerClient::ReadThread(const std::wstring_view threadId,
                                                              const bool includeTurns,
                                                              const HANDLE cancellationEvent) const noexcept
    {
        CodexAppServerReadResult result;
        try
        {
            if (!_validIdentifier(threadId))
            {
                result.Error = _error(CodexAppServerErrorKind::InvalidArgument, L"Invalid Thread ID", L"thread/read");
                return result;
            }

            ProtocolSession session{ _makeTransport(_transportFactory) };
            if (auto error = session.Initialize(_timeouts.Initialize, cancellationEvent))
            {
                result.Error = std::move(error);
                return result;
            }

            JsonObject params;
            params.Insert(L"threadId", JsonValue::CreateStringValue(threadId));
            params.Insert(L"includeTurns", JsonValue::CreateBooleanValue(includeTurns));
            auto response = session.Request(L"thread/read", params, _timeouts.Read, cancellationEvent);
            if (response.Error)
            {
                result.Error = std::move(response.Error);
                return result;
            }
            if (!_isType(response.Result, L"thread", JsonValueType::Object))
            {
                result.Error = _error(CodexAppServerErrorKind::MalformedResponse,
                                      L"App Server returned an invalid thread/read result",
                                      L"thread/read");
                return result;
            }
            if (auto error = _parseThread(response.Result.GetNamedObject(L"thread"), includeTurns, result.Thread))
            {
                result.Error = std::move(error);
                return result;
            }
            if (result.Thread.Summary.Id != threadId)
            {
                result.Thread = {};
                result.Error = _error(CodexAppServerErrorKind::UnexpectedResponse,
                                      L"thread/read returned a different Thread ID",
                                      L"thread/read");
            }
            return result;
        }
        catch (...)
        {
            result.Thread = {};
            result.Error = _error(CodexAppServerErrorKind::InternalError, L"thread/read failed", L"thread/read");
            return result;
        }
    }

    CodexAppServerResumeResult CodexAppServerClient::ProbeResume(const std::wstring_view threadId,
                                                                 const HANDLE cancellationEvent) const noexcept
    {
        CodexAppServerResumeResult result;
        try
        {
            if (!_validIdentifier(threadId))
            {
                result.Error = _error(CodexAppServerErrorKind::InvalidArgument, L"Invalid Thread ID", L"thread/resume");
                return result;
            }

            ProtocolSession session{ _makeTransport(_transportFactory) };
            if (auto error = session.Initialize(_timeouts.Initialize, cancellationEvent))
            {
                result.Error = std::move(error);
                return result;
            }

            JsonObject params;
            params.Insert(L"threadId", JsonValue::CreateStringValue(threadId));
            auto response = session.Request(L"thread/resume", params, _timeouts.Resume, cancellationEvent);
            if (response.Error)
            {
                result.Error = std::move(response.Error);
                return result;
            }
            if (!_isType(response.Result, L"thread", JsonValueType::Object))
            {
                result.Error = _error(CodexAppServerErrorKind::MalformedResponse,
                                      L"App Server returned an invalid thread/resume result",
                                      L"thread/resume");
                return result;
            }
            if (auto error = _parseThreadSummary(response.Result.GetNamedObject(L"thread"), result.Thread, L"thread/resume"))
            {
                result.Error = std::move(error);
                return result;
            }
            if (result.Thread.Id != threadId)
            {
                result.Thread = {};
                result.Error = _error(CodexAppServerErrorKind::UnexpectedResponse,
                                      L"thread/resume returned a different Thread ID",
                                      L"thread/resume");
                return result;
            }

            JsonObject unsubscribeParams;
            unsubscribeParams.Insert(L"threadId", JsonValue::CreateStringValue(threadId));
            static_cast<void>(session.Request(L"thread/unsubscribe",
                                              unsubscribeParams,
                                              _timeouts.Unsubscribe,
                                              cancellationEvent));
            if (_isCancelled(cancellationEvent))
            {
                result.Thread = {};
                result.Error = _error(CodexAppServerErrorKind::Cancelled,
                                      L"App Server operation was cancelled",
                                      L"thread/resume");
            }
            return result;
        }
        catch (...)
        {
            result.Thread = {};
            result.Error = _error(CodexAppServerErrorKind::InternalError, L"thread/resume failed", L"thread/resume");
            return result;
        }
    }
}
