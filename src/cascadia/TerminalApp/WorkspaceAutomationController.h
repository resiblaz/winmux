// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

namespace winrt::TerminalApp::implementation
{
    struct TerminalPage;
    class WorkspaceAutomationServer;

    // Owns the per-window automation endpoint and dispatches all protocol
    // methods. This controller is UI-thread affine; blocking transport work is
    // performed by WorkspaceAutomationServer before dispatching here.
    class WorkspaceAutomationController final
    {
    public:
        explicit WorkspaceAutomationController(TerminalPage& page) noexcept;
        ~WorkspaceAutomationController();

        WorkspaceAutomationController(const WorkspaceAutomationController&) = delete;
        WorkspaceAutomationController& operator=(const WorkspaceAutomationController&) = delete;

        void Update(bool enabled);
        void Stop() noexcept;
        std::wstring_view PipeName() const noexcept;

    private:
        std::string _HandleRequest(std::string_view request);

        TerminalPage& _page;
        std::unique_ptr<WorkspaceAutomationServer> _server;
        std::deque<std::chrono::steady_clock::time_point> _notificationTimes;
        bool _enabled{};
    };
}
