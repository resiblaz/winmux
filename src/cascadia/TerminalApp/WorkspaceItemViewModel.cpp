// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceItemViewModel.h"
#include "WorkspaceGitBranch.h"

#include "WorkspaceItemViewModel.g.cpp"

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        winrt::hstring _compactWorkingDirectory(const winrt::hstring& workingDirectory)
        {
            const std::wstring_view path{ workingDirectory };
            std::vector<std::wstring_view> components;
            for (size_t index = 0; index < path.size();)
            {
                while (index < path.size() && (path[index] == L'\\' || path[index] == L'/'))
                {
                    ++index;
                }

                const auto start = index;
                while (index < path.size() && path[index] != L'\\' && path[index] != L'/')
                {
                    ++index;
                }

                if (index > start)
                {
                    const auto component = path.substr(start, index - start);
                    if (!(components.empty() && component.size() == 2 && component[1] == L':'))
                    {
                        components.emplace_back(component);
                    }
                }
            }

            if (components.empty())
            {
                return workingDirectory;
            }

            const auto first = components.size() > 1 ? components.size() - 2 : 0;
            std::wstring compact{ components[first] };
            for (auto index = first + 1; index < components.size(); ++index)
            {
                compact.push_back(L'\\');
                compact.append(components[index]);
            }
            return winrt::hstring{ compact };
        }

        winrt::hstring _formatPorts(const std::span<const uint16_t> ports, const size_t maximumCount, const bool showOverflow)
        {
            std::wstring text;
            const auto count = std::min(ports.size(), maximumCount);
            for (size_t index = 0; index < count; ++index)
            {
                if (!text.empty())
                {
                    text.append(L" · ");
                }
                text.append(std::to_wstring(ports[index]));
            }
            if (showOverflow && ports.size() > count)
            {
                text.append(L" · +");
                text.append(std::to_wstring(ports.size() - count));
            }
            return winrt::hstring{ text };
        }
    }

    WorkspaceItemViewModel::WorkspaceItemViewModel(const TerminalApp::Tab& tab) :
        _tab{ tab },
        _notifications{ winrt::single_threaded_observable_vector<TerminalApp::WorkspaceNotification>() },
        _dispatcher{ Windows::System::DispatcherQueue::GetForCurrentThread() }
    {
        GUID id{};
        THROW_IF_FAILED(CoCreateGuid(&id));
        _workspaceId = id;

        if (_tab)
        {
            _tabChangedRevoker = _tab.PropertyChanged(winrt::auto_revoke, [weakThis{ get_weak() }](const auto&, const auto& args) {
                if (const auto self = weakThis.get())
                {
                    if (args.PropertyName() == L"WorkingDirectory")
                    {
                        self->_UpdateWorkingDirectory();
                    }
                    else
                    {
                        self->PropertyChanged.raise(*self, args);
                        const auto propertyName = args.PropertyName();
                        if (propertyName == L"Title" || propertyName == L"IsPinned" || propertyName == L"SwitchShortcutText")
                        {
                            self->PropertyChanged.raise(*self, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"AutomationName" });
                        }
                        if (propertyName == L"SwitchShortcutText")
                        {
                            self->PropertyChanged.raise(*self, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SwitchShortcutHintText" });
                        }
                        if (propertyName == L"IsPinned")
                        {
                            self->PropertyChanged.raise(*self, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"PinAutomationName" });
                        }
                    }
                }
            });

            if (const auto status = _tab.TabStatus())
            {
                _tabStatusChangedRevoker = status.PropertyChanged(winrt::auto_revoke, [weakThis{ get_weak() }](const auto&, const auto&) {
                    if (const auto self = weakThis.get())
                    {
                        self->_UpdateStatus();
                    }
                });
            }
        }

        _UpdateWorkingDirectory(false);
        _UpdateStatus();
    }

    winrt::guid WorkspaceItemViewModel::WorkspaceId() const noexcept
    {
        return _workspaceId;
    }

    TerminalApp::Tab WorkspaceItemViewModel::SourceTab() const noexcept
    {
        return _tab;
    }

    bool WorkspaceItemViewModel::IsSelected() const noexcept
    {
        return _isSelected;
    }

    void WorkspaceItemViewModel::IsSelected(const bool value)
    {
        if (_isSelected != value)
        {
            _isSelected = value;
            PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IsSelected" });
            if (value)
            {
                RefreshGitBranch();
            }
        }
    }

    winrt::hstring WorkspaceItemViewModel::Title() const
    {
        return _tab ? _tab.Title() : winrt::hstring{};
    }

    winrt::hstring WorkspaceItemViewModel::AutomationName() const
    {
        std::wstring value{ Title() };
        const auto append = [&](const winrt::hstring& detail) {
            if (!detail.empty())
            {
                if (!value.empty())
                {
                    value.append(L", ");
                }
                value.append(detail);
            }
        };

        if (IsPinned())
        {
            append(RS_(L"WorkspaceSidebar_ItemAutomationPinned"));
        }
        if (const auto directory = WorkingDirectoryDisplay(); !directory.empty())
        {
            append(winrt::hstring{ RS_fmt(L"WorkspaceSidebar_ItemAutomationDirectory", directory) });
        }
        if (const auto branch = GitBranch(); !branch.empty())
        {
            append(winrt::hstring{ RS_fmt(L"WorkspaceSidebar_ItemAutomationGitBranch", branch) });
        }
        append(ListeningPortsTooltip());
        if (const auto status = StatusText(); !status.empty())
        {
            append(winrt::hstring{ RS_fmt(L"WorkspaceSidebar_ItemAutomationStatus", status) });
        }
        if (const auto shortcut = SwitchShortcutText(); !shortcut.empty())
        {
            append(winrt::hstring{ RS_fmt(L"WorkspaceSidebar_ItemAutomationShortcut", shortcut) });
        }
        return winrt::hstring{ value };
    }

    winrt::hstring WorkspaceItemViewModel::Icon() const
    {
        return _tab ? _tab.Icon() : winrt::hstring{};
    }

    bool WorkspaceItemViewModel::IsPinned() const
    {
        return _tab && _tab.IsPinned();
    }

    void WorkspaceItemViewModel::IsPinned(const bool value)
    {
        if (_tab)
        {
            _tab.IsPinned(value);
        }
    }

    winrt::hstring WorkspaceItemViewModel::PinIconGlyph() const
    {
        return _tab ? _tab.PinIconGlyph() : winrt::hstring{};
    }

    winrt::hstring WorkspaceItemViewModel::PinActionText() const
    {
        return IsPinned() ? RS_(L"WorkspaceSidebar_UnpinAction") : RS_(L"WorkspaceSidebar_PinAction");
    }

    winrt::hstring WorkspaceItemViewModel::PinAutomationName() const
    {
        return IsPinned() ? RS_(L"WorkspaceSidebar_UnpinAutomationName") : RS_(L"WorkspaceSidebar_PinAutomationName");
    }

    winrt::hstring WorkspaceItemViewModel::SwitchShortcutText() const
    {
        return _tab ? _tab.SwitchShortcutText() : winrt::hstring{};
    }

    winrt::hstring WorkspaceItemViewModel::SwitchShortcutHintText() const
    {
        if (!_showSwitchShortcutHint)
        {
            return {};
        }

        const std::wstring_view shortcut{ SwitchShortcutText() };
        if (shortcut.empty())
        {
            return {};
        }

        const auto separator = shortcut.find_last_of(L'+');
        return winrt::hstring{ shortcut.substr(separator == std::wstring_view::npos ? 0 : separator + 1) };
    }

    void WorkspaceItemViewModel::ShowSwitchShortcutHint(const bool value)
    {
        if (_showSwitchShortcutHint != value)
        {
            _showSwitchShortcutHint = value;
            PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SwitchShortcutHintText" });
        }
    }

    winrt::hstring WorkspaceItemViewModel::UnreadBadgeText() const
    {
        const auto count = UnreadCount();
        return count == 0 ? winrt::hstring{} : winrt::to_hstring(count);
    }

    winrt::hstring WorkspaceItemViewModel::NotificationAutomationName() const
    {
        const auto count = UnreadCount();
        return count == 0 ? winrt::hstring{} : winrt::hstring{ RS_fmt(L"WorkspaceSidebar_UnreadAutomationName", count) };
    }

    TerminalApp::WorkspaceNotification WorkspaceItemViewModel::LatestUnreadNotification() const
    {
        for (auto index = _notifications.Size(); index > 0; --index)
        {
            const auto notification = _notifications.GetAt(index - 1);
            if (!notification.IsRead())
            {
                return notification;
            }
        }
        return nullptr;
    }

    uint32_t WorkspaceItemViewModel::NotificationHistoryCount() const noexcept
    {
        return _notifications.Size();
    }

    winrt::hstring WorkspaceItemViewModel::NotificationHistoryCountText() const
    {
        const auto count = NotificationHistoryCount();
        return count == 0 ? winrt::hstring{} : winrt::to_hstring(count);
    }

    winrt::hstring WorkspaceItemViewModel::WorkingDirectoryDisplay() const
    {
        return _compactWorkingDirectory(WorkingDirectory());
    }

    winrt::hstring WorkspaceItemViewModel::ListeningPortsDisplay() const
    {
        return _formatPorts(_listeningPorts, MaximumDisplayedListeningPorts, true);
    }

    winrt::hstring WorkspaceItemViewModel::ListeningPortsTooltip() const
    {
        if (_listeningPorts.empty())
        {
            return {};
        }
        return winrt::hstring{ RS_fmt(L"WorkspaceSidebar_ListeningPortsTooltip",
                                      _formatPorts(_listeningPorts, _listeningPorts.size(), false)) };
    }

    void WorkspaceItemViewModel::UpdateListeningPorts(std::vector<uint16_t> ports)
    {
        std::sort(ports.begin(), ports.end());
        ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
        if (_listeningPorts == ports)
        {
            return;
        }

        _listeningPorts = std::move(ports);
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"ListeningPortsDisplay" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"ListeningPortsTooltip" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"AutomationName" });
    }

    const std::vector<uint16_t>& WorkspaceItemViewModel::ListeningPorts() const noexcept
    {
        return _listeningPorts;
    }

    bool WorkspaceItemViewModel::HasAgentResume() const noexcept
    {
        return !_agentResumeProvider.empty() && !_agentResumeSessionId.empty();
    }

    winrt::hstring WorkspaceItemViewModel::AgentResumeProvider() const noexcept
    {
        return _agentResumeProvider;
    }

    winrt::hstring WorkspaceItemViewModel::AgentResumeSessionId() const noexcept
    {
        return _agentResumeSessionId;
    }

    winrt::hstring WorkspaceItemViewModel::AgentResumeWorkingDirectory() const noexcept
    {
        return _agentResumeWorkingDirectory;
    }

    void WorkspaceItemViewModel::ConfigureAgentResume(const winrt::hstring& provider,
                                                      const winrt::hstring& sessionId,
                                                      const winrt::hstring& workingDirectory)
    {
        _agentResumeProvider = provider;
        _agentResumeSessionId = sessionId;
        _agentResumeWorkingDirectory = workingDirectory;
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"AgentResumeProvider" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"AgentResumeSessionId" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"AgentResumeWorkingDirectory" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"HasAgentResume" });
    }

    void WorkspaceItemViewModel::ClearAgentResume()
    {
        ConfigureAgentResume({}, {}, {});
    }

    void WorkspaceItemViewModel::SetAutomationStatus(const winrt::hstring& text)
    {
        _automationStatus = text;
        _UpdateStatus();
    }

    void WorkspaceItemViewModel::ClearAutomationStatus()
    {
        SetAutomationStatus({});
    }

    winrt::hstring WorkspaceItemViewModel::AutomationStatus() const noexcept
    {
        return _automationStatus;
    }

    void WorkspaceItemViewModel::SetAutomationProgress(const uint64_t state, const uint32_t value)
    {
        _automationProgressState = state;
        _automationProgressValue = value;
        _UpdateStatus();
    }

    void WorkspaceItemViewModel::ClearAutomationProgress()
    {
        SetAutomationProgress(0, 0);
    }

    uint64_t WorkspaceItemViewModel::AutomationProgressState() const noexcept
    {
        return _automationProgressState;
    }

    uint32_t WorkspaceItemViewModel::AutomationProgressValue() const noexcept
    {
        return _automationProgressValue;
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceNotification> WorkspaceItemViewModel::Notifications() const noexcept
    {
        return _notifications;
    }

    void WorkspaceItemViewModel::RestoreWorkspaceId(const winrt::guid& workspaceId) noexcept
    {
        if (workspaceId != winrt::guid{})
        {
            _workspaceId = workspaceId;
        }
    }

    void WorkspaceItemViewModel::BindSourceSnapshot(const winrt::guid& snapshotId, const uint64_t revision) noexcept
    {
        if (snapshotId != winrt::guid{} && revision != 0)
        {
            _sourceSnapshotId = snapshotId;
            _sourceSnapshotRevision = revision;
        }
        else
        {
            ClearSourceSnapshot();
        }
    }

    void WorkspaceItemViewModel::ClearSourceSnapshot() noexcept
    {
        _sourceSnapshotId.reset();
        _sourceSnapshotRevision = 0;
    }

    std::optional<winrt::guid> WorkspaceItemViewModel::SourceSnapshotId() const noexcept
    {
        return _sourceSnapshotId;
    }

    uint64_t WorkspaceItemViewModel::SourceSnapshotRevision() const noexcept
    {
        return _sourceSnapshotRevision;
    }

    void WorkspaceItemViewModel::RefreshGitBranch()
    {
        const auto directory = WorkingDirectory();
        const auto generation = ++_gitBranchRequestGeneration;
        if (!WorkspaceGitBranch::IsSupportedDirectory(directory))
        {
            GitBranch(L"");
            GitRepositoryRoot(L"");
            IsGitBranchLoading(false);
            return;
        }

        IsGitBranchLoading(true);
        _RefreshGitBranchAsync(directory, generation);
    }

    safe_void_coroutine WorkspaceItemViewModel::_RefreshGitBranchAsync(winrt::hstring directory, const uint64_t generation)
    {
        const auto weakThis = get_weak();
        const auto dispatcher = _dispatcher;
        co_await winrt::resume_background();
        const auto result = WorkspaceGitBranch::QueryCached(directory, GitQueryTimeout, GitCacheMaximumAge);
        co_await wil::resume_foreground(dispatcher);

        if (const auto self = weakThis.get(); self && self->_gitBranchRequestGeneration == generation && self->WorkingDirectory() == directory)
        {
            const auto branch = result.IsRepository ? result.Branch : winrt::hstring{};
            const auto branchChanged = self->GitBranch() != branch;
            self->GitBranch(branch);
            self->GitRepositoryRoot(result.IsRepository ? result.RepositoryRoot : winrt::hstring{});
            self->IsGitBranchLoading(false);
            if (branchChanged)
            {
                self->PropertyChanged.raise(*self, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"AutomationName" });
            }
        }
    }

    void WorkspaceItemViewModel::AddNotification(const uint32_t paneId, const winrt::hstring& title, const winrt::hstring& message)
    {
        _notifications.Append(TerminalApp::WorkspaceNotification{ _workspaceId, paneId, Title(), title, message, winrt::clock::now() });
        if (_notifications.Size() > MaximumNotifications)
        {
            _notifications.RemoveAt(0);
        }
        _UpdateNotificationProperties();
    }

    void WorkspaceItemViewModel::MarkAllNotificationsRead()
    {
        bool changed = false;
        for (const auto& notification : _notifications)
        {
            if (!notification.IsRead())
            {
                notification.IsRead(true);
                changed = true;
            }
        }

        if (changed)
        {
            _UpdateNotificationProperties();
        }
    }

    void WorkspaceItemViewModel::MarkNotificationsReadForPane(const uint32_t paneId)
    {
        bool changed = false;
        for (const auto& notification : _notifications)
        {
            if (notification.PaneId() == paneId && !notification.IsRead())
            {
                notification.IsRead(true);
                changed = true;
            }
        }

        if (changed)
        {
            _UpdateNotificationProperties();
        }
    }

    void WorkspaceItemViewModel::ClearNotificationHistory()
    {
        if (_notifications.Size() == 0)
        {
            return;
        }

        _notifications.Clear();
        _UpdateNotificationProperties();
    }

    void WorkspaceItemViewModel::_UpdateNotificationProperties()
    {
        uint32_t unreadCount = 0;
        for (const auto& notification : _notifications)
        {
            unreadCount += notification.IsRead() ? 0 : 1;
        }
        UnreadCount(unreadCount);

        const auto latest = LatestUnreadNotification();
        LatestNotification(latest ? (latest.Message().empty() ? latest.Title() : latest.Message()) : winrt::hstring{});

        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"UnreadBadgeText" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"NotificationAutomationName" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"NotificationHistoryCount" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"NotificationHistoryCountText" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"LatestUnreadNotification" });
        _UpdateStatus();
    }

    void WorkspaceItemViewModel::_UpdateWorkingDirectory(const bool refreshGit)
    {
        const auto workingDirectory = _tab ? _tab.WorkingDirectory() : winrt::hstring{};
        if (WorkingDirectory() == workingDirectory)
        {
            if (refreshGit)
            {
                RefreshGitBranch();
            }
            return;
        }

        WorkingDirectory(workingDirectory);
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"WorkingDirectoryDisplay" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"AutomationName" });
        if (refreshGit)
        {
            RefreshGitBranch();
        }
    }

    void WorkspaceItemViewModel::_UpdateStatus()
    {
        winrt::hstring text;
        if (const auto status = _tab ? _tab.TabStatus() : nullptr)
        {
            if (status.IsConnectionClosed())
            {
                text = RS_(L"WorkspaceSidebar_StatusDisconnected");
            }
            else if (UnreadCount() > 0)
            {
                text = RS_(L"WorkspaceSidebar_StatusAttention");
            }
            else if (!_automationStatus.empty())
            {
                text = _automationStatus;
            }
            else if (_automationProgressState != 0)
            {
                switch (_automationProgressState)
                {
                case 1:
                    text = _automationProgressValue > 0 ?
                               winrt::hstring{ RS_fmt(L"WorkspaceSidebar_StatusProgress", _automationProgressValue) } :
                               RS_(L"WorkspaceSidebar_StatusRunning");
                    break;
                case 2:
                    text = RS_(L"WorkspaceSidebar_StatusFailed");
                    break;
                case 3:
                    text = RS_(L"WorkspaceSidebar_StatusRunning");
                    break;
                case 4:
                    text = RS_(L"WorkspaceSidebar_StatusPaused");
                    break;
                }
            }
            else
            {
                switch (status.ProgressState())
                {
                case 1:
                    text = status.ProgressValue() > 0 ?
                               winrt::hstring{ RS_fmt(L"WorkspaceSidebar_StatusProgress", status.ProgressValue()) } :
                               RS_(L"WorkspaceSidebar_StatusRunning");
                    break;
                case 2:
                    text = RS_(L"WorkspaceSidebar_StatusFailed");
                    break;
                case 3:
                    text = RS_(L"WorkspaceSidebar_StatusRunning");
                    break;
                case 4:
                    text = RS_(L"WorkspaceSidebar_StatusPaused");
                    break;
                default:
                    if (status.IsInputBroadcastActive())
                    {
                        text = RS_(L"WorkspaceSidebar_StatusBroadcasting");
                    }
                    else if (status.IsReadOnlyActive())
                    {
                        text = RS_(L"WorkspaceSidebar_StatusReadOnly");
                    }
                    break;
                }
            }
        }
        if (StatusText() != text)
        {
            StatusText(text);
            PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"AutomationName" });
        }
    }
}
