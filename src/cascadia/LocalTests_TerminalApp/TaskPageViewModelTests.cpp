// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "../TerminalApp/Tasks/TaskDetailsPane.h"
#include "../TerminalApp/Tasks/TaskPage.h"
#include "../TerminalApp/Tasks/TaskPageViewModel.h"
#include "CppWinrtTailored.h"

using namespace WEX::TestExecution;

namespace TerminalAppLocalTests
{
    namespace tasks = winrt::TerminalApp::implementation::tasks;

    class RecordingTaskProjectionSource final : public tasks::ITaskProjectionSource
    {
    public:
        std::vector<tasks::TaskProjection> Items;
        std::wstring LastTaskId;
        std::optional<tasks::TaskCommand> LastCommand;
        std::optional<tasks::TaskDraft> LastDraft;
        bool LastStartImmediately{};

        std::vector<tasks::TaskProjection> Snapshot() const override
        {
            return Items;
        }

        tasks::TaskProjection CreateTask(const tasks::TaskDraft& draft, const bool startImmediately) override
        {
            LastDraft = draft;
            LastStartImmediately = startImmediately;

            tasks::TaskProjection task;
            task.Id = L"RWT-99";
            task.Workspace = draft.Workspace;
            task.WorkspaceKey = draft.Workspace;
            task.Title = draft.Title;
            task.Description = draft.Description;
            task.Status = startImmediately ? tasks::TaskStatus::InProgress : tasks::TaskStatus::Todo;
            task.TerminalState = startImmediately ? tasks::TaskTerminalState::Busy : tasks::TaskTerminalState::NotCreated;
            task.HasActiveRun = startImmediately;
            Items.insert(Items.begin(), task);
            return task;
        }

        std::optional<tasks::TaskProjection> Execute(const std::wstring& taskId, const tasks::TaskCommand command) override
        {
            LastTaskId = taskId;
            LastCommand = command;
            const auto task = std::find_if(Items.begin(), Items.end(), [&](const auto& item) { return item.Id == taskId; });
            return task == Items.end() ? std::nullopt : std::optional<tasks::TaskProjection>{ *task };
        }
    };

    class TaskPageViewModelTests
    {
        BEGIN_TEST_CLASS(TaskPageViewModelTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        TEST_METHOD(StartsEmptyAndLoadsInjectedProjection);
        TEST_METHOD(FiltersAndPreservesSelectionByTaskId);
        TEST_METHOD(RoutesCommandsAndDraftsThroughProjectionSource);
        TEST_METHOD(CreatesTaskPageXamlTypes);
        TEST_METHOD(PageFilteringPreservesSelectionByTaskId);
        TEST_METHOD(PrototypeResolvesStableWorkspaceKey);
    };

    tasks::TaskProjection _task(std::wstring id,
                                std::wstring workspace,
                                const tasks::TaskStatus status)
    {
        tasks::TaskProjection task;
        task.Id = std::move(id);
        task.Workspace = std::move(workspace);
        task.WorkspaceKey = task.Workspace;
        task.Title = task.Id + L" title";
        task.Description = L"description";
        task.Status = status;
        task.TerminalState = tasks::TaskTerminalState::Ready;
        return task;
    }

    void TaskPageViewModelTests::StartsEmptyAndLoadsInjectedProjection()
    {
        const auto result = RunOnUIThread([]() {
            const auto viewModel = winrt::make_self<winrt::TerminalApp::implementation::TaskPageViewModel>();
            VERIFY_IS_FALSE(viewModel->IsSourceAttached());
            VERIFY_ARE_EQUAL(0u, viewModel->Tasks().Size());
            VERIFY_IS_NULL(viewModel->SelectedTask());
            tasks::TaskDraft unavailableDraft;
            unavailableDraft.Title = L"Unavailable";
            VERIFY_IS_FALSE(viewModel->CreateTask(unavailableDraft, false));

            const auto source = std::make_shared<RecordingTaskProjectionSource>();
            source->Items = {
                _task(L"RWT-1", L"win_terminal", tasks::TaskStatus::Todo),
                _task(L"RWT-2", L"win_terminal", tasks::TaskStatus::InReview),
                _task(L"API-1", L"richerlife-api", tasks::TaskStatus::Blocked),
                _task(L"RWT-3", L"win_terminal", tasks::TaskStatus::Done),
            };
            viewModel->AttachSource(source);

            VERIFY_IS_TRUE(viewModel->IsSourceAttached());
            VERIFY_ARE_EQUAL(2u, viewModel->Tasks().Size());
            VERIFY_ARE_EQUAL(L"RWT-1", viewModel->SelectedTask().Id());
        });
        VERIFY_SUCCEEDED(result);
    }

