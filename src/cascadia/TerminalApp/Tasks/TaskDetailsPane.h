// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskDetailsPane.g.h"
#include "TaskProjection.h"

#include <functional>

namespace winrt::TerminalApp::implementation
{
    struct TaskDetailsPane : TaskDetailsPaneT<TaskDetailsPane>
    {
        TaskDetailsPane();

        til::property_changed_event PropertyChanged;
        WINRT_OBSERVABLE_PROPERTY(TerminalApp::TaskItemViewModel, Task, PropertyChanged.raise, nullptr);

    public:
        void SetCommandHandler(std::function<void(tasks::TaskCommand)> handler);

        void _StartTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _StopTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _OpenTerminal_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CloseTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _MarkDone_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ReopenTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

    private:
        std::function<void(tasks::TaskCommand)> _commandHandler;
        void _Request(tasks::TaskCommand command) const;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TaskDetailsPane);
}
