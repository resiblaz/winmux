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

        std::optional<uint32_t> _parsePaneId(const JsonObject& params)
        {
            if (!_isValueType(params, L"paneId", JsonValueType::Number))
            {
                return std::nullopt;
            }

            const auto value = params.GetNamedNumber(L"paneId");
            if (!std::isfinite(value) || std::floor(value) != value || value < 0 || value > std::numeric_limits<uint32_t>::max())
            {
                return std::nullopt;
            }
            return gsl::narrow_cast<uint32_t>(value);
        }

        std::optional<uint32_t> _parseBoundedUInt32(const JsonObject& params,
                                                    const std::wstring_view key,
                                                    const uint32_t defaultValue,
                                                    const uint32_t maximumValue)
        {
            if (!params.HasKey(key))
            {
                return defaultValue;
            }
            if (!_isValueType(params, key, JsonValueType::Number))
            {
                return std::nullopt;
            }

            const auto value = params.GetNamedNumber(key);
            if (!std::isfinite(value) || std::floor(value) != value || value < 1 || value > maximumValue)
            {
                return std::nullopt;
            }
            return gsl::narrow_cast<uint32_t>(value);
        }

        std::optional<WorkspaceAutomationSplitDirection> _parseSplitDirection(const JsonObject& params)
        {
            if (!params.HasKey(L"direction"))
            {
                return WorkspaceAutomationSplitDirection::Automatic;
            }
            if (!_isValueType(params, L"direction", JsonValueType::String))
            {
                return std::nullopt;
            }

            const auto value = params.GetNamedString(L"direction");
            if (value == L"automatic")
            {
                return WorkspaceAutomationSplitDirection::Automatic;
            }
            if (value == L"up")
            {
                return WorkspaceAutomationSplitDirection::Up;
            }
            if (value == L"right")
            {
                return WorkspaceAutomationSplitDirection::Right;
            }
            if (value == L"down")
            {
                return WorkspaceAutomationSplitDirection::Down;
            }
            if (value == L"left")
            {
                return WorkspaceAutomationSplitDirection::Left;
            }
            return std::nullopt;
        }

        bool _isValidText(const winrt::hstring& value, const size_t minimumLength, const size_t maximumLength, const bool allowLineBreaks)
        {
            if (value.size() < minimumLength || value.size() > maximumLength)
            {
                return false;
            }
            if (minimumLength > 0 && std::all_of(value.begin(), value.end(), [](const wchar_t character) { return std::iswspace(character); }))
            {
                return false;
            }

            return std::none_of(value.begin(), value.end(), [&](const wchar_t character) {
                const auto isControlCharacter = character < L' ' || (character >= L'\x7f' && character <= L'\x9f');
                if (!isControlCharacter)
                {
                    return false;
                }
                return !allowLineBreaks || (character != L'\r' && character != L'\n' && character != L'\t');
            });
        }

        std::optional<WorkspaceAutomationProgressState> _parseProgressState(const winrt::hstring& value)
        {
            if (value == L"normal")
            {
                return WorkspaceAutomationProgressState::Normal;
            }
            if (value == L"error")
            {
                return WorkspaceAutomationProgressState::Error;
            }
            if (value == L"indeterminate")
            {
                return WorkspaceAutomationProgressState::Indeterminate;
            }
            if (value == L"paused")
            {
                return WorkspaceAutomationProgressState::Paused;
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

        if (method == L"pane.list" || method == L"pane.focus" || method == L"pane.split")
        {
            const auto allowedKeys = method == L"pane.list" ?
                                         std::initializer_list<std::wstring_view>{ L"workspaceId" } :
                                     method == L"pane.focus" ?
                                         std::initializer_list<std::wstring_view>{ L"workspaceId", L"paneId" } :
                                         std::initializer_list<std::wstring_view>{ L"workspaceId", L"paneId", L"direction", L"size" };
            if (!_hasOnlyKeys(params, allowedKeys) || !_isValueType(params, L"workspaceId", JsonValueType::String))
            {
                return _parseError(std::move(id), "invalid_params", "Pane methods require a workspaceId and only documented parameters");
            }

            const auto workspaceId = _parseWorkspaceId(params.GetNamedString(L"workspaceId"));
            if (!workspaceId)
            {
                return _parseError(std::move(id), "invalid_params", "params.workspaceId must be a non-zero canonical UUID");
            }

            if (method == L"pane.list")
            {
                return {
                    .Request = WorkspaceAutomationRequest{
                        .Id = id,
                        .Method = WorkspaceAutomationMethod::PaneList,
                        .WorkspaceId = workspaceId,
                    },
                    .Id = std::move(id),
                };
            }

            const auto paneId = _parsePaneId(params);
            if (!paneId)
            {
                return _parseError(std::move(id), "invalid_params", "params.paneId must be a UInt32");
            }

            if (method == L"pane.focus")
            {
                return {
                    .Request = WorkspaceAutomationRequest{
                        .Id = id,
                        .Method = WorkspaceAutomationMethod::PaneFocus,
                        .WorkspaceId = workspaceId,
                        .PaneId = paneId,
                    },
                    .Id = std::move(id),
                };
            }

            const auto direction = _parseSplitDirection(params);
            if (!direction)
            {
                return _parseError(std::move(id), "invalid_params", "params.direction must be automatic, up, right, down, or left");
            }

            double splitSize = 0.5;
            if (params.HasKey(L"size"))
            {
                if (!_isValueType(params, L"size", JsonValueType::Number))
                {
                    return _parseError(std::move(id), "invalid_params", "params.size must be a number from 0.1 through 0.9");
                }
                splitSize = params.GetNamedNumber(L"size");
                if (!std::isfinite(splitSize) || splitSize < 0.1 || splitSize > 0.9)
                {
                    return _parseError(std::move(id), "invalid_params", "params.size must be a number from 0.1 through 0.9");
                }
            }

            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = WorkspaceAutomationMethod::PaneSplit,
                    .WorkspaceId = workspaceId,
                    .PaneId = paneId,
                    .SplitDirection = direction.value(),
                    .SplitSize = gsl::narrow_cast<float>(splitSize),
                },
                .Id = std::move(id),
            };
        }

        if (method == L"notification.list" || method == L"notification.clear")
        {
            if (!_hasOnlyKeys(params, { L"workspaceId" }) || !_isValueType(params, L"workspaceId", JsonValueType::String))
            {
                return _parseError(std::move(id), "invalid_params", "Notification method requires only params.workspaceId");
            }
            const auto workspaceId = _parseWorkspaceId(params.GetNamedString(L"workspaceId"));
            if (!workspaceId)
            {
                return _parseError(std::move(id), "invalid_params", "params.workspaceId must be a non-zero canonical UUID");
            }
            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = method == L"notification.list" ? WorkspaceAutomationMethod::NotificationList : WorkspaceAutomationMethod::NotificationClear,
                    .WorkspaceId = workspaceId,
                },
                .Id = std::move(id),
            };
        }

        if (method == L"notification.create")
        {
            if (!_hasOnlyKeys(params, { L"workspaceId", L"paneId", L"title", L"body" }) ||
                !_isValueType(params, L"workspaceId", JsonValueType::String) ||
                !_isValueType(params, L"title", JsonValueType::String) ||
                !_isValueType(params, L"body", JsonValueType::String))
            {
                return _parseError(std::move(id), "invalid_params", "notification.create requires workspaceId, paneId, title, and body");
            }

            const auto workspaceId = _parseWorkspaceId(params.GetNamedString(L"workspaceId"));
            const auto paneId = _parsePaneId(params);
            const auto title = params.GetNamedString(L"title");
            const auto body = params.GetNamedString(L"body");
            if (!workspaceId || !paneId || !_isValidText(title, 1, 128, false) || !_isValidText(body, 1, 1024, true))
            {
                return _parseError(std::move(id), "invalid_params", "Notification target or text is invalid");
            }

            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = WorkspaceAutomationMethod::NotificationCreate,
                    .WorkspaceId = workspaceId,
                    .PaneId = paneId,
                    .Title = title,
                    .Body = body,
                },
                .Id = std::move(id),
            };
        }

        if (method == L"notification.markRead")
        {
            if (!_hasOnlyKeys(params, { L"workspaceId", L"paneId" }) || !_isValueType(params, L"workspaceId", JsonValueType::String))
            {
                return _parseError(std::move(id), "invalid_params", "notification.markRead requires workspaceId and optional paneId");
            }
            const auto workspaceId = _parseWorkspaceId(params.GetNamedString(L"workspaceId"));
            const auto paneId = params.HasKey(L"paneId") ? _parsePaneId(params) : std::optional<uint32_t>{};
            if (!workspaceId || (params.HasKey(L"paneId") && !paneId))
            {
                return _parseError(std::move(id), "invalid_params", "Notification target is invalid");
            }
            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = WorkspaceAutomationMethod::NotificationMarkRead,
                    .WorkspaceId = workspaceId,
                    .PaneId = paneId,
                },
                .Id = std::move(id),
            };
        }

        if (method == L"status.get" || method == L"status.clear" || method == L"progress.clear")
        {
            if (!_hasOnlyKeys(params, { L"workspaceId" }) || !_isValueType(params, L"workspaceId", JsonValueType::String))
            {
                return _parseError(std::move(id), "invalid_params", "Status method requires only params.workspaceId");
            }
            const auto workspaceId = _parseWorkspaceId(params.GetNamedString(L"workspaceId"));
            if (!workspaceId)
            {
                return _parseError(std::move(id), "invalid_params", "params.workspaceId must be a non-zero canonical UUID");
            }

            const auto parsedMethod = method == L"status.get"   ? WorkspaceAutomationMethod::StatusGet :
                                      method == L"status.clear" ? WorkspaceAutomationMethod::StatusClear :
                                                                  WorkspaceAutomationMethod::ProgressClear;
            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = parsedMethod,
                    .WorkspaceId = workspaceId,
                },
                .Id = std::move(id),
            };
        }

        if (method == L"status.set")
        {
            if (!_hasOnlyKeys(params, { L"workspaceId", L"text" }) ||
                !_isValueType(params, L"workspaceId", JsonValueType::String) ||
                !_isValueType(params, L"text", JsonValueType::String))
            {
                return _parseError(std::move(id), "invalid_params", "status.set requires workspaceId and text");
            }
            const auto workspaceId = _parseWorkspaceId(params.GetNamedString(L"workspaceId"));
            const auto text = params.GetNamedString(L"text");
            if (!workspaceId || !_isValidText(text, 1, 96, false))
            {
                return _parseError(std::move(id), "invalid_params", "Status text must contain 1 to 96 characters without control characters");
            }
            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = WorkspaceAutomationMethod::StatusSet,
                    .WorkspaceId = workspaceId,
                    .Text = text,
                },
                .Id = std::move(id),
            };
        }

        if (method == L"progress.set")
        {
            if (!_hasOnlyKeys(params, { L"workspaceId", L"state", L"value" }) ||
                !_isValueType(params, L"workspaceId", JsonValueType::String) ||
                !_isValueType(params, L"state", JsonValueType::String) ||
                (params.HasKey(L"value") && !_isValueType(params, L"value", JsonValueType::Number)))
            {
                return _parseError(std::move(id), "invalid_params", "progress.set requires workspaceId, state, and optional value");
            }

            const auto workspaceId = _parseWorkspaceId(params.GetNamedString(L"workspaceId"));
            const auto state = _parseProgressState(params.GetNamedString(L"state"));
            double progressValue = params.GetNamedNumber(L"value", 0);
            if (!workspaceId || !state || !std::isfinite(progressValue) || std::floor(progressValue) != progressValue || progressValue < 0 || progressValue > 100)
            {
                return _parseError(std::move(id), "invalid_params", "Progress state or integer value from 0 through 100 is invalid");
            }

            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = WorkspaceAutomationMethod::ProgressSet,
                    .WorkspaceId = workspaceId,
                    .ProgressState = state.value(),
                    .ProgressValue = gsl::narrow_cast<uint32_t>(progressValue),
                },
                .Id = std::move(id),
            };
        }

        if (method == L"terminal.sendInput")
        {
            if (!_hasOnlyKeys(params, { L"workspaceId", L"paneId", L"text" }) ||
                !_isValueType(params, L"workspaceId", JsonValueType::String) ||
                !_isValueType(params, L"text", JsonValueType::String))
            {
                return _parseError(std::move(id), "invalid_params", "terminal.sendInput requires workspaceId, paneId, and text");
            }

            const auto workspaceId = _parseWorkspaceId(params.GetNamedString(L"workspaceId"));
            const auto paneId = _parsePaneId(params);
            const auto text = params.GetNamedString(L"text");
            if (!workspaceId || !paneId || text.empty() || text.size() > MaximumInputCharacters)
            {
                return _parseError(std::move(id), "invalid_params", "Input target or text length from 1 through 4096 is invalid");
            }

            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = WorkspaceAutomationMethod::TerminalSendInput,
                    .WorkspaceId = workspaceId,
                    .PaneId = paneId,
                    .Text = text,
                },
                .Id = std::move(id),
            };
        }

        if (method == L"terminal.readContent")
        {
            if (!_hasOnlyKeys(params, { L"workspaceId", L"paneId", L"maxLines", L"maxCharacters" }) ||
                !_isValueType(params, L"workspaceId", JsonValueType::String))
            {
                return _parseError(std::move(id), "invalid_params", "terminal.readContent requires a target and only documented limits");
            }

            const auto workspaceId = _parseWorkspaceId(params.GetNamedString(L"workspaceId"));
            const auto paneId = _parsePaneId(params);
            const auto maximumLines = _parseBoundedUInt32(params, L"maxLines", 50, MaximumReadLines);
            const auto maximumCharacters = _parseBoundedUInt32(params, L"maxCharacters", MaximumReadCharacters, MaximumReadCharacters);
            if (!workspaceId || !paneId || !maximumLines || !maximumCharacters)
            {
                return _parseError(std::move(id), "invalid_params", "Read target or integer limits are invalid");
            }

            return {
                .Request = WorkspaceAutomationRequest{
                    .Id = id,
                    .Method = WorkspaceAutomationMethod::TerminalReadContent,
                    .WorkspaceId = workspaceId,
                    .PaneId = paneId,
                    .MaximumLines = maximumLines.value(),
                    .MaximumCharacters = maximumCharacters.value(),
                },
                .Id = std::move(id),
            };
        }

        return _parseError(std::move(id), "method_not_found", "Unknown automation method");
    }

    WorkspaceAutomationContentResult WorkspaceAutomationProtocol::TailContent(const std::wstring_view content,
                                                                               const uint32_t maximumLines,
                                                                               const uint32_t maximumCharacters)
    {
        if (content.empty())
        {
            return {};
        }
        if (maximumLines == 0 || maximumCharacters == 0)
        {
            return { {}, true };
        }

        size_t start = 0;
        auto searchEnd = content.size();
        if (content.back() == L'\n')
        {
            --searchEnd;
        }

        for (uint32_t line = 0; line < maximumLines && searchEnd > 0; ++line)
        {
            const auto separator = content.rfind(L'\n', searchEnd - 1);
            if (separator == std::wstring_view::npos)
            {
                break;
            }
            if (line + 1 == maximumLines)
            {
                start = separator + 1;
                break;
            }
            searchEnd = separator;
        }

        if (content.size() - start > maximumCharacters)
        {
            start = content.size() - maximumCharacters;
            if (start > 0 && start < content.size() &&
                content[start] >= 0xdc00 && content[start] <= 0xdfff &&
                content[start - 1] >= 0xd800 && content[start - 1] <= 0xdbff)
            {
                ++start;
            }
        }

        return {
            .Text = winrt::hstring{ content.substr(start) },
            .Truncated = start > 0,
        };
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