    void TaskPageViewModelTests::FiltersAndPreservesSelectionByTaskId()
    {
        const auto result = RunOnUIThread([]() {
            const auto source = std::make_shared<RecordingTaskProjectionSource>();
            source->Items = {
                _task(L"RWT-1", L"win_terminal", tasks::TaskStatus::Todo),
                _task(L"RWT-2", L"win_terminal", tasks::TaskStatus::InReview),
                _task(L"API-1", L"richerlife-api", tasks::TaskStatus::Blocked),
            };
            const auto viewModel = winrt::make_self<winrt::TerminalApp::implementation::TaskPageViewModel>();
            viewModel->AttachSource(source);
            viewModel->SelectTask(viewModel->Tasks().GetAt(1));
            VERIFY_ARE_EQUAL(L"RWT-2", viewModel->SelectedTask().Id());

            viewModel->SetShowAllWorkspaces(true);
            VERIFY_ARE_EQUAL(3u, viewModel->Tasks().Size());
            VERIFY_ARE_EQUAL(L"RWT-2", viewModel->SelectedTask().Id());

            viewModel->SetSearchText(L"no matches");
            VERIFY_ARE_EQUAL(0u, viewModel->Tasks().Size());
            VERIFY_IS_NULL(viewModel->SelectedTask());

            viewModel->SetSearchText(L"");
            VERIFY_ARE_EQUAL(3u, viewModel->Tasks().Size());
            VERIFY_ARE_EQUAL(L"RWT-2", viewModel->SelectedTask().Id());
        });
        VERIFY_SUCCEEDED(result);
    }

    void TaskPageViewModelTests::RoutesCommandsAndDraftsThroughProjectionSource()
    {
        const auto result = RunOnUIThread([]() {
            const auto source = std::make_shared<RecordingTaskProjectionSource>();
            source->Items = { _task(L"RWT-1", L"win_terminal", tasks::TaskStatus::Todo) };
            const auto viewModel = winrt::make_self<winrt::TerminalApp::implementation::TaskPageViewModel>();
            viewModel->AttachSource(source);

            viewModel->ExecuteSelected(tasks::TaskCommand::Start);
            VERIFY_IS_TRUE(source->LastCommand.has_value());
            VERIFY_IS_TRUE(source->LastCommand.value() == tasks::TaskCommand::Start);
            VERIFY_ARE_EQUAL(L"RWT-1", source->LastTaskId);

            tasks::TaskDraft draft;
            draft.Workspace = L"win_terminal";
            draft.Title = L"New task";
            draft.Description = L"Goal";
            draft.Acceptance = { L"Criterion" };
            viewModel->CreateTask(draft, true);
            VERIFY_IS_TRUE(source->LastDraft.has_value());
            VERIFY_IS_TRUE(source->LastStartImmediately);
            VERIFY_ARE_EQUAL(L"New task", source->LastDraft->Title);
            VERIFY_ARE_EQUAL(L"RWT-99", viewModel->SelectedTask().Id());
        });
        VERIFY_SUCCEEDED(result);
    }

    void TaskPageViewModelTests::CreatesTaskPageXamlTypes()
    {
        const auto result = RunOnUIThread([]() {
            const winrt::TerminalApp::TaskPage page;
            const winrt::TerminalApp::TaskDetailsPane details;
            VERIFY_IS_NOT_NULL(page);
            VERIFY_IS_NOT_NULL(details);
            VERIFY_IS_NOT_NULL(page.ViewModel());
        });
        VERIFY_SUCCEEDED(result);
    }

