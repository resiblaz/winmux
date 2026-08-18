// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceAutomation.h"

#include "../../types/inc/utils.hpp"

#include <aclapi.h>
#include <wil/token_helpers.h>

using namespace winrt::Windows::Data::Json;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr auto PipeRequestTimeout{ std::chrono::seconds{ 10 } };
        constexpr auto UiDispatchTimeout{ std::chrono::seconds{ 5 } };

        WorkspaceAutomationParseResult _parseError(std::string id, std::string code, std::string message)
        {
            return {
                .Id = std::move(id),
                .ErrorCode = std::move(code),
                .ErrorMessage = std::move(message),
            };
        }

        bool _hasOnlyKeys(const JsonObject& object, std::initializer_list<std::wstring_view> allowed)
        {
            for (const auto& pair : object)
            {
                const std::wstring_view key{ pair.Key() };
                if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
                {
                    return false;
                }
            }
            return true;
        }

        bool _isValueType(const JsonObject& object, const std::wstring_view key, const JsonValueType type)
        {
            return object.HasKey(key) && object.GetNamedValue(key).ValueType() == type;
        }

        std::optional<winrt::guid> _parseWorkspaceId(const winrt::hstring& value)
        {
            if (value.size() != 36)
            {
                return std::nullopt;
            }

            try
            {
                const auto parsed = ::Microsoft::Console::Utils::GuidFromPlainString(value.c_str());
                if (parsed != GUID{})
                {
                    return parsed;
                }
            }
            catch (...)
            {
            }
            return std::nullopt;
        }

        std::string _stringify(const JsonObject& object)
        {
            return winrt::to_string(object.Stringify());
        }
    }

    WorkspaceAutomationParseResult WorkspaceAutomationProtocol::ParseRequest(const std::string_view request)
    {
        if (request.size() > MaximumMessageBytes)
        {
            return _parseError({}, "message_too_large", "Request exceeds the 64 KiB limit");
        }

        JsonObject root{ nullptr };
        try
        {
            if (!JsonObject::TryParse(winrt::to_hstring(request), root) || !root)
            {
                return _parseError({}, "parse_error", "Request is not valid JSON");
            }
        }
        catch (...)
        {
            return _parseError({}, "parse_error", "Request is not valid UTF-8 JSON");
        }

        std::string id;
        if (_isValueType(root, L"id", JsonValueType::String))
        {
            id = winrt::to_string(root.GetNamedString(L"id"));
        }

        if (!_hasOnlyKeys(root, { L"version", L"id", L"method", L"params" }) ||
            !_isValueType(root, L"version", JsonValueType::Number) ||
            !_isValueType(root, L"id", JsonValueType::String) ||
            !_isValueType(root, L"method", JsonValueType::String))
        {
            return _parseError(std::move(id), "invalid_request", "Request must contain only version, id, method, and params");
        }

        if (id.empty() || id.size() > 128)
        {
            return _parseError(std::move(id), "invalid_request", "Request id must contain 1 to 128 UTF-8 bytes");
        }

        const auto version = root.GetNamedNumber(L"version");
        if (!std::isfinite(version) || std::floor(version) != version || version != Version)
        {
            return _parseError(std::move(id), "unsupported_version", "Only protocol version 1 is supported");
        }

        JsonObject params;
        if (root.HasKey(L"params"))
        {
            if (!_isValueType(root, L"params", JsonValueType::Object))
            {
                return _parseError(std::move(id), "invalid_params", "params must be an object");
            }
            params = root.GetNamedObject(L"params");
        }

        const auto method = root.GetNamedString(L"method");
        if (method == L"workspace.list" || method == L"workspace.create")
        {
            if (!_hasOnlyKeys(params, {}))
            {
                return _parseError(std::move(id), "invalid_params", "This method does not accept parameters");
            }
            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = method == L"workspace.list" ? WorkspaceAutomationMethod::List : WorkspaceAutomationMethod::Create,
                },
                .Id = std::move(id),
            };
        }

        if (method == L"workspace.select" || method == L"workspace.close")
        {
            if (!_hasOnlyKeys(params, { L"id" }) || !_isValueType(params, L"id", JsonValueType::String))
            {
                return _parseError(std::move(id), "invalid_params", "params.id must be the only parameter");
            }

            const auto workspaceId = _parseWorkspaceId(params.GetNamedString(L"id"));
            if (!workspaceId)
            {
                return _parseError(std::move(id), "invalid_params", "params.id must be a non-zero canonical UUID");
            }

            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = method == L"workspace.select" ? WorkspaceAutomationMethod::Select : WorkspaceAutomationMethod::Close,
                    .WorkspaceId = workspaceId,
                },
                .Id = std::move(id),
            };
        }

        return _parseError(std::move(id), "method_not_found", "Unknown automation method");
    }

    std::string WorkspaceAutomationProtocol::MakeResult(const std::string_view id, const JsonObject& result)
    {
        JsonObject response;
        response.Insert(L"version", JsonValue::CreateNumberValue(Version));
        response.Insert(L"id", JsonValue::CreateStringValue(winrt::to_hstring(id)));
        response.Insert(L"result", result);
        return _stringify(response);
    }

    std::string WorkspaceAutomationProtocol::MakeError(const std::string_view id, const std::string_view code, const std::string_view message)
    {
        JsonObject error;
        error.Insert(L"code", JsonValue::CreateStringValue(winrt::to_hstring(code)));
        error.Insert(L"message", JsonValue::CreateStringValue(winrt::to_hstring(message)));

        JsonObject response;
        response.Insert(L"version", JsonValue::CreateNumberValue(Version));
        response.Insert(L"id", JsonValue::CreateStringValue(winrt::to_hstring(id)));
        response.Insert(L"error", error);
        return _stringify(response);
    }

    struct WorkspaceAutomationServer::Impl
    {
        enum class IoResult
        {
            Completed,
            Stopped,
            TimedOut,
            Disconnected,
            TooLarge,
            Failed,
        };

        struct DispatchState
        {
            wil::unique_handle Event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            std::string Response;
        };

        Impl(std::wstring pipeName, winrt::Windows::System::DispatcherQueue dispatcher, RequestHandler handler) :
            PipeName{ std::move(pipeName) },
            Dispatcher{ std::move(dispatcher) },
            Handler{ std::move(handler) }
        {
        }

        ~Impl()
        {
            Stop();
        }

        bool Start()
        {
            if (Worker.joinable())
            {
                return true;
            }

            StopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!StopEvent || !_initializeSecurity())
            {
                return false;
            }

            try
            {
                Worker = std::thread{ [this]() { _run(); } };
                return true;
            }
            catch (...)
            {
                StopEvent.reset();
                return false;
            }
        }

        void Stop() noexcept
        {
            if (StopEvent)
            {
                SetEvent(StopEvent.get());
            }
            if (Worker.joinable())
            {
                Worker.join();
            }
            StopEvent.reset();
        }

        bool _initializeSecurity()
        {
            try
            {
                wil::unique_handle processToken{ GetCurrentProcessToken() };
                const auto tokenUser = wil::get_token_information<TOKEN_USER>(processToken.get());

                EXPLICIT_ACCESSW access{};
                access.grfAccessPermissions = GENERIC_ALL;
                access.grfAccessMode = SET_ACCESS;
                access.grfInheritance = NO_INHERITANCE;
                access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
                access.Trustee.TrusteeType = TRUSTEE_IS_USER;
                access.Trustee.ptstrName = static_cast<LPWSTR>(tokenUser->User.Sid);

                Acl.reset();
                const auto error = SetEntriesInAclW(1, &access, nullptr, wil::out_param_ptr<PACL*>(Acl));
                if (error != ERROR_SUCCESS)
                {
                    SetLastError(error);
                    return false;
                }

                if (!InitializeSecurityDescriptor(&SecurityDescriptor, SECURITY_DESCRIPTOR_REVISION) ||
                    !SetSecurityDescriptorDacl(&SecurityDescriptor, TRUE, Acl.get(), FALSE))
                {
                    return false;
                }

                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        wil::unique_hfile _createPipe() const
        {
            SECURITY_ATTRIBUTES attributes{
                .nLength = sizeof(SECURITY_ATTRIBUTES),
                .lpSecurityDescriptor = const_cast<SECURITY_DESCRIPTOR*>(&SecurityDescriptor),
                .bInheritHandle = FALSE,
            };
            return wil::unique_hfile{ CreateNamedPipeW(PipeName.c_str(),
                                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                                       1,
                                                       gsl::narrow<DWORD>(WorkspaceAutomationProtocol::MaximumMessageBytes),
                                                       gsl::narrow<DWORD>(WorkspaceAutomationProtocol::MaximumMessageBytes),
                                                       gsl::narrow<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(PipeRequestTimeout).count()),
                                                       &attributes) };
        }

        IoResult _waitForIo(const HANDLE pipe, OVERLAPPED& overlapped, const std::chrono::milliseconds timeout, DWORD& bytesTransferred) const
        {
            const HANDLE handles[]{ StopEvent.get(), overlapped.hEvent };
            const auto wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, gsl::narrow<DWORD>(timeout.count()));
            if (wait == WAIT_OBJECT_0)
            {
                CancelIoEx(pipe, &overlapped);
                GetOverlappedResult(pipe, &overlapped, &bytesTransferred, TRUE);
                return IoResult::Stopped;
            }
            if (wait == WAIT_TIMEOUT)
            {
                CancelIoEx(pipe, &overlapped);
                GetOverlappedResult(pipe, &overlapped, &bytesTransferred, TRUE);
                return IoResult::TimedOut;
            }
            if (wait != WAIT_OBJECT_0 + 1 || !GetOverlappedResult(pipe, &overlapped, &bytesTransferred, FALSE))
            {
                const auto error = GetLastError();
                return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ? IoResult::Disconnected : IoResult::Failed;
            }
            return IoResult::Completed;
        }

        IoResult _connect(const HANDLE pipe) const
        {
            wil::unique_handle event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            if (!event)
            {
                return IoResult::Failed;
            }

            OVERLAPPED overlapped{};
            overlapped.hEvent = event.get();
            if (ConnectNamedPipe(pipe, &overlapped))
            {
                return IoResult::Completed;
            }

            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED)
            {
                return IoResult::Completed;
            }
            if (error != ERROR_IO_PENDING)
            {
                return IoResult::Failed;
            }

            DWORD bytesTransferred = 0;
            return _waitForIo(pipe, overlapped, std::chrono::duration_cast<std::chrono::milliseconds>(PipeRequestTimeout), bytesTransferred);
        }

        IoResult _readRequest(const HANDLE pipe, std::string& request) const
        {
            const auto deadline = std::chrono::steady_clock::now() + PipeRequestTimeout;
            std::array<char, 4096> buffer{};

            for (;;)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                {
                    return IoResult::TimedOut;
                }

                wil::unique_handle event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
                if (!event)
                {
                    return IoResult::Failed;
                }
                OVERLAPPED overlapped{};
                overlapped.hEvent = event.get();
                DWORD bytesTransferred = 0;
                auto result = IoResult::Completed;
                if (!ReadFile(pipe, buffer.data(), gsl::narrow<DWORD>(buffer.size()), &bytesTransferred, &overlapped))
                {
                    const auto error = GetLastError();
                    if (error == ERROR_BROKEN_PIPE)
                    {
                        return IoResult::Disconnected;
                    }
                    if (error != ERROR_IO_PENDING)
                    {
                        return IoResult::Failed;
                    }
                    result = _waitForIo(pipe,
                                        overlapped,
                                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                                        bytesTransferred);
                }

                if (result != IoResult::Completed)
                {
                    return result;
                }
                if (bytesTransferred == 0)
                {
                    return IoResult::Disconnected;
                }

                request.append(buffer.data(), bytesTransferred);
                if (const auto newline = request.find('\n'); newline != std::string::npos)
                {
                    if (newline > WorkspaceAutomationProtocol::MaximumMessageBytes)
                    {
                        return IoResult::TooLarge;
                    }
                    request.resize(newline);
                    if (!request.empty() && request.back() == '\r')
                    {
                        request.pop_back();
                    }
                    return IoResult::Completed;
                }
                if (request.size() > WorkspaceAutomationProtocol::MaximumMessageBytes)
                {
                    return IoResult::TooLarge;
                }
            }
        }

        IoResult _writeResponse(const HANDLE pipe, std::string response) const
        {
            response.push_back('\n');
            size_t offset = 0;
            const auto deadline = std::chrono::steady_clock::now() + PipeRequestTimeout;
            while (offset < response.size())
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                {
                    return IoResult::TimedOut;
                }

                wil::unique_handle event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
                if (!event)
                {
                    return IoResult::Failed;
                }
                OVERLAPPED overlapped{};
                overlapped.hEvent = event.get();
                DWORD bytesTransferred = 0;
                auto result = IoResult::Completed;
                const auto remaining = gsl::narrow<DWORD>(response.size() - offset);
                if (!WriteFile(pipe, response.data() + offset, remaining, &bytesTransferred, &overlapped))
                {
                    const auto error = GetLastError();
                    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
                    {
                        return IoResult::Disconnected;
                    }
                    if (error != ERROR_IO_PENDING)
                    {
                        return IoResult::Failed;
                    }
                    result = _waitForIo(pipe,
                                        overlapped,
                                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                                        bytesTransferred);
                }
                if (result != IoResult::Completed)
                {
                    return result;
                }
                if (bytesTransferred == 0)
                {
                    return IoResult::Disconnected;
                }
                offset += bytesTransferred;
            }
            return IoResult::Completed;
        }

        std::string _dispatchRequest(std::string request) const
        {
            if (!Dispatcher)
            {
                try
                {
                    return Handler(request);
                }
                catch (...)
                {
                    return WorkspaceAutomationProtocol::MakeError({}, "internal_error", "Request handler failed");
                }
            }

            const auto state = std::make_shared<DispatchState>();
            if (!state->Event)
            {
                return WorkspaceAutomationProtocol::MakeError({}, "internal_error", "Unable to create dispatch event");
            }

            const auto handler = Handler;
            if (!Dispatcher.TryEnqueue([state, handler, request = std::move(request)]() {
                    try
                    {
                        state->Response = handler(request);
                    }
                    catch (...)
                    {
                        state->Response = WorkspaceAutomationProtocol::MakeError({}, "internal_error", "Request handler failed");
                    }
                    SetEvent(state->Event.get());
                }))
            {
                return WorkspaceAutomationProtocol::MakeError({}, "server_unavailable", "UI dispatcher is unavailable");
            }

            const HANDLE handles[]{ StopEvent.get(), state->Event.get() };
            const auto wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, gsl::narrow<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(UiDispatchTimeout).count()));
            if (wait == WAIT_OBJECT_0 + 1)
            {
                return std::move(state->Response);
            }
            if (wait == WAIT_TIMEOUT)
            {
                return WorkspaceAutomationProtocol::MakeError({}, "request_timeout", "UI request timed out");
            }
            return {};
        }

        void _serve(const HANDLE pipe) const
        {
            std::string request;
            const auto readResult = _readRequest(pipe, request);
            std::string response;
            if (readResult == IoResult::Completed)
            {
                response = _dispatchRequest(std::move(request));
            }
            else if (readResult == IoResult::TooLarge)
            {
                response = WorkspaceAutomationProtocol::MakeError({}, "message_too_large", "Request exceeds the 64 KiB limit");
            }
            else if (readResult == IoResult::TimedOut)
            {
                response = WorkspaceAutomationProtocol::MakeError({}, "request_timeout", "Request timed out");
            }

            if (!response.empty())
            {
                if (response.size() > WorkspaceAutomationProtocol::MaximumMessageBytes)
                {
                    response = WorkspaceAutomationProtocol::MakeError({}, "response_too_large", "Response exceeds the 64 KiB limit");
                }
                _writeResponse(pipe, std::move(response));
            }
        }

        void _run() noexcept
        {
            try
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                const auto uninitialize = wil::scope_exit([]() noexcept { winrt::uninit_apartment(); });
                while (WaitForSingleObject(StopEvent.get(), 0) != WAIT_OBJECT_0)
                {
                    auto pipe = _createPipe();
                    if (!pipe)
                    {
                        if (WaitForSingleObject(StopEvent.get(), 250) == WAIT_OBJECT_0)
                        {
                            break;
                        }
                        continue;
                    }

                    const auto connectResult = _connect(pipe.get());
                    if (connectResult == IoResult::Completed)
                    {
                        _serve(pipe.get());
                        DisconnectNamedPipe(pipe.get());
                    }
                    else if (connectResult == IoResult::Stopped)
                    {
                        break;
                    }
                }
            }
            catch (...)
            {
            }
        }

        std::wstring PipeName;
        winrt::Windows::System::DispatcherQueue Dispatcher{ nullptr };
        RequestHandler Handler;
        wil::unique_handle StopEvent;
        std::thread Worker;
        wil::unique_hlocal_ptr<ACL> Acl;
        SECURITY_DESCRIPTOR SecurityDescriptor{};
    };

    WorkspaceAutomationServer::WorkspaceAutomationServer(std::wstring pipeName,
                                                         winrt::Windows::System::DispatcherQueue dispatcher,
                                                         RequestHandler handler) :
        _impl{ std::make_unique<Impl>(std::move(pipeName), std::move(dispatcher), std::move(handler)) }
    {
    }

    WorkspaceAutomationServer::~WorkspaceAutomationServer() = default;

    bool WorkspaceAutomationServer::Start()
    {
        return _impl->Start();
    }

    void WorkspaceAutomationServer::Stop() noexcept
    {
        _impl->Stop();
    }

    std::wstring_view WorkspaceAutomationServer::PipeName() const noexcept
    {
        return _impl->PipeName;
    }

    std::wstring WorkspaceAutomationServer::MakePipeName(const uint32_t processId, const uint64_t windowId)
    {
        return fmt::format(FMT_COMPILE(LR"(\\.\pipe\LOCAL\cmux-terminal-{}-{})"), processId, windowId);
    }
}
