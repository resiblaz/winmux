// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskProjectionSource.h"

#include "TaskProjectionMapper.h"

#include "RuntimeCrypto.h"
#include "RuntimeIdentity.h"
#include "TaskProjectionRpc.h"

#include <wil/resource.h>

namespace winrt::TerminalApp::implementation::tasks
{
    using namespace ::Microsoft::Terminal::TaskRuntime;
    using namespace ::Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        std::optional<std::string> _uuid()
        {
            return NewRuntimeUuid();
        }

        std::wstring _resource(const wchar_t* key)
        {
            const auto value = RS_(key);
            return std::wstring{ std::wstring_view{ value } };
        }

        TaskProjection _optimistic(
            const TaskDraft& draft,
            const std::string_view id,
            const bool startImmediately)
        {
            TaskProjection result;
            result.Id = til::u8u16(id);
            result.WorkspaceKey = draft.WorkspaceId;
            result.Workspace = draft.Workspace.empty() ? draft.WorkspaceId : draft.Workspace;
            result.Title = draft.Title;
            result.Description = draft.Description;
            result.Status = TaskStatus::Todo;
            result.TerminalState = TaskTerminalState::NotCreated;
            result.Priority = TaskPriority::Medium;
            result.UpdatedLabel = _resource(L"TaskPage_UpdatedJustNow");
            result.ExecutionMode = _resource(draft.UseWorktree ? L"TaskPage_WorktreeExecutionMode" : L"TaskPage_DirectExecutionMode");
            result.WorkingDirectory = draft.SourceRoot;
            result.Worktree = draft.UseWorktree ? _resource(L"TaskPage_WorktreePreparing") : _resource(L"TaskPage_NotApplicable");
            result.Branch = draft.UseWorktree ? _resource(L"TaskPage_WorktreePreparing") : _resource(L"TaskPage_CurrentBranch");
            result.Version = 1;
            result.SupportsTerminalSurface = false;
            result.HasActiveRun = startImmediately;
            result.ActiveRunSummary = startImmediately ? _resource(L"TaskPage_NewTaskRunQueued") : std::wstring{};
            for (const auto& item : draft.Acceptance) result.Acceptance.push_back({ item, false });
            return result;
        }

