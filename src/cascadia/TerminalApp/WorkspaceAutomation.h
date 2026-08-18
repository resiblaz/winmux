// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.System.h>

namespace winrt::TerminalApp::implementation
{
    enum class WorkspaceAutomationMethod
    {
        List,
        Select,
        Create,
        Close,
        PaneList,
        PaneFocus,
        PaneSplit,
        NotificationList,
        NotificationCreate,
        NotificationMarkRead,
        NotificationClear,
        StatusGet,
        StatusSet,
        StatusClear,
        ProgressSet,
        ProgressClear,
        TerminalSendInput,
        TerminalReadContent,
    };

    enum class WorkspaceAutomationSplitDirection
    {
        Automatic,
        Up,
        Right,
        Down,
        Left,
    };

    enum class WorkspaceAutomationProgressState : uint64_t
    {
        None = 0,
        Normal = 1,
        Error = 2,
        Indeterminate = 3,
        Paused = 4,
    };

    struct WorkspaceAutomationRequest
    {
        std::string Id;
        WorkspaceAutomationMethod Method{ WorkspaceAutomationMethod::List };
        std::optional<winrt::guid> WorkspaceId;
        std::optional<uint32_t> PaneId;
        WorkspaceAutomationSplitDirection SplitDirection{ WorkspaceAutomationSplitDirection::Automatic };
        float SplitSize{ 0.5f };
        winrt::hstring Text;
        winrt::hstring Title;
        winrt::hstring Body;
        WorkspaceAutomationProgressState ProgressState{ WorkspaceAutomationProgressState::None };
        uint32_t ProgressValue{ 0 };
        uint32_t MaximumLines{ 50 };
        uint32_t MaximumCharacters{ 8192 };
    };

    struct WorkspaceAutomationContentResult
    {
        winrt::hstring Text;
        bool Truncated{ false };
    };

    struct WorkspaceAutomationParseResult
    {
        std::optional<WorkspaceAutomationRequest> Request;
        std::string Id;
        std::string ErrorCode;
        std::string ErrorMessage;

        explicit operator bool() const noexcept
        {
            return Request.has_value();
        }
    };

    class WorkspaceAutomationProtocol final
    {
    public:
        static constexpr uint32_t Version{ 1 };
        static constexpr size_t MaximumMessageBytes{ 64 * 1024 };
        static constexpr uint32_t MaximumInputCharacters{ 4096 };
        static constexpr uint32_t MaximumReadLines{ 200 };
        static constexpr uint32_t MaximumReadCharacters{ 8192 };

        static WorkspaceAutomationParseResult ParseRequest(std::string_view request);
        static WorkspaceAutomationContentResult TailContent(std::wstring_view content, uint32_t maximumLines, uint32_t maximumCharacters);
        static std::string MakeResult(std::string_view id, const winrt::Windows::Data::Json::JsonObject& result);
        static std::string MakeError(std::string_view id, std::string_view code, std::string_view message);
    };

    class WorkspaceAutomationServer final
    {
    public:
        using RequestHandler = std::function<std::string(std::string_view)>;

        WorkspaceAutomationServer(std::wstring pipeName,
                                  winrt::Windows::System::DispatcherQueue dispatcher,
                                  RequestHandler handler);
        ~WorkspaceAutomationServer();

        WorkspaceAutomationServer(const WorkspaceAutomationServer&) = delete;
        WorkspaceAutomationServer& operator=(const WorkspaceAutomationServer&) = delete;

        bool Start();
        void Stop() noexcept;
        std::wstring_view PipeName() const noexcept;

        static std::wstring MakePipeName(uint32_t processId, uint64_t windowId);

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
