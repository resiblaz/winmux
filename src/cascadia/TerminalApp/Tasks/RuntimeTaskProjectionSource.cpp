// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskProjectionSource.h"

#include "TaskProjectionMapper.h"
#include "../Agents/RuntimeAgentCatalogSource.h"

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

        std::optional<TaskRuntimeClientConfiguration> _configuration()
        {
            try
            {
                const auto settingsRoot = std::filesystem::path{
                    std::wstring_view{ ::winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings::SettingsPath() }
                }.parent_path();
                auto identity = BuildCurrentUserRuntimeIdentity(L"dev", settingsRoot);
                const auto clientId = _uuid();
                if (!identity.Succeeded() || !clientId)
                    return std::nullopt;
                const auto module = std::filesystem::path{ wil::GetModuleFileNameW<std::wstring>(nullptr) };
                const auto runtime = module.parent_path() / L"TaskRuntime.exe";
                if (!std::filesystem::is_regular_file(runtime))
                    return std::nullopt;
                TaskRuntimeClientConfiguration configuration;
                configuration.Identity = std::move(identity.Identity);
                configuration.Hello.ClientInstanceId = *clientId;
                configuration.Hello.ClientKind = ProtocolClientKind::TerminalUi;
                configuration.Hello.ExpectedRuntimeIdentity = configuration.Identity.ProtocolIdentity;
                configuration.Hello.RequiredCapabilities =
                    static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
                    static_cast<uint64_t>(ProtocolCapability::TaskCommands);
                configuration.Hello.OptionalCapabilities =
                    static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle) |
                    static_cast<uint64_t>(ProtocolCapability::TerminalStream) |
                    static_cast<uint64_t>(ProtocolCapability::InputControllerLease) |
                    static_cast<uint64_t>(ProtocolCapability::TaskTerminalSurfaces) |
                    static_cast<uint64_t>(ProtocolCapability::TaskOrchestration) |
                    static_cast<uint64_t>(ProtocolCapability::RuntimeDrain) |
                    static_cast<uint64_t>(ProtocolCapability::AgentProfiles);
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
        Windows::UI::Core::CoreDispatcher dispatcher,
        std::shared_ptr<TaskAgentSessionRegistry> agentSessions,
        TaskRuntimeClientDependencies dependencies) :
        _identity{ configuration.Identity },
        _clientInstanceId{ configuration.Hello.ClientInstanceId },
        _client{ std::make_shared<TaskRuntimeClient>(std::move(configuration), std::move(dependencies)) },
        _dispatcher{ std::move(dispatcher) },
        _agentSessions{ std::move(agentSessions) }
    {
        _agentCatalog = agents::MakeRuntimeAgentCatalogSource(_client, _dispatcher);
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

    void RuntimeTaskProjectionSource::SetSurfaceHandler(SurfaceHandler handler)
    {
        const std::scoped_lock lock{ _mutex };
        _surfaceChanged = std::move(handler);
    }

    bool RuntimeTaskProjectionSource::Start()
    {
        const std::scoped_lock lock{ _mutex };
        if (_started)
            return true;
        if (_stopping || !_client || !_client->Valid() || !_client->Start())
            return false;
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
            if (_stopping)
                return;
            _stopping = true;
            _changed = {};
            _surfaceChanged = {};
            _work.clear();
        }
        _condition.notify_all();
        if (_client)
            _client->RequestStop();
        if (_worker.joinable())
            _worker.join();
        if (_client)
            static_cast<void>(_client->WaitForStopped());
    }

    bool RuntimeTaskProjectionSource::_enqueue(WorkItem item)
    {
        const std::scoped_lock lock{ _mutex };
        if (!_started || _stopping || _work.size() >= 256)
            return false;
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
            if (!_started || _stopping || selected == _selectedTaskId)
                return;
            _selectedTaskId = selected;
            if (_work.size() >= 256)
                return;
            _work.push_back({ WorkKind::Refresh, std::nullopt, selected.empty() ? std::nullopt : std::optional<std::string>{ selected } });
        }
        _condition.notify_one();
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
                if (_stopping)
                    return;
                if (signaled)
                {
                    item = std::move(_work.front());
                    _work.pop_front();
                }
                else
                {
                    // Intents and queued messages may complete after an idle
                    // snapshot; other windows can also create or change Tasks.
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
                        if (!_selectedTaskId.empty())
                            selected = _selectedTaskId;
                    }
                    if (selected)
                        _refresh(selected);
                }
            }
            else if (item.Command)
                _submit(*item.Command);
        }
    }

    void RuntimeTaskProjectionSource::_submit(const TaskUiCommandRequest& command) noexcept
    {
        const auto payload = EncodeTaskUiCommandRequest(command);
        if (!payload.Succeeded)
            return;
        auto request = _client->Submit(ControlMethod::SubmitCommand, payload.Payload);
        if (!request.Accepted || !request.Completion.valid())
            return;
        const auto completed = request.Completion.get();
        if (completed.Outcome == RuntimeClientRequestOutcome::Succeeded &&
            completed.Response && completed.Response->Succeeded)
        {
            const auto decoded = DecodeTaskUiCommandResult(completed.Response->ResultJson);
            if (decoded.Succeeded())
            {
                _notifySurface(command, decoded.Value);
            }
        }
        _refresh();
        std::optional<std::string> selected;
        {
            const std::scoped_lock lock{ _mutex };
            if (!_selectedTaskId.empty())
                selected = _selectedTaskId;
        }
        if (selected)
            _refresh(selected);
    }

    void RuntimeTaskProjectionSource::_refresh(const std::optional<std::string>& taskId) noexcept
    {
        uint64_t publicationRevision;
        {
            const std::scoped_lock publication{ _publicationMutex };
            publicationRevision = _publicationRevision;
        }
        const auto finishLoading = wil::scope_exit([&]() noexcept {
            if (_loading.exchange(false, std::memory_order_acq_rel))
                _notify();
        });
        if (!_drainSubmitted && _client->State() == TaskRuntimeClientState::Ready &&
            _client->ProtocolMinor() < CurrentProtocolMinor &&
            HasCapability(_client->NegotiatedCapabilities(), ProtocolCapability::RuntimeDrain))
        {
            _drainSubmitted = true;
            auto drain = _client->Submit(ControlMethod::DrainRuntime, "{}", 1);
            if (drain.Accepted && drain.Completion.valid())
            {
                static_cast<void>(drain.Completion.get());
            }
            return;
        }
        TaskProjectionQueryRequest query;
        query.TaskId = taskId;
        query.IncludeCompleted = true;
        query.EventLimit = taskId ? 200 : 0;
        const auto payload = EncodeTaskProjectionQueryRequest(query);
        if (!payload.Succeeded)
            return;
        auto request = _client->Submit(ControlMethod::QueryTask, payload.Payload);
        if (!request.Accepted || !request.Completion.valid())
            return;
        const auto completed = request.Completion.get();
        if (completed.Outcome != RuntimeClientRequestOutcome::Succeeded || !completed.Response || !completed.Response->Succeeded)
            return;
        const auto decoded = DecodeTaskProjectionQueryResult(completed.Response->ResultJson);
        if (!decoded.Succeeded())
            return;
        std::unique_lock publication{ _publicationMutex };
        // Async creation may finish while this query is in flight. Never
        // publish that older snapshot over the acknowledged new Task.
        if (publicationRevision != _publicationRevision) return;
        if (_agentSessions)
        {
            if (taskId && decoded.Value.Tasks.size() == 1)
                _agentSessions->ReplaceTask(decoded.Value.Tasks.front());
            else if (!taskId)
                _agentSessions->ReplaceAll(decoded.Value.Tasks);
        }
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
        publication.unlock();
        _notify();
    }

    std::shared_ptr<ITaskProjectionSource> MakeRuntimeTaskProjectionSource(
        std::shared_ptr<TaskAgentSessionRegistry> agentSessions)
    {
        const auto configuration = _configuration();
        if (!configuration)
            return {};
        Windows::UI::Core::CoreDispatcher dispatcher{ nullptr };
        try
        {
            if (const auto window = Windows::UI::Xaml::Window::Current())
                dispatcher = window.Dispatcher();
        }
        catch (...)
        {
        }
        auto source = std::make_shared<RuntimeTaskProjectionSource>(
            *configuration, std::move(dispatcher), std::move(agentSessions));
        return source->Start() ? source : std::shared_ptr<ITaskProjectionSource>{};
    }
}
