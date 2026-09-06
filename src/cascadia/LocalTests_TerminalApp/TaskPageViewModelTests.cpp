// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "../TerminalApp/Tasks/TaskDetailsPane.h"
#include "../TerminalApp/Tasks/TaskPage.h"
#include "../TerminalApp/Tasks/TaskPageViewModel.h"
#include "../TerminalApp/Tasks/TaskProjectionCoordinator.h"
#include "../TerminalApp/Tasks/TaskProjectionMapper.h"
#include "../TerminalApp/Tasks/RuntimeTaskProjectionSource.h"
#include "../LocalTests_TaskRuntimeClient/TaskRuntimeClientTestSupport.h"
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
        dependencies.Connector->EnqueueSuccess(connection, ValidHelloResponse());
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
        dependencies.Connector->EnqueueSuccess(connection, ValidHelloResponse());
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
