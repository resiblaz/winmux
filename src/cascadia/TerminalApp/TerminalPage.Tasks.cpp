// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"

#include "Tasks/TaskRuntimeConnection.h"
#include "TerminalPaneContent.h"

#include "../../types/inc/utils.hpp"

using namespace winrt;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Xaml;

namespace winrt::TerminalApp::implementation
{
    tasks::TaskSurfaceId TerminalPage::_TaskSurfaceId(const winrt::TerminalApp::Tab& tab) const noexcept
    {
        return tab ? static_cast<tasks::TaskSurfaceId>(
                         reinterpret_cast<uintptr_t>(winrt::get_self<implementation::Tab>(tab))) :
                     0;
    }

    bool TerminalPage::_IsTaskOwnedTab(const winrt::TerminalApp::Tab& tab) const noexcept
    {
        return _taskSurfacePolicy.IsTaskOwned(_TaskSurfaceId(tab));
    }

    tasks::TaskSurfaceId TerminalPage::_TaskSurfaceId(const winrt::com_ptr<Tab>& tab) const noexcept
    {
        return tab ? static_cast<tasks::TaskSurfaceId>(reinterpret_cast<uintptr_t>(tab.get())) : 0;
    }

    bool TerminalPage::_IsTaskOwnedTab(const winrt::com_ptr<Tab>& tab) const noexcept
    {
        return _taskSurfacePolicy.IsTaskOwned(_TaskSurfaceId(tab));
    }

    safe_void_coroutine TerminalPage::_HandleTaskSurface(tasks::TaskSurfaceDescriptor descriptor)
    {
        const auto strong = get_strong();
        if (descriptor.TaskId.empty() || descriptor.Generation == 0)
        {
            co_return;
        }
        if (descriptor.Action == tasks::TaskSurfaceAction::CloseAuthorized)
        {
            _CloseTaskSurface(
                descriptor.TaskId,
                descriptor.Generation,
                tasks::TaskSurfaceCloseOrigin::TaskDetail);
            co_return;
        }
        if (descriptor.OwnedElsewhere)
        {
            co_return;
        }
        if (const auto existing = _taskSurfacePolicy.Find(descriptor.TaskId))
        {
            for (const auto& tab : _tabs)
            {
                if (_TaskSurfaceId(tab) == *existing)
                {
                    _SetWorkspaceToolPage(WorkspaceToolPage::None);
                    static_cast<void>(_SelectTab(tab.TabViewIndex()));
                    tab.Focus(FocusState::Programmatic);
                    co_return;
                }
            }
            _taskSurfacePolicy.Unregister(*existing);
        }

        winrt::guid workspaceId{};
        try
        {
            if (descriptor.WorkspaceId.size() == 36)
            {
                workspaceId = ::Microsoft::Console::Utils::GuidFromPlainString(descriptor.WorkspaceId.c_str());
            }
        }
        catch (...)
        {
        }
        if (workspaceId == winrt::guid{})
        {
            workspaceId = _ReserveWorkspaceIdForNewTab();
        }
        if (workspaceId == winrt::guid{})
        {
            co_return;
        }

        if (_tabs.Size() > 0)
        {
            co_await wil::resume_foreground(Dispatcher(), CoreDispatcherPriority::Low);
        }

        const auto weakThis = get_weak();
        auto connectionImpl = winrt::make_self<tasks::TaskRuntimeConnection>(
            descriptor,
            [weakThis](std::wstring taskId, const uint64_t generation) {
                if (const auto page = weakThis.get())
                {
                    try
                    {
                        const auto weakPage = page->get_weak();
                        static_cast<void>(page->Dispatcher().RunAsync(
                            CoreDispatcherPriority::Normal,
                            [weakPage, taskId = std::move(taskId), generation] {
                                if (const auto current = weakPage.get())
                                {
                                    current->_CloseTaskSurface(
                                        taskId,
                                        generation,
                                        tasks::TaskSurfaceCloseOrigin::RuntimeEndOfStream);
                                }
                            }));
                    }
                    catch (...)
                    {
                    }
                }
            });
        ITerminalConnection connection{ *connectionImpl };
        NewTerminalArgs args;
        auto pane = _MakePane(args, nullptr, std::move(connection), false, workspaceId);
        if (!pane)
        {
            co_return;
        }
        pane->WalkTree([](const auto& current) {
            current->FinalizeConfigurationGivenDefault();
            if (const auto content = current->GetContent().try_as<TerminalApp::TerminalPaneContent>())
            {
                winrt::get_self<implementation::TerminalPaneContent>(content)->SuppressUserClose(true);
            }
        });
        const auto tab = _CreateNewTabFromPane(std::move(pane), -1, workspaceId);
        if (!tab)
        {
            co_return;
        }
        const auto surface = _TaskSurfaceId(tab);
        if (!_taskSurfacePolicy.Register(surface, { descriptor.TaskId, descriptor.Generation }))
        {
            tab.Close();
            co_return;
        }
        const auto tabImpl = winrt::get_self<implementation::Tab>(tab);
        tabImpl->CloseButtonVisibility(TabCloseButtonVisibility::Never);
        tabImpl->SetTabText(winrt::hstring{ descriptor.Title });
        _SetWorkspaceToolPage(WorkspaceToolPage::None);
        static_cast<void>(_SelectTab(tab.TabViewIndex()));
        tab.Focus(FocusState::Programmatic);
    }

    void TerminalPage::_CloseTaskSurface(
        const std::wstring_view taskId,
        const uint64_t generation,
        const tasks::TaskSurfaceCloseOrigin origin)
    {
        const auto surface = _taskSurfacePolicy.Find(taskId);
        if (!surface)
        {
            return;
        }
        auto authorization = ++_nextTaskSurfaceCloseAuthorization;
        if (authorization == 0)
        {
            authorization = ++_nextTaskSurfaceCloseAuthorization;
        }
        if (!_taskSurfacePolicy.AuthorizeClose(taskId, generation, authorization))
        {
            return;
        }
        const auto decision = _taskSurfacePolicy.Evaluate(*surface, origin, authorization);
        if (decision.Action != tasks::TaskSurfaceCloseAction::Allow || !decision.AuthorizationConsumed)
        {
            return;
        }
        for (const auto& tab : _tabs)
        {
            if (_TaskSurfaceId(tab) == *surface)
            {
                _taskSurfacePolicy.Unregister(*surface);
                tab.Close();
                return;
            }
        }
        _taskSurfacePolicy.Unregister(*surface);
    }
}
