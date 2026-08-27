// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceAutomationController.h"

#include "AgentHistoryStore.h"
#include "TerminalPage.h"
#include "WorkspaceAutomation.h"

#include "../../types/inc/utils.hpp"

using namespace winrt;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;

namespace winrt::TerminalApp::implementation
{
    WorkspaceAutomationController::WorkspaceAutomationController(TerminalPage& page) noexcept :
        _page{ page }
    {
    }

    WorkspaceAutomationController::~WorkspaceAutomationController()
    {
        Stop();
    }

    void WorkspaceAutomationController::Update(const bool enabled)
    {
        assert(_page.Dispatcher().HasThreadAccess());
        _enabled = enabled;
        if (!enabled)
        {
            Stop();
            return;
        }
        if (_server)
        {
            return;
        }

        const auto pipeName = WorkspaceAutomationServer::MakePipeName(GetCurrentProcessId(), _page._WindowProperties.WindowId());
        auto server = std::make_unique<WorkspaceAutomationServer>(
            pipeName,
            DispatcherQueue::GetForCurrentThread(),
            [this](const std::string_view request) {
                if (_enabled)
                {
                    return _HandleRequest(request);
                }
                return WorkspaceAutomationProtocol::MakeError({}, "server_unavailable", "Workspace automation is not available");
            });
        _server = std::move(server);
        if (!_server->Start())
        {
            _server.reset();
        }
    }

    void WorkspaceAutomationController::Stop() noexcept
    {
        _enabled = false;
        if (_server)
        {
            _server->Stop();
            _server.reset();
        }
        _notificationTimes.clear();
    }

