// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "../TerminalApp/Tasks/TaskDetailsPane.h"
#include "../TerminalApp/Tasks/TaskPage.h"
#include "../TerminalApp/Tasks/TaskPageViewModel.h"
#include "../TerminalApp/Tasks/TaskProjectionCoordinator.h"
#include "../TerminalApp/Tasks/TaskProjectionMapper.h"
#include "../TerminalApp/Tasks/TaskBoardView.h"
#include "../TerminalApp/Projects/ProjectNavigationControls.h"
#include "../TerminalApp/Tasks/RuntimeTaskProjectionSource.h"
#include "../LocalTests_TaskRuntimeClient/TaskRuntimeClientTestSupport.h"
#include "CppWinrtTailored.h"
#include "ProjectTestSource.h"

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
        ChangedHandler Handler;
        std::wstring LastMessage;

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

        bool QueueMessage(const std::wstring& taskId, const std::wstring& message) override
        {
            LastTaskId = taskId;
            LastMessage = message;
            return !message.empty();
        }

        void SetChangedHandler(ChangedHandler handler) override
        {
            Handler = std::move(handler);
        }

        void TriggerChanged() const
        {
            if (Handler) Handler();
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
        TEST_METHOD(ProjectionCoordinatorRejectsStaleGenerationAndAcceptsNewEpoch);
        TEST_METHOD(SourceChangeBatchesReloadAndPreservesSelection);
        TEST_METHOD(RuntimeProjectionMapsRunsMessagesAndDetails);
        TEST_METHOD(IdleRuntimeProjectionObservesExternalCreationAndDeletion);
        TEST_METHOD(ConfirmedAsyncCreationSurvivesAnOlderQueryResponse);
        TEST_METHOD(RuntimeSummarySourceLoadsTheNextPageWithoutLoadingHistory);
        TEST_METHOD(SummaryCacheKeepsSelectedDetailsAndSupportsFiveThousandRows);
        TEST_METHOD(UnchangedProjectionPreservesListContainers);
        TEST_METHOD(FollowupDraftSurvivesRefreshAndTaskSwitch);
        TEST_METHOD(BoardPolicySeparatesPlanningExecutionAndAcceptance);
        TEST_METHOD(ProjectLayoutAndMetadataDraftsSurviveViewChanges);
        TEST_METHOD(SnapshotProjectRegistrationWaitsForRuntimeAndKeepsIdentity);
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

    void TaskPageViewModelTests::SummaryCacheKeepsSelectedDetailsAndSupportsFiveThousandRows()
    {
        tasks::TaskProjectionCoordinator cache;
        std::vector<tasks::TaskProjection> page;
        for (size_t i = 0; i < 5000; ++i)
        {
            auto task = _task(L"task-" + std::to_wstring(i), L"project", tasks::TaskStatus::Todo);
            task.IsSummary = true; task.UpdatedAtUnixMilliseconds = 10000 - static_cast<int64_t>(i); page.push_back(std::move(task));
        }
        VERIFY_IS_TRUE(cache.ApplySummaries("epoch", page, {})); VERIFY_ARE_EQUAL(size_t{ 5000 }, cache.Size());
        auto detail = page.front(); detail.IsSummary = false; detail.Description = L"Complete task description";
        VERIFY_IS_TRUE(cache.Merge("epoch", 1, detail));
        VERIFY_IS_TRUE(cache.ApplySummaries("epoch", {}, detail.Id));
        VERIFY_ARE_EQUAL(size_t{ 1 }, cache.Size()); VERIFY_ARE_EQUAL(detail.Description, cache.Snapshot().front().Description);
        cache.RemoveTask("old-epoch", detail.Id); VERIFY_ARE_EQUAL(size_t{ 1 }, cache.Size());
        cache.RemoveTask("epoch", detail.Id); VERIFY_ARE_EQUAL(size_t{}, cache.Size());
        page.push_back(page.front()); VERIFY_IS_FALSE(cache.ApplySummaries("epoch", page, {}));
    }
    void TaskPageViewModelTests::UnchangedProjectionPreservesListContainers()
    {
        VERIFY_SUCCEEDED(RunOnUIThread([] {
            const auto source = std::make_shared<RecordingTaskProjectionSource>();
            source->Items = { _task(L"one", L"win_terminal", tasks::TaskStatus::Todo), _task(L"two", L"win_terminal", tasks::TaskStatus::Todo) };
            const auto view = winrt::make_self<winrt::TerminalApp::implementation::TaskPageViewModel>(); view->AttachSource(source);
            const auto first = view->Tasks().GetAt(0), second = view->Tasks().GetAt(1);
            source->TriggerChanged(); VERIFY_IS_TRUE(first == view->Tasks().GetAt(0)); VERIFY_IS_TRUE(second == view->Tasks().GetAt(1));
            ++source->Items[1].Version; source->Items[1].Title = L"Changed title"; source->TriggerChanged();
            VERIFY_IS_TRUE(first == view->Tasks().GetAt(0)); VERIFY_IS_FALSE(second == view->Tasks().GetAt(1));
            VERIFY_ARE_EQUAL(L"Changed title", view->Tasks().GetAt(1).Title());
        }));
    }
    void TaskPageViewModelTests::FollowupDraftSurvivesRefreshAndTaskSwitch()
    {
        VERIFY_SUCCEEDED(RunOnUIThread([] {
            const auto details = winrt::make_self<winrt::TerminalApp::implementation::TaskDetailsPane>();
            auto one = _task(L"one", L"project", tasks::TaskStatus::Todo), two = _task(L"two", L"project", tasks::TaskStatus::Todo);
            details->Task(winrt::make<winrt::TerminalApp::implementation::TaskItemViewModel>(one));
            const auto input = details->FindName(L"FollowupInput").as<winrt::Windows::UI::Xaml::Controls::TextBox>();
            input.Text(L"first draft"); input.Select(3, 2);
            ++one.Version; one.RunDetails.push_back(L"new log entry");
            details->Task(winrt::make<winrt::TerminalApp::implementation::TaskItemViewModel>(one));
            VERIFY_ARE_EQUAL(L"first draft", input.Text()); VERIFY_ARE_EQUAL(3, input.SelectionStart());
            details->Task(winrt::make<winrt::TerminalApp::implementation::TaskItemViewModel>(two));
            VERIFY_IS_TRUE(input.Text().empty()); input.Text(L"second draft");
            details->Task(winrt::make<winrt::TerminalApp::implementation::TaskItemViewModel>(one));
            VERIFY_ARE_EQUAL(L"first draft", input.Text());
        }));
    }
    void TaskPageViewModelTests::BoardPolicySeparatesPlanningExecutionAndAcceptance()
    {
        namespace protocol = Microsoft::Terminal::TaskRuntimeProtocol;
        auto task = _task(L"one", L"project", tasks::TaskStatus::Todo);
        auto action = tasks::ResolveBoardAction(task, tasks::TaskStatus::InProgress);
        VERIFY_IS_TRUE(action.Allowed && action.Operation == protocol::TaskBoardOperation::Start);
        VERIFY_IS_FALSE(tasks::ResolveBoardAction(task, tasks::TaskStatus::Done).Allowed);
        task.Status = tasks::TaskStatus::InProgress; task.HasActiveRun = true;
        action = tasks::ResolveBoardAction(task, tasks::TaskStatus::Todo);
        VERIFY_IS_TRUE(action.Allowed && action.Confirm && action.Operation == protocol::TaskBoardOperation::StopThenMove);
        VERIFY_IS_FALSE(tasks::ResolveBoardAction(task, tasks::TaskStatus::InReview).Allowed);
        task.HasActiveRun = false; action = tasks::ResolveBoardAction(task, tasks::TaskStatus::InReview);
        VERIFY_IS_TRUE(action.Allowed && action.RequiresText);
        task.Status = tasks::TaskStatus::InReview; task.HasPendingMessages = true;
        VERIFY_IS_FALSE(tasks::ResolveBoardAction(task, tasks::TaskStatus::Done).Allowed);
        task.HasPendingMessages = false; action = tasks::ResolveBoardAction(task, tasks::TaskStatus::InProgress);
        VERIFY_IS_TRUE(action.Allowed && action.RequiresText && action.Operation == protocol::TaskBoardOperation::Continue);
        task.Status = tasks::TaskStatus::Done; action = tasks::ResolveBoardAction(task, tasks::TaskStatus::Todo);
        VERIFY_IS_TRUE(action.Allowed && action.Operation == protocol::TaskBoardOperation::ReopenOnly);
        task.ArchivedProject = true; VERIFY_IS_FALSE(tasks::ResolveBoardAction(task, tasks::TaskStatus::Todo).Allowed);
    }
    void TaskPageViewModelTests::ProjectLayoutAndMetadataDraftsSurviveViewChanges()
    {
        VERIFY_SUCCEEDED(RunOnUIThread([] {
            namespace projects = winrt::TerminalApp::implementation::projects;
            const std::string first{ "10000000-0000-4000-8000-000000000001" }, second{ "20000000-0000-4000-8000-000000000002" };
            projects::ProjectNavigationState state{ nullptr }; state.Open(first); state.Open(second); state.Show(projects::ProjectView::Tasks);
            projects::ProjectNavigationState restored{ nullptr }; VERIFY_IS_TRUE(restored.RestoreLayout(state.SerializeLayout()));
            VERIFY_ARE_EQUAL(second, restored.ActiveProjectId); VERIFY_IS_TRUE(restored.CurrentView() == projects::ProjectView::Tasks);
            restored.CloseView(second); VERIFY_ARE_EQUAL(first, restored.ActiveProjectId); restored.CloseView(first);
            VERIFY_IS_TRUE(restored.ActiveProjectId.empty()); VERIFY_IS_TRUE(restored.RestoreLayout(restored.SerializeLayout()));
            VERIFY_IS_FALSE(restored.RestoreLayout("{\"version\":1,\"active\":\"invalid\",\"open\":[]}"));
            projects::protocol::ProjectProjection project; project.Id = first; project.Name = "First"; project.Version = 1;
            state.Catalog[first] = project; project.Id = second; project.Name = "Second"; state.Catalog[second] = project;
            projects::ProjectNavigationControls controls; state.Open(first); controls.Render(state, false);
            controls.Content.FindName(L"Name").as<winrt::Windows::UI::Xaml::Controls::TextBox>().Text(L"Unsaved name");
            state.Open(second); controls.Render(state, false); state.Open(first); controls.Render(state, false);
            VERIFY_ARE_EQUAL(std::wstring{ L"Unsaved name" }, controls.Draft().Name);
            const auto board = winrt::make_self<winrt::TerminalApp::implementation::TaskBoardView>();
            VERIFY_IS_NOT_NULL(board->FindName(L"TodoList")); VERIFY_IS_NOT_NULL(board->FindName(L"BlockedList"));
        }));
    }

    void TaskPageViewModelTests::SnapshotProjectRegistrationWaitsForRuntimeAndKeepsIdentity()
    {
        const auto source = std::make_shared<ProjectTestSource>(); source->IsConnected = false;
        const auto state = std::make_shared<project_ui::ProjectNavigationState>(source);
        const std::string id{ "10000000-0000-4000-8000-000000000001" };
        state->RegisterLegacy(id, "Saved layout"); VERIFY_IS_TRUE(source->Commands.empty());
        source->IsConnected = true; state->Refresh();
        VERIFY_ARE_EQUAL(size_t{ 1 }, source->Commands.size()); VERIFY_ARE_EQUAL(id, source->Commands.front().ProjectId);
        VERIFY_IS_TRUE(source->Commands.front().Operation == project_ui::protocol::ProjectCatalogOperation::RegisterLegacy);
        state->Refresh(); VERIFY_ARE_EQUAL(size_t{ 1 }, source->Commands.size());
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
            VERIFY_IS_TRUE(viewModel->QueueSelectedMessage(L"follow up"));
            VERIFY_ARE_EQUAL(std::wstring{ L"follow up" }, source->LastMessage);

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
            const auto detailsHost = details.FindName(L"DetailsContentHost").as<winrt::Windows::UI::Xaml::Controls::ContentControl>();
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::HorizontalAlignment::Stretch, detailsHost.HorizontalContentAlignment());
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::VerticalAlignment::Stretch, detailsHost.VerticalContentAlignment());
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
                const auto details = page.FindName(L"DetailsPane").as<winrt::TerminalApp::TaskDetailsPane>();
                const auto pageImpl = winrt::get_self<winrt::TerminalApp::implementation::TaskPage>(page);
                VERIFY_IS_TRUE(page.IsLoaded());
                VERIFY_ARE_EQUAL(2u, list.Items().Size());
                list.SelectedIndex(1);
                VERIFY_IS_NOT_NULL(page.ViewModel().SelectedTask());
                VERIFY_ARE_EQUAL(L"RWT-2", page.ViewModel().SelectedTask().Id());
                VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible, details.Visibility());

                search.Text(L"no matches");
                pageImpl->_Search_TextChanged(search, winrt::Windows::UI::Xaml::Controls::TextChangedEventArgs{ nullptr });
                VERIFY_IS_NULL(page.ViewModel().SelectedTask());
                VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed, details.Visibility());
                search.Text(L"");
                pageImpl->_Search_TextChanged(search, winrt::Windows::UI::Xaml::Controls::TextChangedEventArgs{ nullptr });
                VERIFY_ARE_EQUAL(L"RWT-2", page.ViewModel().SelectedTask().Id());
                VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible, details.Visibility());
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
            VERIFY_ARE_EQUAL(std::wstring{ L"win_terminal" }, source->ResolveWorkspaceKey(L"workspace-id", L"", LR"(C:\Users\u1s1)"));
        });
        VERIFY_SUCCEEDED(result);
    }

    void TaskPageViewModelTests::ProjectionCoordinatorRejectsStaleGenerationAndAcceptsNewEpoch()
    {
        tasks::TaskProjectionCoordinator coordinator;
        auto first = _task(L"RWT-1", L"workspace", tasks::TaskStatus::Todo);
        first.UpdatedAtUnixMilliseconds = 10;
        VERIFY_IS_TRUE(coordinator.Apply("runtime-a", 2, { first }));
        VERIFY_IS_FALSE(coordinator.Apply("runtime-a", 1, { _task(L"stale", L"workspace", tasks::TaskStatus::Todo) }));
        VERIFY_ARE_EQUAL(size_t{ 1 }, coordinator.Size());
        VERIFY_ARE_EQUAL(std::wstring{ L"RWT-1" }, coordinator.Snapshot().front().Id);
        auto detail = first;
        detail.Title = L"Detailed title";
        VERIFY_IS_TRUE(coordinator.Merge("runtime-a", 2, detail));
        VERIFY_ARE_EQUAL(std::wstring{ L"Detailed title" }, coordinator.Snapshot().front().Title);
        VERIFY_IS_FALSE(coordinator.Merge("runtime-a", 1, detail));

        auto restarted = _task(L"RWT-2", L"workspace", tasks::TaskStatus::InProgress);
        restarted.UpdatedAtUnixMilliseconds = 20;
        VERIFY_IS_TRUE(coordinator.Apply("runtime-b", 1, { restarted }));
        VERIFY_ARE_EQUAL(uint64_t{ 1 }, coordinator.Generation());
        VERIFY_ARE_EQUAL(std::wstring{ L"RWT-2" }, coordinator.Snapshot().front().Id);

        std::vector<tasks::TaskProjection> oversized(513, restarted);
        VERIFY_IS_FALSE(coordinator.Apply("runtime-b", 2, std::move(oversized)));
    }

    void TaskPageViewModelTests::SourceChangeBatchesReloadAndPreservesSelection()
    {
        const auto result = RunOnUIThread([]() {
            const auto source = std::make_shared<RecordingTaskProjectionSource>();
            source->Items = {
                _task(L"RWT-1", L"win_terminal", tasks::TaskStatus::Todo),
                _task(L"RWT-2", L"win_terminal", tasks::TaskStatus::InReview),
            };
            const auto viewModel = winrt::make_self<winrt::TerminalApp::implementation::TaskPageViewModel>();
            viewModel->AttachSource(source);
            viewModel->SelectTask(viewModel->Tasks().GetAt(1));
            source->Items.insert(source->Items.begin(), _task(L"RWT-3", L"win_terminal", tasks::TaskStatus::Todo));
            source->TriggerChanged();
            VERIFY_ARE_EQUAL(3u, viewModel->Tasks().Size());
            VERIFY_ARE_EQUAL(L"RWT-2", viewModel->SelectedTask().Id());
            viewModel->DetachSource();
            VERIFY_IS_FALSE(static_cast<bool>(source->Handler));
        });
        VERIFY_SUCCEEDED(result);
    }

    void TaskPageViewModelTests::IdleRuntimeProjectionObservesExternalCreationAndDeletion()
    {
        using namespace TaskRuntimeClientLocalTests;
        using namespace TaskRuntimeClientLocalTests::TestSupport;
        namespace protocol = ::Microsoft::Terminal::TaskRuntimeProtocol;
        RuntimeClientDependenciesFixture dependencies;
        dependencies.EnqueueRequestIds();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        std::atomic_bool visible{};
        std::atomic_uint64_t generation{};
        connection->SetAutoResponseHandler([&](const protocol::ControlRequest& request) {
            auto response = EmptySuccessResponse(request);
            if (request.Method == protocol::ControlMethod::QueryTask)
            {
                protocol::TaskProjectionQueryResult snapshot;
                snapshot.Generation = ++generation;
                if (visible.load())
                {
                    protocol::TaskProjectionRecord task;
                    task.Id = "10000000-0000-4000-8000-000000000001";
                    task.WorkspaceId = "20000000-0000-4000-8000-000000000002";
                    task.Title = "Created in another window";
                    task.Version = 1;
                    task.CreatedAtUnixMilliseconds = 1000;
                    task.UpdatedAtUnixMilliseconds = 1000;
                    task.Execution.Id = "30000000-0000-4000-8000-000000000003";
                    task.Execution.Revision = 1;
                    task.Execution.Mode = protocol::TaskProjectionExecutionMode::Direct;
                    task.Execution.SourceRoot = "C:/src";
                    task.Execution.RuntimeRoot = "C:/src";
                    task.Execution.Provider = "codex";
                    task.LastEventSequence = 1;
                    snapshot.Tasks.push_back(std::move(task));
                }
                response.ResultJson = protocol::EncodeTaskProjectionQueryResult(snapshot).Payload;
            }
            return response;
        });
        connection->EnableAutoResponse();
        auto hello = ValidHelloResponse();
        hello.Capabilities &= ~static_cast<uint64_t>(protocol::ProtocolCapability::TaskBoard);
        dependencies.Connector->EnqueueSuccess(connection, hello);
        auto configuration = ValidConfiguration();
        configuration.Hello.ClientKind = protocol::ProtocolClientKind::TerminalUi;
        tasks::RuntimeTaskProjectionSource source{ configuration, nullptr, nullptr, dependencies.Dependencies() };
        VERIFY_IS_TRUE(source.Start());
        VERIFY_IS_TRUE(WaitUntil([&] { return !source.IsLoading(); }));
        VERIFY_IS_TRUE(source.Snapshot().empty());
        visible = true;
        VERIFY_IS_TRUE(WaitUntil([&] { return source.Snapshot().size() == 1; }, 5000));
        VERIFY_IS_FALSE(source.Snapshot().front().HasActiveRun);
        visible = false;
        VERIFY_IS_TRUE(WaitUntil([&] { return source.Snapshot().empty(); }, 5000));
        source.Stop();
    }

    void TaskPageViewModelTests::ConfirmedAsyncCreationSurvivesAnOlderQueryResponse()
    {
        using namespace TaskRuntimeClientLocalTests;
        using namespace TaskRuntimeClientLocalTests::TestSupport;
        namespace protocol = ::Microsoft::Terminal::TaskRuntimeProtocol;
        RuntimeClientDependenciesFixture dependencies; dependencies.EnqueueRequestIds();
        auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        auto hello = ValidHelloResponse();
        hello.Capabilities &= ~static_cast<uint64_t>(protocol::ProtocolCapability::TaskBoard);
        dependencies.Connector->EnqueueSuccess(connection, hello);
        auto configuration = ValidConfiguration(); configuration.Hello.ClientKind = protocol::ProtocolClientKind::TerminalUi;
        auto source = std::make_shared<tasks::RuntimeTaskProjectionSource>(configuration, nullptr, nullptr, dependencies.Dependencies());
        VERIFY_IS_TRUE(source->Start());
        VERIFY_IS_TRUE(WaitUntil([&] { return std::ranges::any_of(connection->OutboundRequests(), [](const auto& request) { return request.Method == protocol::ControlMethod::QueryTask; }); }));
        const auto first = connection->OutboundRequests();
        const auto older = *std::ranges::find_if(first, [](const auto& request) { return request.Method == protocol::ControlMethod::QueryTask; });
        tasks::TaskCreationAttempt attempt;
        attempt.TaskId = "10000000-0000-4000-8000-000000000011";
        attempt.CommandId = "10000000-0000-4000-8000-000000000012";
        attempt.Draft.WorkspaceId = L"10000000-0000-4000-8000-000000000013";
        attempt.Draft.Title = L"Confirmed task"; attempt.Draft.SourceRoot = L"C:/src"; attempt.Draft.UseWorktree = false;
        auto completed = std::make_shared<std::promise<tasks::TaskCreationResult>>(); auto future = completed->get_future();
        source->CreateTaskAsync(attempt, [completed](auto result) { completed->set_value(std::move(result)); });
        VERIFY_IS_TRUE(WaitUntil([&] { return std::ranges::any_of(connection->OutboundRequests(), [](const auto& request) { return request.Method == protocol::ControlMethod::SubmitCommand; }); }));
        const auto sent = connection->OutboundRequests();
        const auto create = *std::ranges::find_if(sent, [](const auto& request) { return request.Method == protocol::ControlMethod::SubmitCommand; });
        auto response = EmptySuccessResponse(create);
        protocol::TaskUiCommandResult receipt; receipt.CommandId = attempt.CommandId; receipt.TaskId = attempt.TaskId; receipt.TaskVersion = receipt.EventSequence = 1;
        response.ResultJson = protocol::EncodeTaskUiCommandResult(receipt).Payload;
        VERIFY_IS_TRUE(connection->QueueInboundResponse(response));
        VERIFY_IS_TRUE(future.wait_for(std::chrono::seconds{ 5 }) == std::future_status::ready);
        VERIFY_IS_TRUE(future.get().Created); VERIFY_ARE_EQUAL(size_t{ 1 }, source->Snapshot().size());
        auto late = EmptySuccessResponse(older);
        late.ResultJson = protocol::EncodeTaskProjectionQueryResult({ 1, {} }).Payload;
        VERIFY_IS_TRUE(connection->QueueInboundResponse(late));
        VERIFY_IS_TRUE(WaitUntil([&] {
            return std::ranges::count_if(connection->OutboundRequests(), [](const auto& request) { return request.Method == protocol::ControlMethod::QueryTask; }) >= 2;
        }));
        VERIFY_ARE_EQUAL(size_t{ 1 }, source->Snapshot().size());
        VERIFY_ARE_EQUAL(std::wstring{ L"10000000-0000-4000-8000-000000000011" }, source->Snapshot().front().Id);
        source->Stop();
    }

    void TaskPageViewModelTests::RuntimeSummarySourceLoadsTheNextPageWithoutLoadingHistory()
    {
        using namespace TaskRuntimeClientLocalTests;
        using namespace TaskRuntimeClientLocalTests::TestSupport;
        namespace protocol = ::Microsoft::Terminal::TaskRuntimeProtocol;
        RuntimeClientDependenciesFixture dependencies; dependencies.EnqueueRequestIds();
        const auto connection = std::make_shared<FakeRuntimeClientConnectionState>();
        connection->SetAutoResponseHandler([](const protocol::ControlRequest& request) {
            auto response = EmptySuccessResponse(request);
            if (request.Method == protocol::ControlMethod::QueryTaskSummaries)
            {
                const auto query = protocol::DecodeTaskSummaryQueryRequest(request.PayloadJson); VERIFY_IS_TRUE(query.Succeeded());
                protocol::TaskSummaryQueryResult page; page.Generation = 1; page.Total = page.Counts[1] = 51;
                const size_t begin = query.Value.AfterId.empty() ? 0 : 50;
                const auto end = (std::min)(size_t{ 51 }, begin + query.Value.Limit);
                for (size_t i = begin; i < end; ++i)
                {
                    protocol::TaskSummaryProjection task;
                    task.Id = fmt::format("10000000-0000-4000-8000-{:012x}", i + 1);
                    task.ProjectId = "20000000-0000-4000-8000-000000000001"; task.ProjectName = "Project"; task.Title = "Paged task";
                    task.Version = 1; task.ColumnRevision = 1; task.ExecutionContextId = "30000000-0000-4000-8000-000000000001";
                    task.UpdatedAtUnixMilliseconds = 1000 - static_cast<int64_t>(i); page.Tasks.push_back(std::move(task));
                }
                if (end < 51) { page.NextAfterId = page.Tasks.back().Id; page.NextAfterValue = page.Tasks.back().UpdatedAtUnixMilliseconds; }
                const auto encoded = protocol::EncodeTaskSummaryQueryResult(page); VERIFY_IS_TRUE(encoded.Succeeded); response.ResultJson = encoded.Payload;
            }
            return response;
        });
        connection->EnableAutoResponse(); dependencies.Connector->EnqueueSuccess(connection, ValidHelloResponse());
        auto configuration = ValidConfiguration(); configuration.Hello.ClientKind = protocol::ProtocolClientKind::TerminalUi;
        tasks::RuntimeTaskProjectionSource source{ configuration, nullptr, nullptr, dependencies.Dependencies() };
        tasks::TaskListQuery query; query.Statuses.fill(false); query.Statuses[1] = true; source.ConfigureQuery(query);
        VERIFY_IS_TRUE(source.Start()); VERIFY_IS_TRUE(WaitUntil([&] { return source.Snapshot().size() == 50; }, 6000));
        VERIFY_ARE_EQUAL(uint64_t{ 51 }, source.QueryTotal()); VERIFY_IS_TRUE(source.HasMoreTasks());
        source.LoadMore(); VERIFY_IS_TRUE(WaitUntil([&] { return source.Snapshot().size() == 51; }, 6000));
        VERIFY_IS_FALSE(source.HasMoreTasks());
        VERIFY_IS_TRUE(std::ranges::none_of(connection->OutboundRequests(), [](const auto& request) { return request.Method == protocol::ControlMethod::QueryTask; }));
        source.Stop();
    }

    void TaskPageViewModelTests::RuntimeProjectionMapsRunsMessagesAndDetails()
    {
        const auto result = RunOnUIThread([]() {
            namespace protocol = ::Microsoft::Terminal::TaskRuntimeProtocol;
            protocol::TaskProjectionRecord source;
            source.Id = "10000000-0000-4000-8000-000000000001";
            source.WorkspaceId = "20000000-0000-4000-8000-000000000002";
            source.Title = "Mapped task";
            source.Description = "Mapped details";
            source.Status = protocol::TaskProjectionStatus::InProgress;
            source.Priority = protocol::TaskProjectionPriority::High;
            source.Version = 4;
            source.CreatedAtUnixMilliseconds = 1000;
            source.UpdatedAtUnixMilliseconds = 2000;
            source.Execution.Id = "30000000-0000-4000-8000-000000000003";
            source.Execution.Revision = 1;
            source.Execution.SourceRoot = "C:/src";
            source.Execution.RuntimeRoot = "C:/src";
            source.Execution.Provider = "codex";
            source.Terminal.State = protocol::TaskProjectionTerminalState::Exited;
            protocol::TaskProjectionRunRecord run;
            run.Id = "40000000-0000-4000-8000-000000000004";
            run.ExecutionContextId = source.Execution.Id;
            run.State = protocol::TaskProjectionRunState::Running;
            run.Sequence = 1;
            run.CreatedAtUnixMilliseconds = 1500;
            source.Runs.push_back(run);
            source.Messages.push_back({ "50000000-0000-4000-8000-000000000005", 0, 1, 1, "queued follow-up", std::nullopt, 1600, 1600 });
            source.Events.push_back({ "60000000-0000-4000-8000-000000000006", run.Id, 2, 1, "Provider_tool_progress", R"({"text":"building"})", 1700 });
            source.LastEventSequence = 2;
            const auto mapped = tasks::MapTaskProjection(source);
            VERIFY_IS_TRUE(mapped.has_value());
            VERIFY_IS_TRUE(mapped->HasActiveRun);
            VERIFY_ARE_EQUAL(size_t{ 1 }, mapped->Runs.size());
            VERIFY_ARE_EQUAL(size_t{ 1 }, mapped->QueuedMessages.size());
            VERIFY_ARE_EQUAL(size_t{ 1 }, mapped->RunDetails.size());
            VERIFY_IS_TRUE(mapped->RunDetails.front().find(L"building") != std::wstring::npos);
            VERIFY_ARE_EQUAL(uint64_t{ 4 }, mapped->Version);
        });
        VERIFY_SUCCEEDED(result);
    }
}
