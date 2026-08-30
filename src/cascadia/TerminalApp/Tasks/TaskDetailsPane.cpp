// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskDetailsPane.h"

#include "TaskDetailsPane.g.cpp"

namespace winrt::TerminalApp::implementation
{
    TaskDetailsPane::TaskDetailsPane()
    {
        InitializeComponent();
    }

    void TaskDetailsPane::SetCommandHandler(std::function<void(tasks::TaskCommand)> handler)
    {
        _commandHandler = std::move(handler);
    }

    void TaskDetailsPane::_StartTask_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::Start);
    }

    void TaskDetailsPane::_StopTask_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::Stop);
    }

    void TaskDetailsPane::_OpenTerminal_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::OpenTerminal);
    }

    void TaskDetailsPane::_CloseTask_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::CloseTerminal);
    }

    void TaskDetailsPane::_MarkDone_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::MarkDone);
    }

    void TaskDetailsPane::_ReopenTask_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::Reopen);
    }

    void TaskDetailsPane::_Request(const tasks::TaskCommand command) const
    {
        if (_commandHandler)
        {
            _commandHandler(command);
        }
    }
}
