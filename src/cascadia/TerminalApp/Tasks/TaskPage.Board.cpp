// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskPage.h"
#include "TaskDetailsPane.h"
#include "RuntimeCrypto.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation
{
    using namespace projects::protocol;
    void TaskPage::_InitializeBoard()
    {
        const auto weak = get_weak();
        TaskOperationStatus().RegisterPropertyChangedCallback(TextBlock::TextProperty(), [weak](const auto&, const auto&) {
            if (const auto self = weak.get()) self->TaskFeedback().Visibility(self->TaskOperationStatus().Text().empty() ? Visibility::Collapsed : Visibility::Visible);
        });
        winrt::get_self<implementation::TaskBoardView>(BoardView())->SetHandlers(
            [weak](TerminalApp::TaskItemViewModel item) {
                if (const auto self = weak.get())
                {
                    self->_ViewModelImpl()->SelectTask(item); self->_boardDetailsOpen = true;
                    self->BoardDetailsOverlay().Visibility(Visibility::Visible);
                }
            },
            [weak](TaskBoardDrop drop) { if (const auto self = weak.get()) self->_BoardAction(std::move(drop)); },
            [weak](tasks::TaskStatus state) { if (const auto self = weak.get()) self->_ViewModelImpl()->LoadMore(state); });
        _viewModel.PropertyChanged([weak](const auto&, const auto& args) {
            if (const auto self = weak.get(); self && args.PropertyName() == L"Tasks") self->_UpdateBoard();
        });
        _boardPoll = DispatcherTimer{}; _boardPoll.Interval(std::chrono::seconds{ 2 });
        _boardPoll.Tick([weak](const auto&, const auto&) {
            if (const auto self = weak.get()) { if (self->_actions) self->_actions->PollOperations(); self->_UpdateBoard(); }
        });
        Loaded([weak](const auto&, const auto&) { if (const auto self = weak.get()) { self->_boardPoll.Start(); self->_UpdateBoard(); } });
        Unloaded([weak](const auto&, const auto&) { if (const auto self = weak.get()) { self->_boardPoll.Stop(); self->_CapturePageState(); } });
    }
    void TaskPage::_UpdateBoard()
    {
        if (!BoardView()) return;
        const auto source = _source ? _source->WorkspaceSource() : nullptr;
        BoardModeButton().IsEnabled(source && (source->SupportsBoard() || (!source->Connected() && _pageState->Board)));
        winrt::get_self<implementation::TaskDetailsPane>(DetailsPane())->SetBoardAvailable(source && source->SupportsBoard());
        const auto board = winrt::get_self<implementation::TaskBoardView>(BoardView());
        board->SetItems(_viewModel.Tasks());
        std::set<std::wstring> pending;
        for (const auto& item : _viewModel.Tasks())
            if (_actions && _actions->Pending(item.Id())) pending.emplace(item.Id());
        board->SetInteractionState(source && source->Connected() && source->SupportsBoard(),
                                   winrt::unbox_value_or<bool>(AllWorkspacesScope().IsChecked(), false), _viewModel.IsLoading(), std::move(pending));
        if (_source && _source->UsesSummaries())
        {
            std::array<bool, 7> more{};
            for (size_t i = 0; i < more.size(); ++i) more[i] = _source->HasMoreTasks(static_cast<tasks::TaskStatus>(i));
            const auto counts = _source->QueryCounts();
            board->SetPageInfo(counts, more);
            BacklogButton().Content(winrt::box_value(winrt::hstring{ RS_fmt(L"TaskBoard_AuxiliaryCount", RS_(L"TaskPage_StatusBacklog"), counts[0]) }));
            CancelledButton().Content(winrt::box_value(winrt::hstring{ RS_fmt(L"TaskBoard_AuxiliaryCount", RS_(L"TaskPage_StatusCancelled"), counts[6]) }));
            CancelledButton().Visibility(counts[6] ? Visibility::Visible : Visibility::Collapsed);
        }
        _SetBoardMode(_pageState->Board);
    }
    void TaskPage::_SetBoardMode(bool board)
    {
        const auto source = _source ? _source->WorkspaceSource() : nullptr;
        board = board && source && (source->SupportsBoard() || !source->Connected());
        const auto details = DetailsPane();
        if (board && !BoardDetailsHost().Child())
        {
            uint32_t index{};
            if (TaskDetailsRegion().Children().IndexOf(details, index)) TaskDetailsRegion().Children().RemoveAt(index);
            BoardDetailsHost().Child(details);
        }
        else if (!board && BoardDetailsHost().Child())
        {
            BoardDetailsHost().Child(nullptr); TaskDetailsRegion().Children().InsertAt(0, details);
        }
        _pageState->Board = board;
        BoardModeButton().IsChecked(board); ListModeButton().IsChecked(!board);
        BoardView().Visibility(board ? Visibility::Visible : Visibility::Collapsed);
        BoardHint().Visibility(board ? Visibility::Visible : Visibility::Collapsed);
        TasksSplitView().Visibility(!board && _source ? Visibility::Visible : Visibility::Collapsed);
        BoardDetailsOverlay().Visibility(board && _boardDetailsOpen ? Visibility::Visible : Visibility::Collapsed);
        _ViewModelImpl()->SetBoardMode(board);
    }
    void TaskPage::_BoardMode_Click(const IInspectable&, const RoutedEventArgs&)
    { _SetBoardMode(true); }
    void TaskPage::_ListMode_Click(const IInspectable&, const RoutedEventArgs&)
    { _SetBoardMode(false); }
    void TaskPage::_CloseBoardDetails_Click(const IInspectable&, const RoutedEventArgs&)
    { _boardDetailsOpen = false; BoardDetailsOverlay().Visibility(Visibility::Collapsed); BoardModeButton().Focus(FocusState::Programmatic); }
    void TaskPage::_LoadMoreTasks_Click(const IInspectable&, const RoutedEventArgs&)
    { _ViewModelImpl()->LoadMore(); }
    void TaskPage::_RetryBoardAction_Click(const IInspectable&, const RoutedEventArgs&)
    { if (_actions) _actions->Retry(_pageState->RetryBoardTask); }
    void TaskPage::_UndoBoardAction_Click(const IInspectable&, const RoutedEventArgs&)
    { if (_actions) _actions->Undo(_pageState->UndoProjectId); }
    void TaskPage::_BoardAuxiliary_Click(const IInspectable& sender, const RoutedEventArgs&)
    {
        const auto tag = winrt::unbox_value_or<hstring>(sender.as<Button>().Tag(), {});
        _SetBoardMode(false); StatusFilter().SelectedIndex(tag == L"7" ? 7 : 8);
    }
    safe_void_coroutine TaskPage::_BoardAction(TaskBoardDrop drop)
    {
        const auto lifetime = get_strong();
        const auto source = _source ? _source->WorkspaceSource() : nullptr;
        if (!source || !source->Connected() || !source->SupportsBoard() || !_actions)
        { TaskOperationStatus().Text(RS_(L"Project_Offline")); co_return; }
        const auto action = tasks::ResolveBoardAction(drop.Task, drop.Target);
        if (!action.Allowed || _actions->Pending(drop.Task.Id))
        { TaskOperationStatus().Text(RS_(action.Allowed ? L"TaskBoard_PendingOperation" : action.MessageKey)); co_return; }
        std::wstring text;
        if (action.Confirm || action.RequiresText)
        {
            const auto presenter = _dialogPresenter.get(); if (!presenter) co_return;
            ContentDialog dialog; dialog.Title(box_value(RS_(action.MessageKey)));
            dialog.PrimaryButtonText(RS_(L"TaskBoard_Confirm")); dialog.CloseButtonText(RS_(L"TaskBoard_Cancel"));
            dialog.DefaultButton(ContentDialogButton::Close);
            StackPanel content; content.Spacing(12);
            TextBlock description; description.Text(drop.Task.Title); description.TextWrapping(TextWrapping::Wrap); content.Children().Append(description);
            TextBox input;
            if (action.RequiresText)
            {
                input.Header(box_value(RS_(L"TaskBoard_Explanation"))); input.AcceptsReturn(true); input.TextWrapping(TextWrapping::Wrap); input.MinHeight(100);
                input.TextChanged([weakDialog = winrt::make_weak(dialog)](const IInspectable& sender, const TextChangedEventArgs&) {
                    if (const auto current = weakDialog.get()) current.IsPrimaryButtonEnabled(!sender.as<TextBox>().Text().empty());
                });
                dialog.IsPrimaryButtonEnabled(false); content.Children().Append(input);
            }
            dialog.Content(content);
            if (co_await presenter.ShowDialog(dialog) != ContentDialogResult::Primary) co_return;
            if (action.RequiresText) text = input.Text();
        }
        const auto command = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid(); if (!command) co_return;
        TaskBoardCommandRequest request;
        request.CommandId = *command; request.TaskId = til::u16u8(drop.Task.Id); request.ProjectId = til::u16u8(drop.Task.WorkspaceKey);
        request.ExpectedTaskVersion = drop.Task.Version; request.Operation = action.Operation; request.Target = static_cast<TaskProjectionStatus>(drop.Target);
        request.ExpectedColumnRevision = drop.ColumnRevision;
        if (drop.Anchor) request.AnchorId = til::u16u8(*drop.Anchor);
        request.Placement = drop.Anchor ? (drop.After ? TaskBoardPlacement::After : TaskBoardPlacement::Before) : TaskBoardPlacement::End;
        if (action.Operation == TaskBoardOperation::StopThenMove && !drop.Task.ActiveRunId.empty()) request.RelatedRunId = til::u16u8(drop.Task.ActiveRunId);
        request.Text = til::u16u8(text);
        _actions->Execute(std::move(request));
    }
}
