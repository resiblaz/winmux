// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include "TaskBoardView.g.h"
#include "TaskActionController.h"
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <set>
#include <span>

namespace winrt::TerminalApp::implementation
{
    struct TaskBoardDrop
    {
        tasks::TaskProjection Task;
        tasks::TaskStatus Target{ tasks::TaskStatus::Todo };
        std::optional<std::wstring> Anchor;
        bool After{};
        uint64_t ColumnRevision{};
    };
    struct TaskBoardDropCandidate
    {
        std::wstring Id;
        std::wstring ProjectId;
        double Top{};
        double Height{};
        uint64_t ColumnRevision{};
    };
    struct TaskBoardDropPreview
    {
        bool Allowed{};
        TaskBoardDrop Drop;
        std::optional<double> MarkerY;
        const wchar_t* MessageKey{ L"TaskBoard_InvalidMove" };
    };
    TaskBoardDropPreview ResolveTaskBoardDrop(const tasks::TaskProjection& task, tasks::TaskStatus target,
                                            bool allProjects, bool pending, double pointerY,
                                            std::span<const TaskBoardDropCandidate> candidates);
    struct TaskBoardView : TaskBoardViewT<TaskBoardView>
    {
        TaskBoardView();
        void SetItems(const Windows::Foundation::Collections::IObservableVector<TerminalApp::TaskItemViewModel>& items);
        void SetPageInfo(const std::array<uint64_t, 7>& counts, const std::array<bool, 7>& more);
        void SetInteractionState(bool available, bool allProjects, bool loading, std::set<std::wstring> pending);
        void SetHandlers(std::function<void(TerminalApp::TaskItemViewModel)> selected, std::function<void(TaskBoardDrop)> drop, std::function<void(tasks::TaskStatus)> more);
        void _Card_ItemClick(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::Controls::ItemClickEventArgs&);
        void _CardActions_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _LoadMore_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _DragItemsStarting(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::Controls::DragItemsStartingEventArgs&);
        void _DragItemsCompleted(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::Controls::DragItemsCompletedEventArgs&);
        void _DragOver(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::DragEventArgs&);
        void _DragLeave(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::DragEventArgs&);
        void _Drop(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::DragEventArgs&);
        void _Card_Loaded(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&);
        void _Card_DataContextChanged(const Windows::UI::Xaml::FrameworkElement&, const Windows::UI::Xaml::DataContextChangedEventArgs&);
    private:
        std::function<void(TerminalApp::TaskItemViewModel)> _selected;
        std::function<void(TaskBoardDrop)> _drop;
        std::function<void(tasks::TaskStatus)> _more;
        std::optional<tasks::TaskProjection> _dragged;
        std::map<std::wstring, TerminalApp::TaskItemViewModel> _items;
        std::array<Windows::Foundation::Collections::IObservableVector<TerminalApp::TaskItemViewModel>, 7> _columns;
        std::array<uint64_t, 7> _counts{};
        std::array<bool, 7> _moreAvailable{};
        std::set<std::wstring> _pending;
        bool _available{ true };
        bool _allProjects{};
        bool _loading{};
        Windows::UI::Xaml::Controls::Border _previewColumn{ nullptr };
        Windows::UI::Xaml::Media::SolidColorBrush _transparent{ Windows::UI::Colors::Transparent() };
        Windows::UI::Xaml::Controls::ListView _ListForStatus(tasks::TaskStatus status);
        Windows::UI::Xaml::Controls::Border _ColumnForStatus(tasks::TaskStatus status);
        Windows::UI::Xaml::Media::Brush _StatusBrush(tasks::TaskStatus status);
        void _RefreshColumnState();
        void _RefreshCards();
        void _UpdateCard(const Windows::UI::Xaml::FrameworkElement& root);
        TaskBoardDropPreview _Preview(tasks::TaskStatus target, const Windows::UI::Xaml::DragEventArgs& args);
        void _ShowPreview(const TaskBoardDropPreview& preview);
        void _ClearPreview();
    };
}
namespace winrt::TerminalApp::factory_implementation { BASIC_FACTORY(TaskBoardView); }
