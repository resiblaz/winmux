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
        _followupInput = FollowupInput();
        _queueFollowupButton = QueueFollowupButton();
    }

    void TaskDetailsPane::Task(const TerminalApp::TaskItemViewModel& value)
    {
        if (_task == value) return;
        const auto previousId = _task ? _task.Id() : winrt::hstring{};
        const auto nextId = value ? value.Id() : winrt::hstring{};
        if (!previousId.empty()) _followupDrafts[std::wstring{ previousId }] = _followupInput.Text();
        _task = value;
        _queuedMessages = nullptr; _queuedMessageEditor = nullptr;
        _editQueuedMessageButton = nullptr; _removeQueuedMessageButton = nullptr;
        if (previousId != nextId)
        {
            _restoringFollowup = true;
            const auto draft = _followupDrafts.find(std::wstring{ nextId });
            _followupInput.Tag(winrt::box_value(nextId)); _queueFollowupButton.Tag(winrt::box_value(nextId));
            _followupInput.Text(draft == _followupDrafts.end() ? winrt::hstring{} : draft->second);
            _restoringFollowup = false;
        }
        FollowupComposer().Visibility(value ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed);
        _queueFollowupButton.IsEnabled(value && !_followupInput.Text().empty());
        EditAcceptanceButton().IsEnabled(_boardAvailable && value && value.CanMarkDone());
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"Task" });
    }

    void TaskDetailsPane::SetCommandHandler(std::function<void(tasks::TaskCommand)> handler)
    {
        _commandHandler = std::move(handler);
    }
    void TaskDetailsPane::SetBoardAvailable(bool value)
    {
        _boardAvailable = value;
        EditAcceptanceButton().Visibility(value ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed);
        EditAcceptanceButton().IsEnabled(value && Task() && Task().CanMarkDone());
    }
    void TaskDetailsPane::_EditAcceptance_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    { _Request(tasks::TaskCommand::EditAcceptance); }

    void TaskDetailsPane::SetMessageHandler(std::function<bool(const winrt::hstring&)> handler)
    {
        _messageHandler = std::move(handler);
    }

    void TaskDetailsPane::SetMessageMutationHandler(
        std::function<bool(uint32_t, const std::optional<std::wstring>&)> handler)
    {
        _messageMutationHandler = std::move(handler);
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

    void TaskDetailsPane::_RetryTask_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::Retry);
    }

    void TaskDetailsPane::_CancelTask_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::Cancel);
    }

    void TaskDetailsPane::_FinalizeWorktree_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::FinalizeWorktree);
    }

    void TaskDetailsPane::_DeleteTask_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::Delete);
    }

    bool TaskDetailsPane::_IsCurrentInput(const Windows::UI::Xaml::Controls::Control& control) const
    {
        return control && Task() && winrt::unbox_value_or<winrt::hstring>(control.Tag(), {}) == Task().Id();
    }

    void TaskDetailsPane::_FollowupInput_TextChanged(
        const Windows::Foundation::IInspectable& sender,
        const Windows::UI::Xaml::Controls::TextChangedEventArgs&)
    {
        const auto input = sender.try_as<Windows::UI::Xaml::Controls::TextBox>();
        if (_restoringFollowup || !_IsCurrentInput(input) || input != _followupInput) return;
        _followupDrafts[std::wstring{ Task().Id() }] = input.Text();
        if (_followupInput && !_queueFollowupButton)
        {
            if (const auto parent = _followupInput.Parent().try_as<Windows::UI::Xaml::Controls::StackPanel>())
            {
                for (const auto& child : parent.Children())
                {
                    if (const auto button = child.try_as<Windows::UI::Xaml::Controls::Button>())
                        _queueFollowupButton = button;
                }
            }
        }
        if (_queueFollowupButton)
            _queueFollowupButton.IsEnabled(_followupInput && !_followupInput.Text().empty());
    }

    void TaskDetailsPane::_QueueFollowup_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        const auto button = sender.try_as<Windows::UI::Xaml::Controls::Button>();
        if (!_IsCurrentInput(button) || !_IsCurrentInput(_followupInput)) return;
        _queueFollowupButton = button;
        const auto message = _followupInput ? _followupInput.Text() : winrt::hstring{};
        if (!message.empty() && _messageHandler && _messageHandler(message))
        {
            _followupInput.Text({});
            _followupInput.Focus(Windows::UI::Xaml::FocusState::Programmatic);
        }
    }

    void TaskDetailsPane::_QueuedMessages_SelectionChanged(
        const Windows::Foundation::IInspectable& sender,
        const Windows::UI::Xaml::Controls::SelectionChangedEventArgs&)
    {
        const auto list = sender.try_as<Windows::UI::Xaml::Controls::ListView>();
        if (!list || list.DataContext().try_as<TerminalApp::TaskItemViewModel>() != Task()) return;
        _queuedMessages = list;
        const auto selected = _queuedMessages ? _queuedMessages.SelectedItem() : nullptr;
        if (_queuedMessages && (!_queuedMessageEditor || !_editQueuedMessageButton || !_removeQueuedMessageButton))
        {
            if (const auto parent = _queuedMessages.Parent().try_as<Windows::UI::Xaml::Controls::StackPanel>())
            {
                for (const auto& child : parent.Children())
                {
                    if (const auto editor = child.try_as<Windows::UI::Xaml::Controls::TextBox>())
                        _queuedMessageEditor = editor;
                    if (const auto actions = child.try_as<Windows::UI::Xaml::Controls::StackPanel>())
                    {
                        for (const auto& action : actions.Children())
                        {
                            if (const auto button = action.try_as<Windows::UI::Xaml::Controls::Button>())
                            {
                                if (!_editQueuedMessageButton)
                                    _editQueuedMessageButton = button;
                                else
                                    _removeQueuedMessageButton = button;
                            }
                        }
                    }
                }
            }
        }
        if (_queuedMessageEditor)
            _queuedMessageEditor.Text(selected ? winrt::unbox_value_or<winrt::hstring>(selected, {}) : winrt::hstring{});
        if (_editQueuedMessageButton)
            _editQueuedMessageButton.IsEnabled(static_cast<bool>(selected));
        if (_removeQueuedMessageButton)
            _removeQueuedMessageButton.IsEnabled(static_cast<bool>(selected));
    }

    void TaskDetailsPane::_EditQueuedMessage_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        const auto index = _queuedMessages ? _queuedMessages.SelectedIndex() : -1;
        const auto replacement = _queuedMessageEditor ? _queuedMessageEditor.Text() : winrt::hstring{};
        if (index >= 0 && !replacement.empty() && _messageMutationHandler &&
            _messageMutationHandler(static_cast<uint32_t>(index), std::wstring{ replacement }))
        {
            _queuedMessages.SelectedIndex(-1);
        }
    }

    void TaskDetailsPane::_RemoveQueuedMessage_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        const auto index = _queuedMessages ? _queuedMessages.SelectedIndex() : -1;
        if (index >= 0 && _messageMutationHandler &&
            _messageMutationHandler(static_cast<uint32_t>(index), std::nullopt))
        {
            _queuedMessages.SelectedIndex(-1);
        }
    }

    void TaskDetailsPane::_ConfigureAgent_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _Request(tasks::TaskCommand::ConfigureAgent);
    }

    void TaskDetailsPane::_Request(const tasks::TaskCommand command) const
    {
        if (_commandHandler)
        {
            _commandHandler(command);
        }
    }
}