    void TaskPageViewModelTests::PageFilteringPreservesSelectionByTaskId()
    {
        ::details::Event loadedEvent;
        if (!loadedEvent.IsValid())
        {
            VERIFY_SUCCEEDED(HRESULT_FROM_WIN32(::GetLastError()));
            return;
        }

        winrt::TerminalApp::TaskPage page{ nullptr };
        winrt::Windows::UI::Xaml::UIElement previousContent{ nullptr };
        bool contentReplaced{};
        const auto mountResult = RunOnUIThread([&]() {
            const auto source = std::make_shared<RecordingTaskProjectionSource>();
            source->Items = {
                _task(L"RWT-1", L"win_terminal", tasks::TaskStatus::Todo),
                _task(L"RWT-2", L"win_terminal", tasks::TaskStatus::InReview),
            };
            page = winrt::TerminalApp::TaskPage{};
            winrt::get_self<winrt::TerminalApp::implementation::TaskPage>(page)->AttachProjectionSource(source);

            const auto window = winrt::Windows::UI::Xaml::Window::Current();
            previousContent = window.Content();
            page.Loaded([&](auto&&, auto&&) { loadedEvent.Set(); });
            window.Content(page);
            contentReplaced = true;
            window.Activate();
        });

        HRESULT loadedResult{ E_UNEXPECTED };
        HRESULT interactionResult{ E_UNEXPECTED };
        if (SUCCEEDED(mountResult))
        {
            constexpr unsigned long loadedTimeoutMilliseconds{ 10'000 };
            loadedResult = Thread_Wait_For(loadedEvent.m_handle, loadedTimeoutMilliseconds);
        }
        if (SUCCEEDED(loadedResult))
        {
            interactionResult = RunOnUIThread([&]() {
                const auto list = page.FindName(L"TaskList").as<winrt::Windows::UI::Xaml::Controls::ListView>();
                const auto search = page.FindName(L"TaskSearchBox").as<winrt::Windows::UI::Xaml::Controls::TextBox>();
                const auto pageImpl = winrt::get_self<winrt::TerminalApp::implementation::TaskPage>(page);
                VERIFY_IS_TRUE(page.IsLoaded());
                VERIFY_ARE_EQUAL(2u, list.Items().Size());
                list.SelectedIndex(1);
                VERIFY_IS_NOT_NULL(page.ViewModel().SelectedTask());
                VERIFY_ARE_EQUAL(L"RWT-2", page.ViewModel().SelectedTask().Id());

                search.Text(L"no matches");
                pageImpl->_Search_TextChanged(search, winrt::Windows::UI::Xaml::Controls::TextChangedEventArgs{ nullptr });
                VERIFY_IS_NULL(page.ViewModel().SelectedTask());
                search.Text(L"");
                pageImpl->_Search_TextChanged(search, winrt::Windows::UI::Xaml::Controls::TextChangedEventArgs{ nullptr });
                VERIFY_ARE_EQUAL(L"RWT-2", page.ViewModel().SelectedTask().Id());
            });
        }

        const auto cleanupResult = RunOnUIThread([&]() {
            if (contentReplaced)
            {
                winrt::Windows::UI::Xaml::Window::Current().Content(previousContent);
            }
            previousContent = nullptr;
            page = nullptr;
        });

        VERIFY_SUCCEEDED(mountResult);
        if (SUCCEEDED(mountResult))
        {
            VERIFY_SUCCEEDED(loadedResult);
        }
        if (SUCCEEDED(loadedResult))
        {
            VERIFY_SUCCEEDED(interactionResult);
        }
        VERIFY_SUCCEEDED(cleanupResult);
    }

    void TaskPageViewModelTests::PrototypeResolvesStableWorkspaceKey()
    {
        const auto result = RunOnUIThread([]() {
            const auto source = tasks::MakePrototypeTaskProjectionSource();
            VERIFY_IS_TRUE(source->IsPrototype());
            VERIFY_ARE_EQUAL(std::wstring{ L"win_terminal" }, source->ResolveWorkspaceKey(L"workspace-id", LR"(F:\RicherLife\codes\win_terminal)", LR"(F:\RicherLife\codes\win_terminal\src\cascadia)"));
            VERIFY_ARE_EQUAL(std::wstring{ L"richerlife-api" }, source->ResolveWorkspaceKey(L"workspace-id", L"", LR"(F:\RicherLife\codes\richerlife-api\src)"));
            VERIFY_ARE_EQUAL(std::wstring{ L"win_terminal" }, source->ResolveWorkspaceKey(L"workspace-id", LR"(F:\RicherLife\task-worktrees\rwt-42)", LR"(F:\RicherLife\task-worktrees\rwt-42\src)"));
            VERIFY_IS_TRUE(source->ResolveWorkspaceKey(L"workspace-id", L"", LR"(F:\unrelated)").empty());
        });
        VERIFY_SUCCEEDED(result);
    }
}