    std::wstring_view WorkspaceAutomationController::PipeName() const noexcept
    {
        return _server ? _server->PipeName() : std::wstring_view{};
    }
    std::string WorkspaceAutomationController::_HandleRequest(const std::string_view message)
    {
        const auto _workspaceItems = _page._WorkspaceItems();
        auto& _WindowProperties = _page._WindowProperties;
        auto& _settingsTab = _page._settingsTab;
        auto& _tabContent = _page._tabContent;
        auto& _conclusionSidebar = _page._conclusionSidebar;
        const auto _currentWindowSettings = [&]() { return _page._currentWindowSettings(); };
        const auto _GetTabImpl = [&](const TerminalApp::Tab& tab) { return TerminalPage::_GetTabImpl(tab); };
        const auto _SelectTab = [&](const uint32_t index) { return _page._SelectTab(index); };
        const auto _ReserveWorkspaceIdForNewTab = [&]() { return _page._ReserveWorkspaceIdForNewTab(); };
        const auto _MakePane = [&](auto&&... args) { return _page._MakePane(std::forward<decltype(args)>(args)...); };
        const auto _CreateNewTabFromPane = [&](auto&&... args) { return _page._CreateNewTabFromPane(std::forward<decltype(args)>(args)...); };
        const auto _SplitPane = [&](auto&&... args) { return _page._SplitPane(std::forward<decltype(args)>(args)...); };
        const auto _SendDesktopNotification = [&](auto&&... args) { return _page._SendDesktopNotification(std::forward<decltype(args)>(args)...); };
        const auto _RefreshWorkspaceNotifications = [&]() { return _page._RefreshWorkspaceNotifications(); };
        const auto _MarkWorkspaceNotificationsRead = [&](const auto& item) { return _page._MarkWorkspaceNotificationsRead(item); };
        const auto _ClearWorkspaceNotificationHistory = [&](const auto& item) { return _page._ClearWorkspaceNotificationHistory(item); };

        assert(_page.Dispatcher().HasThreadAccess());

        const auto parsed = WorkspaceAutomationProtocol::ParseRequest(message);
        if (!parsed)
        {
            return WorkspaceAutomationProtocol::MakeError(parsed.Id, parsed.ErrorCode, parsed.ErrorMessage);
        }

        const auto& request = parsed.Request.value();
        const auto workspaceJson = [&](const TerminalApp::WorkspaceItemViewModel& item, const uint32_t index) {
            JsonObject workspace;
            workspace.Insert(L"id", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(item.WorkspaceId())));
            workspace.Insert(L"index", JsonValue::CreateNumberValue(index));
            workspace.Insert(L"title", JsonValue::CreateStringValue(item.Title()));
            workspace.Insert(L"selected", JsonValue::CreateBooleanValue(item.IsSelected()));
            workspace.Insert(L"pinned", JsonValue::CreateBooleanValue(item.IsPinned()));
            workspace.Insert(L"workingDirectory", JsonValue::CreateStringValue(item.WorkingDirectory()));
            workspace.Insert(L"gitBranch", JsonValue::CreateStringValue(item.GitBranch()));
            JsonArray listeningPorts;
            for (const auto port : winrt::get_self<WorkspaceItemViewModel>(item)->ListeningPorts())
            {
                listeningPorts.Append(JsonValue::CreateNumberValue(port));
            }
            workspace.Insert(L"listeningPorts", listeningPorts);

            JsonArray terminalSessionIds;
            if (const auto tabImpl = _GetTabImpl(item.SourceTab()); tabImpl && tabImpl->GetRootPane())
            {
                tabImpl->GetRootPane()->WalkTree([&](const auto& pane) {
                    if (const auto control = pane->GetTerminalControl())
                    {
                        if (const auto connection = control.Connection())
                        {
                            if (const auto sessionId = connection.SessionId(); sessionId != winrt::guid{})
                            {
                                terminalSessionIds.Append(JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(sessionId)));
                            }
                        }
                    }
                });
            }
            workspace.Insert(L"terminalSessionIds", terminalSessionIds);
            return workspace;
        };
        const auto findWorkspace = [&](const winrt::guid& id) -> std::optional<uint32_t> {
            for (uint32_t index = 0; index < _workspaceItems.Size(); ++index)
            {
                if (_workspaceItems.GetAt(index).WorkspaceId() == id)
                {
                    return index;
                }
            }
            return std::nullopt;
        };
        const auto paneJson = [](const std::shared_ptr<Pane>& pane, const bool active) {
            JsonObject value;
            value.Insert(L"id", JsonValue::CreateNumberValue(pane->Id().value()));
            value.Insert(L"active", JsonValue::CreateBooleanValue(active));
            value.Insert(L"connectionClosed", JsonValue::CreateBooleanValue(pane->IsConnectionClosed()));

            if (const auto content = pane->GetContent())
            {
                value.Insert(L"title", JsonValue::CreateStringValue(content.Title()));
                value.Insert(L"readOnly", JsonValue::CreateBooleanValue(content.ReadOnly()));
            }
            if (const auto profile = pane->GetProfile())
            {
                value.Insert(L"profile", JsonValue::CreateStringValue(profile.Name()));
            }
            if (const auto control = pane->GetTerminalControl())
            {
                value.Insert(L"workingDirectory", JsonValue::CreateStringValue(control.WorkingDirectory()));
            }
            return value;
        };
        const auto progressStateName = [](const uint64_t state) -> winrt::hstring {
            switch (state)
            {
            case 1:
                return L"normal";
            case 2:
                return L"error";
            case 3:
                return L"indeterminate";
            case 4:
                return L"paused";
            default:
                return L"none";
            }
        };
        const auto notificationJson = [](const TerminalApp::WorkspaceNotification& notification) {
            constexpr int64_t UnixEpochInWindowsTicks{ 116'444'736'000'000'000 };
            constexpr int64_t TicksPerMillisecond{ 10'000 };
            const auto timestampMilliseconds = (notification.Timestamp().time_since_epoch().count() - UnixEpochInWindowsTicks) / TicksPerMillisecond;

            JsonObject value;
            value.Insert(L"id", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(notification.NotificationId())));
            value.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(notification.WorkspaceId())));
            value.Insert(L"paneId", JsonValue::CreateNumberValue(notification.PaneId()));
            value.Insert(L"title", JsonValue::CreateStringValue(notification.Title()));
            value.Insert(L"body", JsonValue::CreateStringValue(notification.Message()));
            value.Insert(L"timestampMs", JsonValue::CreateNumberValue(static_cast<double>(timestampMilliseconds)));
            value.Insert(L"read", JsonValue::CreateBooleanValue(notification.IsRead()));
            return value;
        };
        const auto statusJson = [&](const TerminalApp::WorkspaceItemViewModel& item) {
            const auto itemImpl = winrt::get_self<WorkspaceItemViewModel>(item);
            const auto terminalStatus = item.SourceTab() ? item.SourceTab().TabStatus() : nullptr;

            JsonObject automationProgress;
            automationProgress.Insert(L"state", JsonValue::CreateStringValue(progressStateName(itemImpl->AutomationProgressState())));
            automationProgress.Insert(L"value", JsonValue::CreateNumberValue(itemImpl->AutomationProgressValue()));

            JsonObject automation;
            automation.Insert(L"text", JsonValue::CreateStringValue(itemImpl->AutomationStatus()));
            automation.Insert(L"progress", automationProgress);

            JsonObject terminal;
            terminal.Insert(L"connectionClosed", JsonValue::CreateBooleanValue(terminalStatus && terminalStatus.IsConnectionClosed()));
            terminal.Insert(L"readOnly", JsonValue::CreateBooleanValue(terminalStatus && terminalStatus.IsReadOnlyActive()));
            terminal.Insert(L"inputBroadcast", JsonValue::CreateBooleanValue(terminalStatus && terminalStatus.IsInputBroadcastActive()));
            terminal.Insert(L"progressState", JsonValue::CreateStringValue(progressStateName(terminalStatus ? terminalStatus.ProgressState() : 0)));
            terminal.Insert(L"progressValue", JsonValue::CreateNumberValue(terminalStatus ? terminalStatus.ProgressValue() : 0));

            JsonObject value;
            value.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(item.WorkspaceId())));
            value.Insert(L"displayText", JsonValue::CreateStringValue(item.StatusText()));
            value.Insert(L"unreadCount", JsonValue::CreateNumberValue(item.UnreadCount()));
            value.Insert(L"automation", automation);
            value.Insert(L"terminal", terminal);
            return value;
        };

        switch (request.Method)
        {
        case WorkspaceAutomationMethod::List:
        {
            JsonArray workspaces;
            for (uint32_t index = 0; index < _workspaceItems.Size(); ++index)
            {
                workspaces.Append(workspaceJson(_workspaceItems.GetAt(index), index));
            }

            JsonObject result;
            result.Insert(L"windowId", JsonValue::CreateStringValue(winrt::to_hstring(_WindowProperties.WindowId())));
            result.Insert(L"pipe", JsonValue::CreateStringValue(_server ? _server->PipeName() : std::wstring_view{}));
            JsonObject permissions;
            permissions.Insert(L"sendInput", JsonValue::CreateBooleanValue(_currentWindowSettings().WorkspaceAutomationAllowInput()));
            permissions.Insert(L"readContent", JsonValue::CreateBooleanValue(_currentWindowSettings().WorkspaceAutomationAllowContentRead()));
            result.Insert(L"permissions", permissions);
            result.Insert(L"workspaces", workspaces);
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::Select:
        {
            const auto index = findWorkspace(request.WorkspaceId.value());
            if (!index)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }
            _SelectTab(index.value());

            JsonObject result;
            result.Insert(L"workspace", workspaceJson(_workspaceItems.GetAt(index.value()), index.value()));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::Create:
        {
            std::unordered_set<winrt::guid> existingIds;
            for (const auto& item : _workspaceItems)
            {
                existingIds.emplace(item.WorkspaceId());
            }

            const auto workspaceId = _ReserveWorkspaceIdForNewTab();
            const auto newPane = _MakePane(NewTerminalArgs(), nullptr, nullptr, false, workspaceId);
            if (!newPane)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "operation_failed", "Unable to create a workspace");
            }
            _CreateNewTabFromPane(newPane, -1, workspaceId);

            for (uint32_t index = 0; index < _workspaceItems.Size(); ++index)
            {
                const auto item = _workspaceItems.GetAt(index);
                if (!existingIds.contains(item.WorkspaceId()))
                {
                    JsonObject result;
                    result.Insert(L"workspace", workspaceJson(item, index));
                    return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
                }
            }
            return WorkspaceAutomationProtocol::MakeError(request.Id, "operation_failed", "Workspace creation did not add a local workspace");
        }
        case WorkspaceAutomationMethod::Close:
        {
            const auto index = findWorkspace(request.WorkspaceId.value());
            if (!index)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }

            const auto tab = _workspaceItems.GetAt(index.value()).SourceTab();
            const auto closeQueued = DispatcherQueue::GetForCurrentThread().TryEnqueue([weakThis{ _page.get_weak() }, tab]() {
                if (const auto page = weakThis.get())
                {
                    page->_HandleCloseTabRequested(tab);
                }
            });
            if (!closeQueued)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "server_unavailable", "Unable to queue the close request");
            }

            JsonObject result;
            result.Insert(L"accepted", JsonValue::CreateBooleanValue(true));
            result.Insert(L"id", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(request.WorkspaceId.value())));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::PaneList:
        {
            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }

            const auto tab = _workspaceItems.GetAt(workspaceIndex.value()).SourceTab();
            const auto tabImpl = _GetTabImpl(tab);
            if (!tabImpl || !tabImpl->GetRootPane())
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_not_supported", "Workspace does not expose a Pane tree");
            }

            const auto activePane = tabImpl->GetActivePane();
            JsonArray panes;
            tabImpl->GetRootPane()->WalkTree([&](const auto& pane) {
                if (pane->Id())
                {
                    panes.Append(paneJson(pane, pane == activePane));
                }
            });

            JsonObject result;
            result.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(request.WorkspaceId.value())));
            result.Insert(L"panes", panes);
            if (activePane && activePane->Id())
            {
                result.Insert(L"activePaneId", JsonValue::CreateNumberValue(activePane->Id().value()));
            }
            else
            {
                result.Insert(L"activePaneId", JsonValue::CreateNullValue());
            }
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::PaneFocus:
        {
            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }

            const auto tab = _workspaceItems.GetAt(workspaceIndex.value()).SourceTab();
            const auto tabImpl = _GetTabImpl(tab);
            const auto pane = tabImpl && tabImpl->GetRootPane() ? tabImpl->GetRootPane()->FindPane(request.PaneId.value()) : nullptr;
            if (!pane)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_not_found", "Pane was not found in the requested workspace");
            }

            _SelectTab(workspaceIndex.value());
            if (!tabImpl->FocusPane(request.PaneId.value()))
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "operation_failed", "Unable to focus Pane");
            }

            JsonObject result;
            result.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(request.WorkspaceId.value())));
            result.Insert(L"pane", paneJson(pane, true));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::PaneSplit:
        {
            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }

            const auto tab = _workspaceItems.GetAt(workspaceIndex.value()).SourceTab();
            const auto tabImpl = _GetTabImpl(tab);
            const auto rootPane = tabImpl ? tabImpl->GetRootPane() : nullptr;
            const auto targetPane = rootPane ? rootPane->FindPane(request.PaneId.value()) : nullptr;
            if (!targetPane)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_not_found", "Pane was not found in the requested workspace");
            }
            if (*tabImpl == _settingsTab)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_not_supported", "Settings workspaces cannot be split");
            }
            if (!targetPane->GetTerminalControl())
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_not_supported", "Only terminal Panes can be split through automation");
            }

            _SelectTab(workspaceIndex.value());
            if (!tabImpl->FocusPane(request.PaneId.value()))
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "operation_failed", "Unable to focus the target Pane");
            }

            SplitDirection direction{ SplitDirection::Automatic };
            switch (request.SplitDirection)
            {
            case WorkspaceAutomationSplitDirection::Up:
                direction = SplitDirection::Up;
                break;
            case WorkspaceAutomationSplitDirection::Right:
                direction = SplitDirection::Right;
                break;
            case WorkspaceAutomationSplitDirection::Down:
                direction = SplitDirection::Down;
                break;
            case WorkspaceAutomationSplitDirection::Left:
                direction = SplitDirection::Left;
                break;
            case WorkspaceAutomationSplitDirection::Automatic:
                break;
            }

            const Windows::Foundation::Size availableSpace{
                static_cast<float>(_tabContent.ActualWidth()),
                static_cast<float>(_tabContent.ActualHeight()),
            };
            const auto realDirection = tabImpl->PreCalculateCanSplit(direction, request.SplitSize, availableSpace);
            if (!realDirection)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "cannot_split", "Target Pane does not have enough space for this split");
            }

            std::unordered_set<uint32_t> existingPaneIds;
            rootPane->WalkTree([&](const auto& pane) {
                if (const auto paneId = pane->Id())
                {
                    existingPaneIds.emplace(paneId.value());
                }
            });

            const auto workspaceId = request.WorkspaceId.value();
            const auto newPane = _MakePane(nullptr, *tabImpl, nullptr, false, workspaceId);
            if (!newPane)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "operation_failed", "Unable to duplicate the target Pane");
            }
            _SplitPane(tabImpl, realDirection.value(), request.SplitSize, newPane, workspaceId);

            std::shared_ptr<Pane> createdPane;
            rootPane->WalkTree([&](const auto& pane) -> bool {
                if (const auto paneId = pane->Id(); paneId && !existingPaneIds.contains(paneId.value()))
                {
                    createdPane = pane;
                    return true;
                }
                return false;
            });
            if (!createdPane)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "operation_failed", "Pane split did not create a local Pane");
            }

            JsonObject result;
            result.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(request.WorkspaceId.value())));
            result.Insert(L"pane", paneJson(createdPane, true));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::ConclusionCreate:
        {
            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }
            if (!_conclusionSidebar)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "operation_failed", "Conclusion sidebar is not available");
            }

            const auto item = _workspaceItems.GetAt(workspaceIndex.value());
            const auto stored = winrt::get_self<implementation::ConclusionSidebar>(_conclusionSidebar)->AddConclusion(
                item.WorkspaceId(),
                item.Title(),
                L"codex",
                request.SessionId,
                request.TurnId,
                item.WorkingDirectory(),
                request.Title,
                request.Prompt,
                request.Body,
                request.Model,
                winrt::clock::now());
            if (!stored)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "persistence_failed", "Unable to commit the AI history turn");
            }

            JsonObject result;
            result.Insert(L"accepted", JsonValue::CreateBooleanValue(true));
            result.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(item.WorkspaceId())));
            result.Insert(L"sessionId", JsonValue::CreateStringValue(request.SessionId));
            result.Insert(L"turnId", JsonValue::CreateStringValue(request.TurnId));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::HistoryIngest:
        {
            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }
            if (!_conclusionSidebar)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "operation_failed", "History sidebar is not available");
            }

            const auto item = _workspaceItems.GetAt(workspaceIndex.value());
            const auto eventTimestamp = winrt::clock::from_sys(
                std::chrono::system_clock::time_point{ std::chrono::milliseconds{ request.EventAt.value() } });
            const auto cwd = request.Cwd.empty() ? item.WorkingDirectory() : request.Cwd;
            const AgentHistoryEventReceipt event{
                .EventId = request.EventId.value(),
                .Cwd = request.Cwd,
                .Title = request.Title,
                .Prompt = request.Prompt,
                .FinalMarkdown = request.Body,
                .Model = request.Model,
                .EventAt = request.EventAt,
            };
            const auto sidebar = winrt::get_self<implementation::ConclusionSidebar>(_conclusionSidebar);
            const auto stored = request.HistoryEvent == AgentHistoryEventKind::Prompt ?
                                    sidebar->AddPrompt(item.WorkspaceId(),
                                                       item.Title(),
                                                       L"codex",
                                                       request.SessionId,
                                                       request.TurnId,
                                                       cwd,
                                                       request.Prompt,
                                                       request.Model,
                                                       eventTimestamp,
                                                       event) :
                                    sidebar->AddConclusion(item.WorkspaceId(),
                                                           item.Title(),
                                                           L"codex",
                                                           request.SessionId,
                                                           request.TurnId,
                                                           cwd,
                                                           request.Title,
                                                           request.Prompt,
                                                           request.Body,
                                                           request.Model,
                                                           eventTimestamp,
                                                           event);
            if (!stored)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "persistence_failed", "Unable to commit the AI history event");
            }

            JsonObject result;
            result.Insert(L"accepted", JsonValue::CreateBooleanValue(true));
            result.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(item.WorkspaceId())));
            result.Insert(L"eventId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(request.EventId.value())));
            result.Insert(L"event", JsonValue::CreateStringValue(request.HistoryEvent == AgentHistoryEventKind::Prompt ? L"prompt" : L"final"));
            result.Insert(L"sessionId", JsonValue::CreateStringValue(request.SessionId));
            result.Insert(L"turnId", JsonValue::CreateStringValue(request.TurnId));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::NotificationList:
        {
            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }

            const auto item = _workspaceItems.GetAt(workspaceIndex.value());
            const auto notifications = item.Notifications();
            JsonArray history;
            for (auto index = notifications.Size(); index > 0; --index)
            {
                history.Append(notificationJson(notifications.GetAt(index - 1)));
            }

            JsonObject result;
            result.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(item.WorkspaceId())));
            result.Insert(L"unreadCount", JsonValue::CreateNumberValue(item.UnreadCount()));
            result.Insert(L"notifications", history);
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::NotificationCreate:
        {
            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }

            const auto item = _workspaceItems.GetAt(workspaceIndex.value());
            const auto tabImpl = _GetTabImpl(item.SourceTab());
            const auto pane = tabImpl && tabImpl->GetRootPane() ? tabImpl->GetRootPane()->FindPane(request.PaneId.value()) : nullptr;
            const auto content = pane ? pane->GetContent() : nullptr;
            if (!pane || !content)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_not_found", "Pane was not found in the requested workspace");
            }

            const auto now = std::chrono::steady_clock::now();
            while (!_notificationTimes.empty() &&
                   now - _notificationTimes.front() >= std::chrono::seconds{ 10 })
            {
                _notificationTimes.pop_front();
            }
            if (_notificationTimes.size() >= 5)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "rate_limited", "At most five automation notifications are allowed per 10 seconds");
            }
            _notificationTimes.emplace_back(now);

            const auto notifications = item.Notifications();
            const auto previousId = notifications.Size() > 0 ? notifications.GetAt(notifications.Size() - 1).NotificationId() : winrt::guid{};
            _SendDesktopNotification(request.Title, request.Body, tabImpl, content);

            JsonObject result;
            if (notifications.Size() > 0)
            {
                const auto latest = notifications.GetAt(notifications.Size() - 1);
                if (latest.NotificationId() != previousId)
                {
                    result.Insert(L"suppressed", JsonValue::CreateBooleanValue(false));
                    result.Insert(L"notification", notificationJson(latest));
                    return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
                }
            }
            result.Insert(L"suppressed", JsonValue::CreateBooleanValue(true));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::NotificationMarkRead:
        {
            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }

            const auto item = _workspaceItems.GetAt(workspaceIndex.value());
            if (request.PaneId)
            {
                item.MarkNotificationsReadForPane(request.PaneId.value());
                if (const auto tabImpl = _GetTabImpl(item.SourceTab()); tabImpl && tabImpl->GetRootPane())
                {
                    if (const auto pane = tabImpl->GetRootPane()->FindPane(request.PaneId.value()))
                    {
                        pane->SetAttention(false);
                    }
                }
                _RefreshWorkspaceNotifications();
            }
            else
            {
                _MarkWorkspaceNotificationsRead(item);
            }

            JsonObject result;
            result.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(item.WorkspaceId())));
            result.Insert(L"unreadCount", JsonValue::CreateNumberValue(item.UnreadCount()));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::NotificationClear:
        {
            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }

            const auto item = _workspaceItems.GetAt(workspaceIndex.value());
            _ClearWorkspaceNotificationHistory(item);

            JsonObject result;
            result.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(item.WorkspaceId())));
            result.Insert(L"cleared", JsonValue::CreateBooleanValue(true));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::StatusGet:
        case WorkspaceAutomationMethod::StatusSet:
        case WorkspaceAutomationMethod::StatusClear:
        case WorkspaceAutomationMethod::ProgressSet:
        case WorkspaceAutomationMethod::ProgressClear:
        {
            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }

            const auto item = _workspaceItems.GetAt(workspaceIndex.value());
            const auto itemImpl = winrt::get_self<WorkspaceItemViewModel>(item);
            switch (request.Method)
            {
            case WorkspaceAutomationMethod::StatusSet:
                itemImpl->SetAutomationStatus(request.Text);
                break;
            case WorkspaceAutomationMethod::StatusClear:
                itemImpl->ClearAutomationStatus();
                break;
            case WorkspaceAutomationMethod::ProgressSet:
                itemImpl->SetAutomationProgress(static_cast<uint64_t>(request.ProgressState), request.ProgressValue);
                break;
            case WorkspaceAutomationMethod::ProgressClear:
                itemImpl->ClearAutomationProgress();
                break;
            default:
                break;
            }

            JsonObject result;
            result.Insert(L"status", statusJson(item));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::TerminalSendInput:
        {
            if (!_currentWindowSettings().WorkspaceAutomationAllowInput())
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "permission_denied", "Enable experimental.workspaceAutomation.allowInput to send terminal input");
            }

            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }
            const auto item = _workspaceItems.GetAt(workspaceIndex.value());
            const auto tabImpl = _GetTabImpl(item.SourceTab());
            const auto pane = tabImpl && tabImpl->GetRootPane() ? tabImpl->GetRootPane()->FindPane(request.PaneId.value()) : nullptr;
            if (!pane)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_not_found", "Pane was not found in the requested workspace");
            }
            const auto control = pane->GetTerminalControl();
            if (!control)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_not_supported", "Only terminal panes accept input");
            }
            if (pane->IsConnectionClosed())
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "connection_closed", "The target terminal connection is closed");
            }
            if (control.ReadOnly())
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_read_only", "The target terminal pane is read-only");
            }

            // Send directly to the requested control. The input-broadcast path could
            // affect panes outside the explicit Workspace/Pane target.
            control.SendInput(request.Text);

            JsonObject result;
            result.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(item.WorkspaceId())));
            result.Insert(L"paneId", JsonValue::CreateNumberValue(request.PaneId.value()));
            result.Insert(L"accepted", JsonValue::CreateBooleanValue(true));
            result.Insert(L"characters", JsonValue::CreateNumberValue(request.Text.size()));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        case WorkspaceAutomationMethod::TerminalReadContent:
        {
            if (!_currentWindowSettings().WorkspaceAutomationAllowContentRead())
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "permission_denied", "Enable experimental.workspaceAutomation.allowContentRead to read terminal content");
            }

            const auto workspaceIndex = findWorkspace(request.WorkspaceId.value());
            if (!workspaceIndex)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "workspace_not_found", "Workspace was not found in this window");
            }
            const auto item = _workspaceItems.GetAt(workspaceIndex.value());
            const auto tabImpl = _GetTabImpl(item.SourceTab());
            const auto pane = tabImpl && tabImpl->GetRootPane() ? tabImpl->GetRootPane()->FindPane(request.PaneId.value()) : nullptr;
            if (!pane)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_not_found", "Pane was not found in the requested workspace");
            }
            const auto control = pane->GetTerminalControl();
            if (!control)
            {
                return WorkspaceAutomationProtocol::MakeError(request.Id, "pane_not_supported", "Only terminal panes expose terminal content");
            }

            const auto content = WorkspaceAutomationProtocol::TailContent(control.ReadEntireBuffer(), request.MaximumLines, request.MaximumCharacters);
            JsonObject result;
            result.Insert(L"workspaceId", JsonValue::CreateStringValue(::Microsoft::Console::Utils::GuidToPlainString(item.WorkspaceId())));
            result.Insert(L"paneId", JsonValue::CreateNumberValue(request.PaneId.value()));
            result.Insert(L"text", JsonValue::CreateStringValue(content.Text));
            result.Insert(L"truncated", JsonValue::CreateBooleanValue(content.Truncated));
            return WorkspaceAutomationProtocol::MakeResult(request.Id, result);
        }
        }

        return WorkspaceAutomationProtocol::MakeError(request.Id, "method_not_found", "Unknown automation method");
    }

}