        std::optional<TaskRuntimeClientConfiguration> _configuration()
        {
            try
            {
                const auto settingsRoot = std::filesystem::path{
                    std::wstring_view{ ::winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings::SettingsPath() }
                }.parent_path();
                auto identity = BuildCurrentUserRuntimeIdentity(L"dev", settingsRoot);
                const auto clientId = _uuid();
                if (!identity.Succeeded() || !clientId) return std::nullopt;
                const auto module = std::filesystem::path{ wil::GetModuleFileNameW<std::wstring>(nullptr) };
                const auto runtime = module.parent_path() / L"TaskRuntime.exe";
                if (!std::filesystem::is_regular_file(runtime)) return std::nullopt;
                TaskRuntimeClientConfiguration configuration;
                configuration.Identity = std::move(identity.Identity);
                configuration.Hello.ClientInstanceId = *clientId;
                configuration.Hello.ClientKind = ProtocolClientKind::TerminalUi;
                configuration.Hello.ExpectedRuntimeIdentity = configuration.Identity.ProtocolIdentity;
                configuration.Hello.RequiredCapabilities =
                    static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
                    static_cast<uint64_t>(ProtocolCapability::TaskCommands);
                configuration.Hello.OptionalCapabilities =
                    static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
                configuration.Activation = TaskRuntimeClientActivationConfiguration{
                    runtime,
                    module.parent_path(),
                };
                return configuration;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
    }

    RuntimeTaskProjectionSource::RuntimeTaskProjectionSource(
        TaskRuntimeClientConfiguration configuration,
        Windows::UI::Core::CoreDispatcher dispatcher) :
        _client{ std::make_shared<TaskRuntimeClient>(std::move(configuration)) },
        _dispatcher{ std::move(dispatcher) }
    {
    }

    RuntimeTaskProjectionSource::~RuntimeTaskProjectionSource()
    {
        Stop();
    }

    std::vector<TaskProjection> RuntimeTaskProjectionSource::Snapshot() const
    {
        return _coordinator.Snapshot();
    }

    void RuntimeTaskProjectionSource::SetChangedHandler(ChangedHandler handler)
    {
        const std::scoped_lock lock{ _mutex };
        _changed = std::move(handler);
    }

    bool RuntimeTaskProjectionSource::Start()
    {
        const std::scoped_lock lock{ _mutex };
        if (_started) return true;
        if (_stopping || !_client || !_client->Valid() || !_client->Start()) return false;
        try
        {
            _work.push_back({ WorkKind::Refresh, std::nullopt, std::nullopt });
            _worker = std::thread{ [this] { _run(); } };
            _started = true;
            _condition.notify_one();
            return true;
        }
        catch (...)
        {
            _client->RequestStop();
            static_cast<void>(_client->WaitForStopped());
            _work.clear();
            return false;
        }
    }

    bool RuntimeTaskProjectionSource::IsLoading() const noexcept
    {
        return _loading.load(std::memory_order_acquire);
    }

    void RuntimeTaskProjectionSource::Stop() noexcept
    {
        {
            const std::scoped_lock lock{ _mutex };
            if (_stopping) return;
            _stopping = true;
            _changed = {};
            _work.clear();
        }
        _condition.notify_all();
        if (_client) _client->RequestStop();
        if (_worker.joinable()) _worker.join();
        if (_client) static_cast<void>(_client->WaitForStopped());
    }

    bool RuntimeTaskProjectionSource::_enqueue(WorkItem item)
    {
        const std::scoped_lock lock{ _mutex };
        if (!_started || _stopping || _work.size() >= 256) return false;
        _work.emplace_back(std::move(item));
        _condition.notify_one();
        return true;
    }

    void RuntimeTaskProjectionSource::SelectTask(const std::wstring_view taskId)
    {
        std::string selected;
        try
        {
            selected = til::u16u8(taskId);
        }
        catch (...)
        {
            return;
        }
        {
            const std::scoped_lock lock{ _mutex };
            if (!_started || _stopping || selected == _selectedTaskId) return;
            _selectedTaskId = selected;
            if (_work.size() >= 256) return;
            _work.push_back({ WorkKind::Refresh, std::nullopt, selected.empty() ? std::nullopt : std::optional<std::string>{ selected } });
        }
        _condition.notify_one();
    }

    TaskProjection RuntimeTaskProjectionSource::CreateTask(
        const TaskDraft& draft,
        const bool startImmediately)
    {
        const auto taskId = _uuid();
        const auto createId = _uuid();
        if (!taskId || !createId || draft.WorkspaceId.empty() || draft.SourceRoot.empty()) return {};
        TaskUiCommandRequest create;
        create.CommandId = *createId;
        create.Operation = TaskUiCommandOperation::Create;
        create.TaskId = *taskId;
        create.Draft = TaskUiCreateDraft{};
        create.Draft->WorkspaceId = til::u16u8(draft.WorkspaceId);
        create.Draft->Title = til::u16u8(draft.Title);
        create.Draft->Description = til::u16u8(draft.Description);
        for (const auto& item : draft.Acceptance) create.Draft->Acceptance.emplace_back(til::u16u8(item));
        create.Draft->ExecutionMode = draft.UseWorktree ? TaskProjectionExecutionMode::Worktree : TaskProjectionExecutionMode::Direct;
        create.Draft->SourceRoot = til::u16u8(draft.SourceRoot);
        create.Draft->RelativeWorkingDirectory = til::u16u8(draft.RelativeWorkingDirectory);
        create.Draft->Model = til::u16u8(draft.Model);
        create.Draft->Instructions = til::u16u8(draft.Instructions);
        auto projection = _optimistic(draft, *taskId, startImmediately);
        if (!_enqueue({ WorkKind::Command, create, std::nullopt })) return {};
        _coordinator.UpsertOptimistic(projection);
        if (startImmediately)
        {
            const auto startId = _uuid();
            if (startId)
            {
                TaskUiCommandRequest start;
                start.CommandId = *startId;
                start.Operation = TaskUiCommandOperation::Start;
                start.TaskId = *taskId;
                start.ExpectedTaskVersion = 1;
                static_cast<void>(_enqueue({ WorkKind::Command, start, std::nullopt }));
            }
        }
        _notify();
        return projection;
    }

    std::optional<TaskProjection> RuntimeTaskProjectionSource::Execute(
        const std::wstring& taskId,
        const TaskCommand command)
    {
        const auto snapshot = Snapshot();
        const auto task = std::ranges::find(snapshot, taskId, &TaskProjection::Id);
        if (task == snapshot.end()) return std::nullopt;
        TaskUiCommandOperation operation;
        switch (command)
        {
        case TaskCommand::Start: operation = TaskUiCommandOperation::Start; break;
        case TaskCommand::Stop: operation = TaskUiCommandOperation::Stop; break;
        case TaskCommand::CloseTerminal: operation = TaskUiCommandOperation::Close; break;
        case TaskCommand::MarkDone: operation = TaskUiCommandOperation::MarkDone; break;
        case TaskCommand::Reopen: operation = TaskUiCommandOperation::Reopen; break;
        case TaskCommand::OpenTerminal:
        default: return *task;
        }
        const auto commandId = _uuid();
        if (!commandId) return std::nullopt;
        TaskUiCommandRequest request;
        request.CommandId = *commandId;
        request.Operation = operation;
        request.TaskId = til::u16u8(task->Id);
        request.ExpectedTaskVersion = task->Version;
        return _enqueue({ WorkKind::Command, request, std::nullopt }) ? std::optional<TaskProjection>{ *task } : std::nullopt;
    }

    std::wstring RuntimeTaskProjectionSource::ResolveWorkspaceKey(
        const std::wstring_view workspaceId,
        const std::wstring_view,
        const std::wstring_view) const
    {
        return std::wstring{ workspaceId };
    }

    void RuntimeTaskProjectionSource::_run() noexcept
    {
        for (;;)
        {
            WorkItem item;
            {
                std::unique_lock lock{ _mutex };
                const auto signaled = _condition.wait_for(
                    lock,
                    std::chrono::seconds{ 2 },
                    [&] { return _stopping || !_work.empty(); });
                if (_stopping) return;
                if (signaled)
                {
                    item = std::move(_work.front());
                    _work.pop_front();
                }
                else
                {
                    const auto snapshot = _coordinator.Snapshot();
                    if (!std::ranges::any_of(snapshot, &TaskProjection::HasActiveRun)) continue;
                    item = { WorkKind::Refresh, std::nullopt, std::nullopt };
                }
            }
            if (item.Kind == WorkKind::Refresh)
            {
                _refresh(item.QueryTaskId);
                if (!item.QueryTaskId)
                {
                    std::optional<std::string> selected;
                    {
                        const std::scoped_lock lock{ _mutex };
                        if (!_selectedTaskId.empty()) selected = _selectedTaskId;
                    }
                    if (selected) _refresh(selected);
                }
            }
            else if (item.Command) _submit(*item.Command);
        }
    }

    void RuntimeTaskProjectionSource::_submit(const TaskUiCommandRequest& command) noexcept
    {
        const auto payload = EncodeTaskUiCommandRequest(command);
        if (!payload.Succeeded) return;
        auto request = _client->Submit(ControlMethod::SubmitCommand, payload.Payload);
        if (!request.Accepted || !request.Completion.valid()) return;
        static_cast<void>(request.Completion.get());
        _refresh();
        std::optional<std::string> selected;
        {
            const std::scoped_lock lock{ _mutex };
            if (!_selectedTaskId.empty()) selected = _selectedTaskId;
        }
        if (selected) _refresh(selected);
    }

    void RuntimeTaskProjectionSource::_refresh(const std::optional<std::string>& taskId) noexcept
    {
        const auto finishLoading = wil::scope_exit([&]() noexcept {
            if (_loading.exchange(false, std::memory_order_acq_rel)) _notify();
        });
        TaskProjectionQueryRequest query;
        query.TaskId = taskId;
        query.IncludeCompleted = true;
        query.EventLimit = taskId ? 200 : 0;
        const auto payload = EncodeTaskProjectionQueryRequest(query);
        if (!payload.Succeeded) return;
        auto request = _client->Submit(ControlMethod::QueryTask, payload.Payload);
        if (!request.Accepted || !request.Completion.valid()) return;
        const auto completed = request.Completion.get();
        if (completed.Outcome != RuntimeClientRequestOutcome::Succeeded || !completed.Response || !completed.Response->Succeeded) return;
        const auto decoded = DecodeTaskProjectionQueryResult(completed.Response->ResultJson);
        if (!decoded.Succeeded()) return;
        auto mapped = MapTaskProjectionSnapshot(decoded.Value);
        _loading.store(false, std::memory_order_release);
        if (mapped)
        {
            if (taskId && mapped->size() == 1)
            {
                static_cast<void>(_coordinator.Merge(_client->RuntimeInstanceId(), decoded.Value.Generation, std::move(mapped->front())));
            }
            else
            {
                static_cast<void>(_coordinator.Apply(_client->RuntimeInstanceId(), decoded.Value.Generation, std::move(*mapped)));
            }
        }
        _notify();
    }

    void RuntimeTaskProjectionSource::_notify() noexcept
    {
        ChangedHandler handler;
        {
            const std::scoped_lock lock{ _mutex };
            if (_stopping) return;
            handler = _changed;
        }
        if (!handler) return;
        try
        {
            if (_dispatcher && !_dispatcher.HasThreadAccess())
            {
                static_cast<void>(_dispatcher.RunAsync(
                    Windows::UI::Core::CoreDispatcherPriority::Normal,
                    [handler = std::move(handler)] { handler(); }));
            }
            else
            {
                handler();
            }
        }
        catch (...)
        {
        }
    }

    std::shared_ptr<ITaskProjectionSource> MakeRuntimeTaskProjectionSource()
    {
        const auto configuration = _configuration();
        if (!configuration) return {};
        Windows::UI::Core::CoreDispatcher dispatcher{ nullptr };
        try
        {
            if (const auto window = Windows::UI::Xaml::Window::Current()) dispatcher = window.Dispatcher();
        }
        catch (...)
        {
        }
        auto source = std::make_shared<RuntimeTaskProjectionSource>(*configuration, std::move(dispatcher));
        return source->Start() ? source : std::shared_ptr<ITaskProjectionSource>{};
    }
}
