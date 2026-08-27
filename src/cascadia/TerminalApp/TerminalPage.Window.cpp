// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"
#include "WorkspaceFeatureSet.h"

#include <TerminalCore/ControlKeyStates.hpp>
#include <TerminalThemeHelpers.h>
#include <til/hash.h>
#include <til/unicode.h>
#include <Utils.h>

#include "../../types/inc/ColorFix.hpp"
#include "../../types/inc/utils.hpp"
#include "../TerminalSettingsAppAdapterLib/TerminalSettings.h"
#include "App.h"
#include "DebugTapConnection.h"
#include "MarkdownPaneContent.h"
#include "Remoting.h"
#include "ScratchpadContent.h"
#include "SettingsPaneContent.h"
#include "SnippetsPaneContent.h"
#include "TabRowControl.h"
#include "TerminalSettingsCache.h"
#include "WorkspaceListeningPorts.h"

using namespace winrt;
using namespace winrt::Microsoft::Management::Deployment;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Core;
using namespace std::chrono_literals;

#define HOOKUP_ACTION(action) _actionDispatch->action({ this, &TerminalPage::_Handle##action });

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
    using VirtualKeyModifiers = Windows::System::VirtualKeyModifiers;
}

namespace winrt::TerminalApp::implementation
{    safe_void_coroutine TerminalPage::RequestQuit()
    {
        const auto setting = _settings.GlobalSettings().ConfirmOnClose();
        if (setting != ConfirmOnClose::Never && !_displayingCloseDialog)
        {
            _displayingCloseDialog = true;

            const auto weak = get_weak();
            auto warningResult = co_await _ShowConfirmCloseDialog(ConfirmCloseDialogKind::CloseAll);
            const auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }

            _displayingCloseDialog = false;

            if (warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }
        }

