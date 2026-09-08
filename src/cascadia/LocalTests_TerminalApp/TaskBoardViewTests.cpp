// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "../TerminalApp/Tasks/TaskBoardView.h"
#include "../TerminalApp/Tasks/TaskItemViewModel.h"
#include "CppWinrtTailored.h"

using namespace WEX::TestExecution;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace TerminalAppLocalTests
{
    namespace ui = winrt::TerminalApp::implementation;
    namespace tasks = ui::tasks;

    class TaskBoardViewTests
    {
        BEGIN_TEST_CLASS(TaskBoardViewTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()
        TEST_METHOD(DropPreviewUsesVisibleAnchorsAndPreservesProjectScope);
        TEST_METHOD(ColumnsExposeCountsEmptyStatesAndOnlyAvailablePages);
        TEST_METHOD(LoadedCardsPreserveIdentityAndShowPendingOperations);
    };

    void TaskBoardViewTests::DropPreviewUsesVisibleAnchorsAndPreservesProjectScope()
    {
        tasks::TaskProjection task; task.Id = L"dragged"; task.WorkspaceKey = L"project-a";
        task.Status = tasks::TaskStatus::Todo; task.ColumnRevision = 12;
        const std::vector<ui::TaskBoardDropCandidate> rows{
            { L"first", L"project-a", 0, 40, 20 },
            { L"foreign", L"project-b", 45, 40, 99 },
            { task.Id, task.WorkspaceKey, 90, 40, 12 },
            { L"last-visible", L"project-a", 135, 40, 20 },
        };
        auto preview = ui::ResolveTaskBoardDrop(task, tasks::TaskStatus::Todo, false, false, 0, rows);
        VERIFY_IS_TRUE(preview.Allowed); VERIFY_IS_TRUE(preview.Drop.Anchor == L"first");
        VERIFY_IS_FALSE(preview.Drop.After); VERIFY_ARE_EQUAL(0.0, *preview.MarkerY);
        VERIFY_ARE_EQUAL(uint64_t{ 12 }, preview.Drop.ColumnRevision);
        preview = ui::ResolveTaskBoardDrop(task, tasks::TaskStatus::Todo, false, false, 300, rows);
        VERIFY_IS_TRUE(preview.Drop.Anchor == L"last-visible"); VERIFY_IS_TRUE(preview.Drop.After);
        VERIFY_ARE_EQUAL(175.0, *preview.MarkerY); // Anchor after the visible page, never silently jump past unloaded rows.
        preview = ui::ResolveTaskBoardDrop(task, tasks::TaskStatus::InProgress, false, false, 10, rows);
        VERIFY_IS_TRUE(preview.Allowed); VERIFY_ARE_EQUAL(uint64_t{ 20 }, preview.Drop.ColumnRevision);
        preview = ui::ResolveTaskBoardDrop(task, tasks::TaskStatus::Todo, true, false, 10, rows);
        VERIFY_IS_FALSE(preview.Allowed);
        preview = ui::ResolveTaskBoardDrop(task, tasks::TaskStatus::InProgress, true, false, 10, rows);
        VERIFY_IS_TRUE(preview.Allowed); VERIFY_IS_FALSE(preview.Drop.Anchor.has_value()); VERIFY_IS_FALSE(preview.MarkerY.has_value());
        VERIFY_ARE_EQUAL(task.WorkspaceKey, preview.Drop.Task.WorkspaceKey);
        VERIFY_IS_FALSE(ui::ResolveTaskBoardDrop(task, tasks::TaskStatus::InProgress, false, true, 10, rows).Allowed);
        VERIFY_IS_FALSE(ui::ResolveTaskBoardDrop(task, tasks::TaskStatus::Done, false, false, 10, rows).Allowed);
        task.Status = tasks::TaskStatus::Blocked;
        VERIFY_IS_FALSE(ui::ResolveTaskBoardDrop(task, tasks::TaskStatus::InProgress, false, false, 10, rows).Allowed);
        VERIFY_IS_TRUE(ui::ResolveTaskBoardDrop(task, tasks::TaskStatus::Blocked, false, false, 10, rows).Allowed);
    }

    void TaskBoardViewTests::ColumnsExposeCountsEmptyStatesAndOnlyAvailablePages()
    {
        VERIFY_SUCCEEDED(RunOnUIThread([] {
            const auto board = winrt::make_self<ui::TaskBoardView>();
            auto items = winrt::single_threaded_observable_vector<winrt::TerminalApp::TaskItemViewModel>();
            for (const auto state : { tasks::TaskStatus::Todo, tasks::TaskStatus::InProgress, tasks::TaskStatus::Blocked, tasks::TaskStatus::InReview })
            {
                tasks::TaskProjection task; task.Id = std::to_wstring(static_cast<int>(state)); task.Status = state;
                items.Append(winrt::make<ui::TaskItemViewModel>(task));
            }
            board->SetItems(items); board->SetInteractionState(true, false, false, {});
            VERIFY_ARE_EQUAL(L"1", board->TodoCount().Text()); VERIFY_ARE_EQUAL(L"2", board->DoingCount().Text());
            VERIFY_ARE_EQUAL(L"1", board->ReviewCount().Text()); VERIFY_ARE_EQUAL(L"0", board->DoneCount().Text());
            VERIFY_IS_TRUE(board->TodoEmpty().Visibility() == Visibility::Collapsed);
            VERIFY_IS_TRUE(board->DoneEmpty().Visibility() == Visibility::Visible);
            VERIFY_IS_FALSE(board->DoneEmptyLabel().Text().empty());
            VERIFY_IS_TRUE(board->BlockedList().Visibility() == Visibility::Visible);
            VERIFY_IS_TRUE(board->TodoMore().Visibility() == Visibility::Collapsed);
            std::array<uint64_t, 7> counts{}; counts[1] = 5000;
            std::array<bool, 7> more{}; more[1] = true;
            board->SetPageInfo(counts, more);
            VERIFY_ARE_EQUAL(L"5000", board->TodoCount().Text());
            VERIFY_IS_TRUE(board->TodoMore().Visibility() == Visibility::Visible); VERIFY_IS_TRUE(board->TodoMore().IsEnabled());
            VERIFY_ARE_EQUAL(1u, board->TodoList().Items().Size());
            board->SetInteractionState(false, false, false, {}); VERIFY_IS_FALSE(board->TodoMore().IsEnabled());
        }));
    }

    void TaskBoardViewTests::LoadedCardsPreserveIdentityAndShowPendingOperations()
    {
        ::details::Event loaded;
        if (!loaded.IsValid()) { VERIFY_SUCCEEDED(HRESULT_FROM_WIN32(::GetLastError())); return; }
        winrt::com_ptr<ui::TaskBoardView> board;
        UIElement previous{ nullptr };
        bool mounted{};
        const std::wstring id{ L"10000000-0000-4000-8000-000000000001" };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::TaskItemViewModel> items{ nullptr };
        const auto mount = RunOnUIThread([&] {
            items = winrt::single_threaded_observable_vector<winrt::TerminalApp::TaskItemViewModel>();
            board = winrt::make_self<ui::TaskBoardView>(); board->Width(1100); board->Height(540);
            tasks::TaskProjection task; task.Id = id; task.Title = L"Task card with a frozen Agent";
            task.Workspace = L"Project A"; task.WorkspaceKey = L"project-a"; task.Status = tasks::TaskStatus::Todo;
            task.AgentName = L"Fixed Agent"; task.AgentRevision = 3; task.Priority = tasks::TaskPriority::High;
            task.HasActiveRun = true; task.LatestRunState = uint8_t{ 4 }; task.ActiveRunSummary = L"Running with long metadata that belongs in details";
            items.Append(winrt::make<ui::TaskItemViewModel>(task));
            board->SetItems(items); board->SetInteractionState(true, true, false, { id });
            const auto window = Window::Current(); previous = window.Content();
            board->Loaded([&](const auto&, const auto&) { loaded.Set(); });
            window.Content(*board); mounted = true; window.Activate();
        });
        const auto ready = SUCCEEDED(mount) ? Thread_Wait_For(loaded.m_handle, 10'000) : E_UNEXPECTED;
        HRESULT inspect{ E_UNEXPECTED };
        if (SUCCEEDED(ready)) inspect = RunOnUIThread([&] {
            board->UpdateLayout(); board->TodoList().UpdateLayout();
            const auto container = board->TodoList().ContainerFromIndex(0).try_as<ListViewItem>();
            VERIFY_IS_NOT_NULL(container); if (!container) return;
            const auto root = container.ContentTemplateRoot().try_as<FrameworkElement>();
            VERIFY_IS_NOT_NULL(root); if (!root) return;
            VERIFY_ARE_EQUAL(L"10000000", root.FindName(L"CardId").as<TextBlock>().Text());
            VERIFY_ARE_EQUAL(L"Fixed Agent · v3", root.FindName(L"CardAgent").as<TextBlock>().Text());
            VERIFY_IS_FALSE(root.FindName(L"CardRun").as<TextBlock>().Text().empty());
            VERIFY_IS_TRUE(root.FindName(L"CardRun").as<TextBlock>().Text() != L"Running with long metadata that belongs in details");
            VERIFY_IS_TRUE(root.FindName(L"CardPending").as<StackPanel>().Visibility() == Visibility::Visible);
            VERIFY_IS_TRUE(root.FindName(L"CardPendingRing").as<ProgressRing>().IsActive());
            VERIFY_IS_TRUE(root.FindName(L"CardProject").as<TextBlock>().Visibility() == Visibility::Visible);
            board->SetItems(items); board->SetInteractionState(true, false, false, {});
            VERIFY_IS_TRUE(container == board->TodoList().ContainerFromIndex(0));
            VERIFY_IS_TRUE(root == container.ContentTemplateRoot());
            VERIFY_IS_TRUE(root.FindName(L"CardPending").as<StackPanel>().Visibility() == Visibility::Collapsed);
            VERIFY_IS_FALSE(root.FindName(L"CardPendingRing").as<ProgressRing>().IsActive());
            VERIFY_IS_TRUE(root.FindName(L"CardProject").as<TextBlock>().Visibility() == Visibility::Collapsed);
        });
        const auto cleanup = RunOnUIThread([&] { if (mounted) Window::Current().Content(previous); previous = nullptr; board = nullptr; });
        VERIFY_SUCCEEDED(mount); if (SUCCEEDED(mount)) VERIFY_SUCCEEDED(ready);
        if (SUCCEEDED(ready)) VERIFY_SUCCEEDED(inspect);
        VERIFY_SUCCEEDED(cleanup);
    }
}
