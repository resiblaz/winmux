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
        void SetMessageHandler(std::function<bool(const winrt::hstring&)> handler);
        void SetMessageMutationHandler(std::function<bool(uint32_t, const std::optional<std::wstring>&)> handler);

        void _StartTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ConfigureAgent_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _StopTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _OpenTerminal_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CloseTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _MarkDone_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _ReopenTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _RetryTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CancelTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _FinalizeWorktree_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _DeleteTask_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _FollowupInput_TextChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::TextChangedEventArgs& args);
        void _QueueFollowup_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _QueuedMessages_SelectionChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);
        void _EditQueuedMessage_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _RemoveQueuedMessage_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

    private:
        std::function<void(tasks::TaskCommand)> _commandHandler;
        std::function<bool(const winrt::hstring&)> _messageHandler;
        std::function<bool(uint32_t, const std::optional<std::wstring>&)> _messageMutationHandler;
        Windows::UI::Xaml::Controls::TextBox _followupInput{ nullptr };
        Windows::UI::Xaml::Controls::Button _queueFollowupButton{ nullptr };
        Windows::UI::Xaml::Controls::ListView _queuedMessages{ nullptr };
        Windows::UI::Xaml::Controls::TextBox _queuedMessageEditor{ nullptr };
        Windows::UI::Xaml::Controls::Button _editQueuedMessageButton{ nullptr };
        Windows::UI::Xaml::Controls::Button _removeQueuedMessageButton{ nullptr };
        void _Request(tasks::TaskCommand command) const;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TaskDetailsPane);
}
