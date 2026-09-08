// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskBoardView.h"
#include "TaskItemViewModel.h"
#include "TaskBoardView.g.cpp"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::ApplicationModel::DataTransfer;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        std::optional<tasks::TaskStatus> status(const Windows::Foundation::IInspectable& sender)
        {
            const auto control = sender.try_as<FrameworkElement>(); if (!control) return std::nullopt;
            const auto tag = winrt::unbox_value_or<winrt::hstring>(control.Tag(), {});
            if (tag.size() != 1 || tag[0] < L'0' || tag[0] > L'6') return std::nullopt;
            return static_cast<tasks::TaskStatus>(tag[0] - L'0');
        }
        const tasks::TaskProjection& projection(const TerminalApp::TaskItemViewModel& item)
        { return winrt::get_self<implementation::TaskItemViewModel>(item)->NativeProjection(); }
        winrt::hstring executionLabel(const tasks::TaskProjection& task)
        {
            if (task.HasActiveRun && task.LatestRunState && *task.LatestRunState <= 4)
            {
                constexpr std::array labels{ L"TaskPage_RunStateQueued", L"TaskPage_RunStatePreparing", L"TaskPage_RunStateWaiting", L"TaskPage_RunStateStarting", L"TaskPage_RunStateRunning" };
                return RS_(labels[*task.LatestRunState]);
            }
            if (task.HasActiveRun) return task.ActiveRunSummary.empty() ? RS_(L"TaskBoard_Executing") : winrt::hstring{ task.ActiveRunSummary };
            if (task.Status == tasks::TaskStatus::Done) return RS_(L"TaskBoard_Accepted");
            if (task.Status == tasks::TaskStatus::InReview) return RS_(L"TaskBoard_ReadyForReview");
            if (!task.ActiveRunSummary.empty()) return winrt::hstring{ task.ActiveRunSummary };
            if (task.Status == tasks::TaskStatus::Blocked) return RS_(L"TaskPage_StatusBlocked");
            if (task.ManualHold) return RS_(L"TaskBoard_Paused");
            return RS_(L"TaskBoard_NotStarted");
        }
    }

    TaskBoardDropPreview ResolveTaskBoardDrop(const tasks::TaskProjection& task, tasks::TaskStatus target,
                                            bool allProjects, bool pending, double pointerY,
                                            std::span<const TaskBoardDropCandidate> candidates)
    {
        TaskBoardDropPreview preview; preview.Drop.Task = task; preview.Drop.Target = target;
        if (pending) { preview.MessageKey = L"TaskBoard_PendingOperation"; return preview; }
        if (task.Status == tasks::TaskStatus::Blocked && target == tasks::TaskStatus::InProgress)
        { preview.MessageKey = L"TaskBoard_ResumeFromMenu"; return preview; }
        if (allProjects && task.Status == target)
        { preview.MessageKey = L"TaskBoard_ReorderCurrentProject"; return preview; }
        const auto action = tasks::ResolveBoardAction(task, target);
        preview.Allowed = action.Allowed; preview.MessageKey = action.MessageKey;
        if (!preview.Allowed || allProjects) return preview;
        for (const auto& candidate : candidates)
        {
            if (candidate.Id == task.Id || candidate.ProjectId != task.WorkspaceKey) continue;
            preview.Drop.Anchor = candidate.Id; preview.Drop.ColumnRevision = candidate.ColumnRevision;
            preview.Drop.After = pointerY >= candidate.Top + candidate.Height / 2;
            preview.MarkerY = preview.Drop.After ? candidate.Top + candidate.Height : candidate.Top;
            if (!preview.Drop.After) break;
        }
        if (!preview.MarkerY) preview.MarkerY = 4;
        if (task.Status == target) preview.Drop.ColumnRevision = task.ColumnRevision;
        return preview;
    }

    TaskBoardView::TaskBoardView()
    {
        InitializeComponent();
        for (auto& column : _columns) column = winrt::single_threaded_observable_vector<TerminalApp::TaskItemViewModel>();
        TodoList().ItemsSource(_columns[1]); DoingList().ItemsSource(_columns[2]);
        ReviewList().ItemsSource(_columns[3]); BlockedList().ItemsSource(_columns[4]); DoneList().ItemsSource(_columns[5]);
        BoardScroller().SizeChanged([weak = get_weak()](const auto&, const SizeChangedEventArgs& args) {
            if (const auto self = weak.get())
            {
                const auto width = static_cast<double>(args.NewSize().Width);
                self->BoardColumns().Width((std::max)(848.0, width - 32.0));
                self->BoardColumns().Height((std::max)(160.0, static_cast<double>(args.NewSize().Height) - 20.0));
                self->HorizontalHint().Visibility(width < 880 ? Visibility::Visible : Visibility::Collapsed);
            }
        });
        Unloaded([weak = get_weak()](const auto&, const auto&) {
            if (const auto self = weak.get()) { self->_dragged.reset(); self->_ClearPreview(); }
        });
        _RefreshColumnState();
    }
    ListView TaskBoardView::_ListForStatus(tasks::TaskStatus state)
    {
        switch (state)
        {
        case tasks::TaskStatus::Todo: return TodoList();
        case tasks::TaskStatus::InProgress: return DoingList();
        case tasks::TaskStatus::InReview: return ReviewList();
        case tasks::TaskStatus::Blocked: return BlockedList();
        case tasks::TaskStatus::Done: return DoneList();
        default: return nullptr;
        }
    }
    Border TaskBoardView::_ColumnForStatus(tasks::TaskStatus state)
    {
        switch (state)
        {
        case tasks::TaskStatus::Todo: return TodoColumn();
        case tasks::TaskStatus::InProgress: case tasks::TaskStatus::Blocked: return DoingColumn();
        case tasks::TaskStatus::InReview: return ReviewColumn();
        case tasks::TaskStatus::Done: return DoneColumn();
        default: return nullptr;
        }
    }
    Media::Brush TaskBoardView::_StatusBrush(tasks::TaskStatus state)
    {
        switch (state)
        {
        case tasks::TaskStatus::InProgress: return DoingGlyph().Foreground();
        case tasks::TaskStatus::Blocked: case tasks::TaskStatus::InReview: return ReviewGlyph().Foreground();
        case tasks::TaskStatus::Done: return DoneGlyph().Foreground();
        default: return TodoGlyph().Foreground();
        }
    }
    void TaskBoardView::SetPageInfo(const std::array<uint64_t, 7>& counts, const std::array<bool, 7>& more)
    { _counts = counts; _moreAvailable = more; _RefreshColumnState(); }
    void TaskBoardView::SetInteractionState(bool available, bool allProjects, bool loading, std::set<std::wstring> pending)
    {
        _available = available; _allProjects = allProjects; _loading = loading; _pending = std::move(pending);
        if (!_available) { _dragged.reset(); _ClearPreview(); }
        _RefreshCards(); _RefreshColumnState();
    }
    void TaskBoardView::_RefreshColumnState()
    {
        TodoCount().Text(winrt::to_hstring(_counts[1])); DoingCount().Text(winrt::to_hstring(_counts[2] + _counts[4]));
        ReviewCount().Text(winrt::to_hstring(_counts[3])); DoneCount().Text(winrt::to_hstring(_counts[5]));
        BlockedHeading().Text(winrt::hstring{ RS_fmt(L"TaskBoard_ColumnCount", RS_(L"TaskPage_StatusBlocked"), _counts[4]) });
        const auto blocked = _columns[4].Size() > 0;
        BlockedHeading().Visibility(blocked ? Visibility::Visible : Visibility::Collapsed);
        BlockedList().Visibility(blocked ? Visibility::Visible : Visibility::Collapsed);
        const auto setEmpty = [&](const Grid& empty, const TextBlock& label, size_t state, bool noCards) {
            empty.Visibility(noCards ? Visibility::Visible : Visibility::Collapsed);
            label.Text(RS_(_loading ? L"TaskBoard_Loading" : _counts[state] ? L"TaskBoard_NoVisibleTasks" : L"TaskBoard_Empty"));
        };
        setEmpty(TodoEmpty(), TodoEmptyLabel(), 1, _columns[1].Size() == 0);
        setEmpty(DoingEmpty(), DoingEmptyLabel(), 2, _columns[2].Size() == 0 && !blocked);
        setEmpty(ReviewEmpty(), ReviewEmptyLabel(), 3, _columns[3].Size() == 0);
        setEmpty(DoneEmpty(), DoneEmptyLabel(), 5, _columns[5].Size() == 0);
        const auto setMore = [&](const Button& button, bool more) { button.Visibility(more ? Visibility::Visible : Visibility::Collapsed); button.IsEnabled(more && !_loading && _available); };
        setMore(TodoMore(), _moreAvailable[1]); setMore(DoingMore(), _moreAvailable[2] || _moreAvailable[4]);
        setMore(ReviewMore(), _moreAvailable[3]); setMore(DoneMore(), _moreAvailable[5]);
        constexpr std::array states{ tasks::TaskStatus::Todo, tasks::TaskStatus::InProgress, tasks::TaskStatus::InReview, tasks::TaskStatus::Done };
        for (const auto state : states)
        {
            const auto count = state == tasks::TaskStatus::InProgress ? _counts[2] + _counts[4] : _counts[static_cast<size_t>(state)];
            constexpr std::array names{ L"TaskPage_StatusBacklog", L"TaskPage_StatusTodo", L"TaskPage_StatusInProgress", L"TaskPage_StatusInReview", L"TaskPage_StatusBlocked", L"TaskPage_StatusDone", L"TaskPage_StatusCancelled" };
            Automation::AutomationProperties::SetName(_ColumnForStatus(state), winrt::hstring{ RS_fmt(L"TaskBoard_ColumnCount", RS_(names[static_cast<size_t>(state)]), count) });
        }
    }
    void TaskBoardView::SetHandlers(std::function<void(TerminalApp::TaskItemViewModel)> selected,
                                   std::function<void(TaskBoardDrop)> drop, std::function<void(tasks::TaskStatus)> more)
    { _selected = std::move(selected); _drop = std::move(drop); _more = std::move(more); }
    void TaskBoardView::SetItems(const Windows::Foundation::Collections::IObservableVector<TerminalApp::TaskItemViewModel>& items)
    {
        std::array<std::vector<TerminalApp::TaskItemViewModel>, 7> desired;
        _items.clear(); _counts.fill(0);
        for (const auto& item : items)
        {
            _items.emplace(std::wstring{ item.Id() }, item);
            const auto state = static_cast<size_t>(projection(item).Status);
            desired[state].push_back(item); ++_counts[state];
        }
        for (size_t state = 0; state < desired.size(); ++state)
        {
            auto& next = desired[state]; auto& column = _columns[state];
            std::ranges::sort(next, [](const auto& a, const auto& b) {
                const auto& left = projection(a); const auto& right = projection(b);
                if (left.WorkspaceKey != right.WorkspaceKey) return left.WorkspaceKey < right.WorkspaceKey;
                return left.BoardRank != right.BoardRank ? left.BoardRank < right.BoardRank : left.Id < right.Id;
            });
            for (uint32_t i = 0; i < next.size(); ++i)
            {
                if (i < column.Size() && column.GetAt(i) == next[i]) continue;
                uint32_t found{}; if (column.IndexOf(next[i], found)) column.RemoveAt(found);
                column.InsertAt(i, next[i]);
            }
            while (column.Size() > next.size()) column.RemoveAtEnd();
        }
        _RefreshColumnState(); _RefreshCards();
    }
    void TaskBoardView::_Card_Loaded(const Windows::Foundation::IInspectable& sender, const RoutedEventArgs&)
    { if (const auto root = sender.try_as<FrameworkElement>()) _UpdateCard(root); }
    void TaskBoardView::_Card_DataContextChanged(const FrameworkElement& sender, const DataContextChangedEventArgs&)
    { _UpdateCard(sender); }
    void TaskBoardView::_UpdateCard(const FrameworkElement& root)
    {
        const auto item = root.DataContext().try_as<TerminalApp::TaskItemViewModel>(); if (!item) return;
        const auto id = root.FindName(L"CardId").try_as<TextBlock>();
        const auto agent = root.FindName(L"CardAgent").try_as<TextBlock>();
        const auto run = root.FindName(L"CardRun").try_as<TextBlock>();
        const auto glyph = root.FindName(L"CardRunGlyph").try_as<TextBlock>();
        const auto pendingRow = root.FindName(L"CardPending").try_as<StackPanel>();
        if (!id || !agent || !run || !glyph || !pendingRow) return;
        const auto& task = projection(item);
        id.Text(winrt::hstring{ task.Id.size() == 36 ? task.Id.substr(0, 8) : task.Id });
        auto label = task.AgentName.empty() ? std::wstring{ RS_(L"Agent_Custom") } : task.AgentName;
        if (!task.AgentName.empty() && task.AgentRevision) label += L" · v" + std::to_wstring(task.AgentRevision);
        agent.Text(label); run.Text(executionLabel(task)); run.Foreground(_StatusBrush(task.Status));
        glyph.Text(item.StatusGlyph()); glyph.Foreground(run.Foreground());
        const auto pending = _pending.contains(task.Id) || task.HasPendingIntent;
        pendingRow.Visibility(pending ? Visibility::Visible : Visibility::Collapsed);
        if (const auto ring = root.FindName(L"CardPendingRing").try_as<ProgressRing>()) ring.IsActive(pending);
        if (const auto project = root.FindName(L"CardProject").try_as<TextBlock>()) project.Visibility(_allProjects ? Visibility::Visible : Visibility::Collapsed);
        if (const auto frame = root.try_as<Border>()) frame.BorderBrush(pending ? DoingGlyph().Foreground() : TodoEmptyOutline().Stroke());
    }
    void TaskBoardView::_RefreshCards()
    {
        for (const auto state : { tasks::TaskStatus::Todo, tasks::TaskStatus::InProgress, tasks::TaskStatus::InReview, tasks::TaskStatus::Blocked, tasks::TaskStatus::Done })
        {
            const auto list = _ListForStatus(state);
            for (uint32_t i = 0; i < list.Items().Size(); ++i)
                if (const auto container = list.ContainerFromIndex(i).try_as<ListViewItem>())
                    if (const auto root = container.ContentTemplateRoot().try_as<FrameworkElement>()) _UpdateCard(root);
        }
    }
    void TaskBoardView::_Card_ItemClick(const Windows::Foundation::IInspectable&, const ItemClickEventArgs& args)
    { if (!_dragged && _selected) _selected(args.ClickedItem().as<TerminalApp::TaskItemViewModel>()); }
    void TaskBoardView::_LoadMore_Click(const Windows::Foundation::IInspectable& sender, const RoutedEventArgs&)
    {
        if (const auto target = status(sender); target && _more)
        { sender.as<Button>().IsEnabled(false); _more(*target); if (*target == tasks::TaskStatus::InProgress) _more(tasks::TaskStatus::Blocked); }
    }
    void TaskBoardView::_CardActions_Click(const Windows::Foundation::IInspectable& sender, const RoutedEventArgs&)
    {
        const auto button = sender.as<Button>(); const auto id = winrt::unbox_value_or<winrt::hstring>(button.Tag(), {});
        const auto found = _items.find(std::wstring{ id }); if (found == _items.end()) return;
        const auto item = found->second; const auto task = projection(item); const auto weak = get_weak(); MenuFlyout menu;
        MenuFlyoutItem details; details.Text(RS_(L"TaskBoard_OpenDetails"));
        details.Click([weak, item](const auto&, const auto&) { if (const auto self = weak.get(); self && self->_selected) self->_selected(item); });
        menu.Items().Append(details);
        constexpr std::array<const wchar_t*, 7> labels{ L"TaskBoard_MoveBacklog", L"TaskBoard_MoveTodo", L"TaskBoard_Start", L"TaskBoard_SubmitReview", L"TaskBoard_BlockReason", L"TaskBoard_ConfirmDone", L"TaskBoard_ReopenOnly" };
        for (size_t i = 0; i < labels.size(); ++i)
        {
            const auto target = static_cast<tasks::TaskStatus>(i);
            if (target == task.Status || !tasks::ResolveBoardAction(task, target).Allowed) continue;
            MenuFlyoutItem action; action.Text(RS_(labels[i])); action.IsEnabled(_available && !_pending.contains(task.Id));
            action.Click([weak, task, target](const auto&, const auto&) { if (const auto self = weak.get(); self && self->_drop) self->_drop({ task, target }); });
            menu.Items().Append(action);
        }
        menu.ShowAt(button);
    }
    void TaskBoardView::_DragItemsStarting(const Windows::Foundation::IInspectable&, const DragItemsStartingEventArgs& args)
    {
        _dragged.reset(); _ClearPreview();
        if (args.Items().Size() != 1) { args.Cancel(true); return; }
        const auto item = args.Items().GetAt(0).try_as<TerminalApp::TaskItemViewModel>();
        if (!item) { args.Cancel(true); return; }
        if (!_available || _pending.contains(std::wstring{ item.Id() }))
        {
            args.Cancel(true); DragFeedback().Text(RS_(_available ? L"TaskBoard_PendingOperation" : L"Project_Offline"));
            DragFeedback().Visibility(Visibility::Visible); return;
        }
        _dragged = projection(item);
        args.Data().SetText(item.Id()); args.Data().RequestedOperation(DataPackageOperation::Move);
    }
    void TaskBoardView::_DragItemsCompleted(const Windows::Foundation::IInspectable&, const DragItemsCompletedEventArgs&)
    { _dragged.reset(); _ClearPreview(); }
    TaskBoardDropPreview TaskBoardView::_Preview(tasks::TaskStatus target, const DragEventArgs& args)
    {
        const auto list = _ListForStatus(target);
        if (!_dragged || !list || !_available) return {};
        std::vector<TaskBoardDropCandidate> candidates;
        for (uint32_t i = 0; i < list.Items().Size(); ++i)
        {
            const auto item = list.Items().GetAt(i).as<TerminalApp::TaskItemViewModel>();
            const auto& task = projection(item);
            if (const auto container = list.ContainerFromIndex(i).try_as<ListViewItem>())
            {
                const auto origin = container.TransformToVisual(list).TransformPoint({ 0, 0 });
                candidates.push_back({ task.Id, task.WorkspaceKey, origin.Y, container.ActualHeight(), task.ColumnRevision });
            }
        }
        return ResolveTaskBoardDrop(*_dragged, target, _allProjects, _pending.contains(_dragged->Id), args.GetPosition(list).Y, candidates);
    }
    void TaskBoardView::_ShowPreview(const TaskBoardDropPreview& preview)
    {
        if (_previewColumn) _previewColumn.BorderBrush(_transparent);
        DropMarker().Visibility(Visibility::Collapsed);
        _previewColumn = _ColumnForStatus(preview.Drop.Target);
        if (_previewColumn) _previewColumn.BorderBrush(preview.Allowed ? DoingGlyph().Foreground() : ReviewGlyph().Foreground());
        DragFeedback().Text(RS_(preview.MessageKey)); DragFeedback().Visibility(Visibility::Visible);
        if (!preview.Allowed || !preview.MarkerY) return;
        const auto list = _ListForStatus(preview.Drop.Target); if (!list) return;
        const auto origin = list.TransformToVisual(BoardColumns()).TransformPoint({ 0, 0 });
        Canvas::SetLeft(DropMarker(), origin.X + 2);
        Canvas::SetTop(DropMarker(), origin.Y + std::clamp(*preview.MarkerY, 0.0, (std::max)(0.0, list.ActualHeight() - 3)));
        DropMarker().Width((std::max)(0.0, list.ActualWidth() - 4)); DropMarker().Visibility(Visibility::Visible);
    }
    void TaskBoardView::_ClearPreview()
    {
        if (_previewColumn) _previewColumn.BorderBrush(_transparent);
        _previewColumn = nullptr; DropMarker().Visibility(Visibility::Collapsed); DragFeedback().Visibility(Visibility::Collapsed);
    }
    void TaskBoardView::_DragOver(const Windows::Foundation::IInspectable& sender, const DragEventArgs& args)
    {
        const auto target = status(sender);
        if (!_dragged || !target) { args.AcceptedOperation(DataPackageOperation::None); return; }
        const auto preview = _Preview(*target, args); _ShowPreview(preview);
        args.AcceptedOperation(preview.Allowed ? DataPackageOperation::Move : DataPackageOperation::None);
        args.DragUIOverride().Caption(RS_(preview.MessageKey)); args.DragUIOverride().IsCaptionVisible(true);
        args.Handled(true);
    }
    void TaskBoardView::_DragLeave(const Windows::Foundation::IInspectable& sender, const DragEventArgs& args)
    {
        const auto element = sender.try_as<FrameworkElement>(); if (!element) return;
        const auto point = args.GetPosition(element);
        if (point.X < 0 || point.Y < 0 || point.X > element.ActualWidth() || point.Y > element.ActualHeight()) _ClearPreview();
    }
    void TaskBoardView::_Drop(const Windows::Foundation::IInspectable& sender, const DragEventArgs& args)
    {
        const auto target = status(sender);
        if (!_dragged || !target || !_drop) return;
        const auto preview = _Preview(*target, args); _ClearPreview();
        args.AcceptedOperation(preview.Allowed ? DataPackageOperation::Move : DataPackageOperation::None); args.Handled(true);
        if (preview.Allowed) _drop(preview.Drop);
    }
}