        QuitRequested.raise(nullptr, nullptr);
    }

    WindowLayout TerminalPage::GetWindowLayout()
    {
        // This method may be called for a window even if it hasn't had a tab yet or lost all of them.
        // We shouldn't persist such windows.
        const auto tabCount = _tabs.Size();
        if (_startupState != StartupState::Initialized || tabCount == 0)
        {
            return nullptr;
        }

        std::vector<ActionAndArgs> actions;

        for (auto tab : _tabs)
        {
            auto t = winrt::get_self<implementation::Tab>(tab);
            auto tabActions = t->BuildStartupActions(BuildStartupKind::Persist);
            actions.insert(actions.end(), std::make_move_iterator(tabActions.begin()), std::make_move_iterator(tabActions.end()));
        }

        // Avoid persisting a window with zero tabs, because `BuildStartupActions` happened to return an empty vector.
        if (actions.empty())
        {
            return nullptr;
        }

        // if the focused tab was not the last tab, restore that
        auto idx = _GetFocusedTabIndex();
        if (idx && idx != tabCount - 1)
        {
            ActionAndArgs action;
            action.Action(ShortcutAction::SwitchToTab);
            SwitchToTabArgs switchToTabArgs{ idx.value() };
            action.Args(switchToTabArgs);

            actions.emplace_back(std::move(action));
        }

        // If the user set a custom name, save it
        if (const auto& windowName{ _WindowProperties.WindowName() }; !windowName.empty())
        {
            ActionAndArgs action;
            action.Action(ShortcutAction::RenameWindow);
            RenameWindowArgs args{ windowName };
            action.Args(args);

            actions.emplace_back(std::move(action));
        }

        WindowLayout layout;
        layout.TabLayout(winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)));

        auto mode = LaunchMode::DefaultMode;
        WI_SetFlagIf(mode, LaunchMode::FullscreenMode, _isFullscreen);
        WI_SetFlagIf(mode, LaunchMode::FocusMode, _isInFocusMode);
        WI_SetFlagIf(mode, LaunchMode::MaximizedMode, _isMaximized);

        layout.LaunchMode({ mode });

        // Only save the content size because the tab size will be added on load.
        const auto contentWidth = static_cast<float>(_tabContent.ActualWidth());
        const auto contentHeight = static_cast<float>(_tabContent.ActualHeight());
        const winrt::Windows::Foundation::Size windowSize{ contentWidth, contentHeight };

        layout.InitialSize(windowSize);

        // We don't actually know our own position. So we have to ask the window
        // layer for that.
        const auto launchPosRequest{ winrt::make<LaunchPositionRequest>() };
        RequestLaunchPosition.raise(*this, launchPosRequest);
        layout.InitialPosition(launchPosRequest.Position());

        std::vector<winrt::guid> workspaceIds;
        std::vector<uint32_t> pinnedWorkspaceIndices;
        std::vector<AgentResumeInfo> agentResumeEntries;
        std::vector<WorkspaceSnapshotSourceInfo> workspaceSnapshotSources;
        workspaceIds.reserve(tabCount);
        for (uint32_t index = 0; index < tabCount; ++index)
        {
            const auto item = _WorkspaceItems().GetAt(index);
            workspaceIds.emplace_back(item.WorkspaceId());
            if (item.IsPinned())
            {
                pinnedWorkspaceIndices.emplace_back(index);
            }
            if (item.HasAgentResume())
            {
                AgentResumeInfo info{};
                info.Version = WorkspaceAgentResume::CurrentVersion;
                info.WorkspaceId = item.WorkspaceId();
                info.Provider = item.AgentResumeProvider();
                info.SessionId = item.AgentResumeSessionId();
                info.WorkingDirectory = item.AgentResumeWorkingDirectory();
                info.RequiresConfirmation = true;
                if (WorkspaceAgentResume::IsSupported(info))
                {
                    agentResumeEntries.emplace_back(std::move(info));
                }
            }
            const auto itemImpl = winrt::get_self<implementation::WorkspaceItemViewModel>(item);
            if (const auto sourceSnapshotId = itemImpl->SourceSnapshotId();
                sourceSnapshotId && itemImpl->SourceSnapshotRevision() != 0)
            {
                workspaceSnapshotSources.emplace_back(WorkspaceSnapshotSourceInfo{
                    .WorkspaceId = item.WorkspaceId(),
                    .SnapshotId = sourceSnapshotId.value(),
                    .Revision = itemImpl->SourceSnapshotRevision(),
                });
            }
        }
        layout.WorkspaceStateVersion(CurrentWorkspaceStateVersion);
        layout.WorkspaceIds(winrt::single_threaded_vector<winrt::guid>(std::move(workspaceIds)));
        layout.PinnedWorkspaceIndices(winrt::single_threaded_vector<uint32_t>(std::move(pinnedWorkspaceIndices)));
        layout.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>(std::move(agentResumeEntries)));
        layout.WorkspaceSnapshotSources(winrt::single_threaded_vector<WorkspaceSnapshotSourceInfo>(std::move(workspaceSnapshotSources)));

        return layout;
    }

    void TerminalPage::PersistState()
    {
        // There are two persistence mechanisms in play here:
        //   * PersistedWindowLayouts (vector) — consumed on next startup to
        //     re-open a matching set of windows. Cleared after restore.
        //   * PersistedWorkspaces (name-keyed map) — the full tab/buffer
        //     state of a named window, claimed by name on demand via
        //     ApplicationState::TakeWorkspace.
        //
        // For named windows we save the full layout into the workspace map
        // and drop a lightweight `openWorkspace` stub into the generic vector,
        // so the generic restore path re-opens the named window which in
        // turn claims its own workspace. Unnamed windows don't have a stable
        // key, so their full layout is stored directly in the vector.
        if (const auto layout = GetWindowLayout())
        {
            const auto& windowName = _WindowProperties.WindowName();
            if (!windowName.empty())
            {
                // Persist the full layout into the workspace collection.
                ApplicationState::SharedInstance().SaveWorkspace(windowName, layout);

                // Build a minimal layout with just an openWorkspace action
                // so the generic restore path re-opens this workspace by name.
                std::vector<ActionAndArgs> actions;
                ActionAndArgs action;
                action.Action(ShortcutAction::OpenWorkspace);
                OpenWorkspaceArgs args{ windowName };
                action.Args(args);
                actions.emplace_back(std::move(action));

                WindowLayout stub;
                stub.TabLayout(winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)));
                ApplicationState::SharedInstance().AppendPersistedWindowLayout(stub);
            }
            else
            {
                ApplicationState::SharedInstance().AppendPersistedWindowLayout(layout);
            }
        }
    }

    // Method Description:
    // - Determines whether a close-window action should show a confirmation
    //   dialog, based on the confirmOnClose setting and the current window state.
    // Arguments:
    // - <none>
    // Return Value:
    // - true, if a warning dialog should be shown before closing the window
    bool TerminalPage::_ShouldWarnOnClose() const
    {
        const auto setting = _settings.GlobalSettings().ConfirmOnClose();
        switch (setting)
        {
        case ConfirmOnClose::Always:
            return true;
        case ConfirmOnClose::Automatic:
        {
            // Warn if there's more than one tab, or the one tab has more than one pane.
            return _HasMultipleTabs() || _GetTabImpl(_tabs.GetAt(0))->GetLeafPaneCount() > 1;
        }
        case ConfirmOnClose::Never:
        default:
            return false;
        }
    }

    // Method Description:
    // - Determines whether closing a specific tab should show a confirmation
    //   dialog, based on the confirmOnClose setting and the tab's state.
    // Arguments:
    // - tab: The tab being closed
    // Return Value:
    // - true, if a warning dialog should be shown before closing the tab
    bool TerminalPage::_ShouldWarnOnCloseTab(const winrt::com_ptr<Tab>& tab) const
    {
        const auto setting = _settings.GlobalSettings().ConfirmOnClose();
        switch (setting)
        {
        case ConfirmOnClose::Always:
            return true;
        case ConfirmOnClose::Automatic:
            // Warn if this tab has more than one pane.
            return tab->GetLeafPaneCount() > 1;
        case ConfirmOnClose::Never:
        default:
            return false;
        }
    }

    // Method Description:
    // - Close the terminal app. If the confirmOnClose setting indicates we should
    //   warn for the current window state, show a warning dialog.
    safe_void_coroutine TerminalPage::CloseWindow()
    {
        if (_ShouldWarnOnClose() &&
            !_displayingCloseDialog)
        {
            if (_newTabButton && _newTabButton.Flyout())
            {
                _newTabButton.Flyout().Hide();
            }
            _DismissTabContextMenus();
            _displayingCloseDialog = true;

            const auto weak = get_weak();
            auto warningResult = co_await _ShowConfirmCloseDialog(ConfirmCloseDialogKind::Window);
            // Hold a strong reference to `this` after the co_await; we may
            // be the last holder if the window was already being torn down.
            auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }

            _displayingCloseDialog = false;

            if (warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }
        }

        CloseWindowRequested.raise(*this, nullptr);
    }

    std::vector<IPaneContent> TerminalPage::Panes() const
    {
        std::vector<IPaneContent> panes;

        for (const auto tab : _tabs)
        {
            const auto impl = _GetTabImpl(tab);
            if (!impl)
            {
                continue;
            }

            impl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (auto content = pane->GetContent())
                {
                    panes.push_back(std::move(content));
                }
            });
        }

        return panes;
    }

    // Method Description:
    // - Move the viewport of the terminal of the currently focused tab up or
    //      down a number of lines.
    // Arguments:
    // - scrollDirection: ScrollUp will move the viewport up, ScrollDown will move the viewport down
    // - rowsToScroll: a number of lines to move the viewport. If not provided we will use a system default.
}
