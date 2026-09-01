// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskProviderCoordinator.h"

namespace Microsoft::Terminal::TaskProvider
{
    namespace
    {
        bool _validIdentity(const std::string_view value) noexcept
        {
            return !value.empty() && value.size() <= MaximumProviderIdentifierBytes &&
                   value.find('\0') == std::string_view::npos;
        }

        bool _terminal(const TaskProviderRunState state) noexcept
        {
            return state == TaskProviderRunState::Completed ||
                   state == TaskProviderRunState::Failed ||
                   state == TaskProviderRunState::Interrupted;
        }

        TaskProviderCoordinatorResult _from(const TaskProviderResult& source)
        {
            TaskProviderCoordinatorResult result;
            result.Status = source.Status;
            result.Error = source.Error;
            return result;
        }
    }

    bool TaskProviderCoordinatorConfiguration::Valid() const noexcept
    {
        return MaximumSessions > 0 && MaximumSessions <= 4096 &&
               MaximumRuns >= MaximumSessions && MaximumRuns <= 16'384 &&
               EventPollMilliseconds > 0 && EventPollMilliseconds <= 5'000;
    }

    TaskProviderCoordinator::TaskProviderCoordinator(
        TaskProviderAdapterFactory adapterFactory,
        std::shared_ptr<ITaskProviderCoordinatorSink> sink,
        TaskProviderCoordinatorConfiguration configuration) :
        _factory{ std::move(adapterFactory) },
        _sink{ std::move(sink) },
        _configuration{ std::move(configuration) }
    {
        _valid = static_cast<bool>(_factory) && static_cast<bool>(_sink) && _configuration.Valid();
    }

    TaskProviderCoordinator::~TaskProviderCoordinator()
    {
        Stop();
    }

    bool TaskProviderCoordinator::Valid() const noexcept
    {
        return _valid;
    }

    bool TaskProviderCoordinator::ProviderRunning() const noexcept
    {
        const std::scoped_lock lock{ _mutex };
        return _adapter != nullptr;
    }

    TaskProviderResult TaskProviderCoordinator::_ensureProvider(const HANDLE cancellationEvent)
    {
        if (!_valid) return { TaskProviderStatus::InvalidRequest, "Task provider coordinator is invalid" };
        if (_adapter) return { TaskProviderStatus::AlreadyApplied, {} };
        try
        {
            auto adapter = _factory();
            if (!adapter) return { TaskProviderStatus::ResourceFailure, "Task provider factory returned no adapter" };
            auto capabilities = adapter->ProbeCapabilities(cancellationEvent);
            if (!capabilities.Succeeded()) return { capabilities.Status, capabilities.Error };
            if (!capabilities.Value ||
                (capabilities.Value->Capabilities & RequiredTaskProviderCapabilities) != RequiredTaskProviderCapabilities)
            {
                adapter->Stop();
                return { TaskProviderStatus::UnsupportedCapability, "Task provider is missing a required capability" };
            }
            _adapter = std::move(adapter);
            return { TaskProviderStatus::Succeeded, {} };
        }
        catch (...)
        {
            return { TaskProviderStatus::ResourceFailure, "Task provider initialization exhausted resources" };
        }
    }

    TaskProviderResult TaskProviderCoordinator::_publishSession(const TaskProviderSessionBinding& session) const
    {
        try
        {
            return _sink->OnSessionStateChanged(session);
        }
        catch (...)
        {
            return { TaskProviderStatus::ResourceFailure, "Task provider session state could not be persisted" };
        }
    }

    TaskProviderResult TaskProviderCoordinator::_publishRun(
        const TaskProviderRunBinding& run,
        const std::optional<TaskProviderFailureDisposition>& failure) const
    {
        try
        {
            return _sink->OnRunStateChanged(run, failure);
        }
        catch (...)
        {
            return { TaskProviderStatus::ResourceFailure, "Task provider Run state could not be persisted" };
        }
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::StartSession(
        const TaskProviderSessionRequest& request,
        const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        if (!_validIdentity(request.TaskId)) return _from({ TaskProviderStatus::InvalidRequest, "Task identity is invalid" });
        if (const auto existing = _sessionsByTask.find(request.TaskId); existing != _sessionsByTask.end())
        {
            TaskProviderCoordinatorResult result;
            result.Status = TaskProviderStatus::AlreadyApplied;
            result.Session = existing->second;
            return result;
        }
        if (_sessionsByTask.size() >= _configuration.MaximumSessions)
        {
            for (auto iterator = _sessionsByTask.begin(); iterator != _sessionsByTask.end();)
            {
                if (iterator->second.State == TaskProviderSessionState::Closed)
                {
                    _tasksByProviderSession.erase(iterator->second.ProviderSessionId);
                    iterator = _sessionsByTask.erase(iterator);
                }
                else
                {
                    ++iterator;
                }
            }
            if (_sessionsByTask.size() >= _configuration.MaximumSessions)
            {
                return _from({ TaskProviderStatus::ResourceFailure, "Task provider session capacity was reached" });
            }
        }
        const auto ready = _ensureProvider(cancellationEvent);
        if (!ready.Succeeded()) return _from(ready);
        auto started = _adapter->StartSession(request, cancellationEvent);
        if (!started.Succeeded() || !started.Value) return _from(started);
        if (!_validIdentity(started.Value->SessionId) || _tasksByProviderSession.contains(started.Value->SessionId))
        {
            return _from({ TaskProviderStatus::MalformedResponse, "Task provider returned a duplicate session identity" });
        }
        TaskProviderSessionBinding binding{ request.TaskId, started.Value->SessionId, started.Value->State };
        _tasksByProviderSession.emplace(binding.ProviderSessionId, binding.TaskId);
        _sessionsByTask.emplace(binding.TaskId, binding);
        const auto persisted = _publishSession(binding);
        if (!persisted.Succeeded())
        {
            _sessionsByTask.erase(binding.TaskId);
            _tasksByProviderSession.erase(binding.ProviderSessionId);
            static_cast<void>(_adapter->CloseSessionProjection(binding.ProviderSessionId, cancellationEvent));
            return _from(persisted);
        }
        TaskProviderCoordinatorResult result;
        result.Status = TaskProviderStatus::Succeeded;
        result.Session = std::move(binding);
        return result;
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::ResumeSession(
        const std::string_view taskId,
        const std::string_view providerSessionId,
        const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        if (!_validIdentity(taskId) || !_validIdentity(providerSessionId))
        {
            return _from({ TaskProviderStatus::InvalidRequest, "Task provider resume identity is invalid" });
        }
        const auto ready = _ensureProvider(cancellationEvent);
        if (!ready.Succeeded()) return _from(ready);
        auto resumed = _adapter->ResumeSession(providerSessionId, cancellationEvent);
        if (!resumed.Succeeded() || !resumed.Value) return _from(resumed);
        if (resumed.Value->SessionId != providerSessionId)
        {
            return _from({ TaskProviderStatus::MalformedResponse, "Task provider resumed a different session" });
        }
        if (const auto owner = _tasksByProviderSession.find(std::string{ providerSessionId });
            owner != _tasksByProviderSession.end() && owner->second != taskId)
        {
            return _from({ TaskProviderStatus::Conflict, "Task provider session belongs to another Task" });
        }
        TaskProviderSessionBinding binding{ std::string{ taskId }, std::string{ providerSessionId }, TaskProviderSessionState::Ready };
        if (const auto old = _sessionsByTask.find(binding.TaskId); old != _sessionsByTask.end())
        {
            _tasksByProviderSession.erase(old->second.ProviderSessionId);
        }
        _sessionsByTask.insert_or_assign(binding.TaskId, binding);
        _tasksByProviderSession.insert_or_assign(binding.ProviderSessionId, binding.TaskId);
        const auto persisted = _publishSession(binding);
        if (!persisted.Succeeded()) return _from(persisted);
        TaskProviderCoordinatorResult result;
        result.Status = TaskProviderStatus::Succeeded;
        result.Session = std::move(binding);
        return result;
    }

    TaskProviderRunBinding* TaskProviderCoordinator::_findRun(
        const std::string_view taskId,
        const std::string_view taskRunId) noexcept
    {
        const auto found = _runsByTaskRun.find(std::string{ taskRunId });
        return found != _runsByTaskRun.end() && found->second.TaskId == taskId ? &found->second : nullptr;
    }

    TaskProviderRunBinding* TaskProviderCoordinator::_activeRun(const std::string_view taskId) noexcept
    {
        for (auto& [id, run] : _runsByTaskRun)
        {
            if (run.TaskId == taskId && !_terminal(run.State)) return &run;
        }
        return nullptr;
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::StartRun(
        const std::string_view taskId,
        const TaskProviderRunRequest& request,
        const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        if (!_validIdentity(taskId) || !_validIdentity(request.Context.RunId))
        {
            return _from({ TaskProviderStatus::InvalidRequest, "Task provider Run identity is invalid" });
        }
        const auto session = _sessionsByTask.find(std::string{ taskId });
        if (session == _sessionsByTask.end() || session->second.ProviderSessionId != request.SessionId)
        {
            return _from({ TaskProviderStatus::NotFound, "Task provider session was not found for the Task" });
        }
        if (session->second.State == TaskProviderSessionState::ClosePending ||
            session->second.State == TaskProviderSessionState::Closing ||
            session->second.State == TaskProviderSessionState::Closed ||
            session->second.State == TaskProviderSessionState::Poisoned)
        {
            return _from({ TaskProviderStatus::Conflict, "Task provider session cannot start a Run in its current state" });
        }
        if (_activeRun(taskId)) return _from({ TaskProviderStatus::Conflict, "Task already has an active provider Run" });
        if (_runsByTaskRun.contains(request.Context.RunId)) return _from({ TaskProviderStatus::Conflict, "TaskRun identity is already registered" });
        if (_runsByTaskRun.size() >= _configuration.MaximumRuns)
        {
            for (auto iterator = _runsByTaskRun.begin(); iterator != _runsByTaskRun.end();)
            {
                if (_terminal(iterator->second.State))
                {
                    _taskRunsByProviderRun.erase(iterator->second.ProviderRunId);
                    iterator = _runsByTaskRun.erase(iterator);
                }
                else
                {
                    ++iterator;
                }
            }
            if (_runsByTaskRun.size() >= _configuration.MaximumRuns)
            {
                return _from({ TaskProviderStatus::ResourceFailure, "Task provider Run capacity was reached" });
            }
        }
        const auto ready = _ensureProvider(cancellationEvent);
        if (!ready.Succeeded()) return _from(ready);
        auto started = _adapter->StartRun(request, cancellationEvent);
        if (!started.Succeeded() || !started.Value) return _from(started);
        if (!_validIdentity(started.Value->RunId) || _taskRunsByProviderRun.contains(started.Value->RunId))
        {
            return _from({ TaskProviderStatus::MalformedResponse, "Task provider returned a duplicate Run identity" });
        }
        TaskProviderRunBinding binding{
            std::string{ taskId }, request.Context.RunId, request.SessionId,
            started.Value->RunId, started.Value->State,
        };
        _taskRunsByProviderRun.emplace(binding.ProviderRunId, binding.TaskRunId);
        _runsByTaskRun.emplace(binding.TaskRunId, binding);
        session->second.State = TaskProviderSessionState::Busy;
        const auto persistedSession = _publishSession(session->second);
        const auto persistedRun = _publishRun(binding);
        if (!persistedSession.Succeeded() || !persistedRun.Succeeded())
        {
            static_cast<void>(_adapter->InterruptRun(
                binding.ProviderSessionId,
                binding.ProviderRunId,
                cancellationEvent));
            _runsByTaskRun.erase(binding.TaskRunId);
            _taskRunsByProviderRun.erase(binding.ProviderRunId);
            session->second.State = TaskProviderSessionState::Ready;
            return _from(!persistedRun.Succeeded() ? persistedRun : persistedSession);
        }
        TaskProviderCoordinatorResult result;
        result.Status = TaskProviderStatus::Succeeded;
        result.Run = std::move(binding);
        return result;
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::SteerRun(
        const std::string_view taskId,
        const std::string_view taskRunId,
        const std::string_view message,
        const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        auto run = _findRun(taskId, taskRunId);
        if (!run || _terminal(run->State)) return _from({ TaskProviderStatus::NotFound, "Active Task provider Run was not found" });
        if (!_adapter) return _from({ TaskProviderStatus::Unavailable, "Task provider is unavailable" });
        const auto steered = _adapter->SteerRun(run->ProviderSessionId, run->ProviderRunId, message, cancellationEvent);
        if (steered.Status == TaskProviderStatus::Rejected || steered.Status == TaskProviderStatus::Conflict)
        {
            try
            {
                const auto persisted = _sink->OnSteeringDeferred(*run, message, "provider_steering_rejected");
                if (!persisted.Succeeded()) return _from(persisted);
            }
            catch (...)
            {
                return _from({ TaskProviderStatus::ResourceFailure, "Deferred steering could not be persisted" });
            }
            TaskProviderCoordinatorResult result;
            result.Status = TaskProviderStatus::Succeeded;
            result.Deferred = true;
            result.Run = *run;
            return result;
        }
        auto result = _from(steered);
        result.Run = *run;
        return result;
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::InterruptRun(
        const std::string_view taskId,
        const std::string_view taskRunId,
        const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        auto run = _findRun(taskId, taskRunId);
        if (!run || _terminal(run->State)) return _from({ TaskProviderStatus::NotFound, "Active Task provider Run was not found" });
        if (!_adapter) return _from({ TaskProviderStatus::Unavailable, "Task provider is unavailable" });
        auto result = _from(_adapter->InterruptRun(run->ProviderSessionId, run->ProviderRunId, cancellationEvent));
        result.Run = *run;
        return result;
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::CloseTask(
        const std::string_view taskId,
        const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        const auto session = _sessionsByTask.find(std::string{ taskId });
        if (session == _sessionsByTask.end()) return _from({ TaskProviderStatus::NotFound, "Task provider session was not found" });
        if (session->second.State == TaskProviderSessionState::Closed)
        {
            TaskProviderCoordinatorResult result;
            result.Status = TaskProviderStatus::AlreadyApplied;
            result.Session = session->second;
            return result;
        }
        if (!_adapter) return _from({ TaskProviderStatus::Unavailable, "Task provider is unavailable" });
        if (auto run = _activeRun(taskId))
        {
            const auto interrupted = _adapter->InterruptRun(run->ProviderSessionId, run->ProviderRunId, cancellationEvent);
            if (interrupted.Status == TaskProviderStatus::Timeout || interrupted.Status == TaskProviderStatus::TransportClosed)
            {
                session->second.State = TaskProviderSessionState::ClosePending;
                const auto persisted = _publishSession(session->second);
                if (!persisted.Succeeded()) return _from(persisted);
                TaskProviderCoordinatorResult result;
                result.Status = TaskProviderStatus::Succeeded;
                result.ClosePending = true;
                result.Session = session->second;
                result.Run = *run;
                return result;
            }
            if (!interrupted.Succeeded()) return _from(interrupted);
            session->second.State = TaskProviderSessionState::Closing;
            const auto persisted = _publishSession(session->second);
            if (!persisted.Succeeded()) return _from(persisted);
            TaskProviderCoordinatorResult result;
            result.Status = TaskProviderStatus::Succeeded;
            result.ClosePending = false;
            result.Session = session->second;
            result.Run = *run;
            return result;
        }
        const auto closed = _adapter->CloseSessionProjection(session->second.ProviderSessionId, cancellationEvent);
        if (!closed.Succeeded()) return _from(closed);
        session->second.State = TaskProviderSessionState::Closed;
        const auto persisted = _publishSession(session->second);
        if (!persisted.Succeeded()) return _from(persisted);
        TaskProviderCoordinatorResult result;
        result.Status = TaskProviderStatus::Succeeded;
        result.Session = session->second;
        return result;
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::SuspendTask(
        const std::string_view taskId,
        const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        const auto session = _sessionsByTask.find(std::string{ taskId });
        if (session == _sessionsByTask.end()) return _from({ TaskProviderStatus::NotFound, "Task provider session was not found" });
        if (_activeRun(taskId)) return _from({ TaskProviderStatus::Conflict, "Active Task provider Run prevents suspension" });
        if (!_adapter) return _from({ TaskProviderStatus::Unavailable, "Task provider is unavailable" });
        const auto suspended = _adapter->SuspendSession(session->second.ProviderSessionId, cancellationEvent);
        if (!suspended.Succeeded()) return _from(suspended);
        session->second.State = TaskProviderSessionState::Suspended;
        const auto persisted = _publishSession(session->second);
        if (!persisted.Succeeded()) return _from(persisted);
        TaskProviderCoordinatorResult result;
        result.Status = TaskProviderStatus::Succeeded;
        result.Session = session->second;
        return result;
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::RespondToApproval(
        const std::string_view taskId,
        const std::string_view taskRunId,
        const std::string_view approvalId,
        const TaskProviderApprovalDecision decision,
        const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        auto run = _findRun(taskId, taskRunId);
        if (!run || _terminal(run->State)) return _from({ TaskProviderStatus::NotFound, "Active Task provider Run was not found" });
        if (!_adapter) return _from({ TaskProviderStatus::Unavailable, "Task provider is unavailable" });
        auto result = _from(_adapter->RespondToApproval(
            run->ProviderSessionId, run->ProviderRunId, approvalId, decision, cancellationEvent));
        result.Run = *run;
        return result;
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::_reconcileProviderFailure(
        const TaskProviderStatus status,
        const std::string_view providerCode,
        std::string error)
    {
        TaskProviderCoordinatorResult result;
        result.Status = status;
        result.Error = std::move(error);
        auto disposition = _adapter ? _adapter->ClassifyFailureAndResumeSafety(status, providerCode) : TaskProviderFailureDisposition{};
        std::unordered_set<std::string> affected;
        for (auto& [id, run] : _runsByTaskRun)
        {
            if (!_terminal(run.State))
            {
                run.State = TaskProviderRunState::Interrupted;
                affected.emplace(run.TaskId);
                static_cast<void>(_publishRun(run, disposition));
            }
        }
        for (auto& [taskId, session] : _sessionsByTask)
        {
            if (session.State == TaskProviderSessionState::ClosePending ||
                session.State == TaskProviderSessionState::Closing)
            {
                session.State = TaskProviderSessionState::Closed;
                affected.emplace(taskId);
                static_cast<void>(_publishSession(session));
            }
            else if (session.State != TaskProviderSessionState::Closed)
            {
                session.State = TaskProviderSessionState::Suspended;
                if (affected.contains(taskId)) static_cast<void>(_publishSession(session));
            }
        }
        result.AffectedTaskIds.assign(affected.begin(), affected.end());
        std::ranges::sort(result.AffectedTaskIds);
        if (_adapter) _adapter->Stop();
        _adapter.reset();
        return result;
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::PumpOneEvent(const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        if (!_adapter) return _from({ TaskProviderStatus::Unavailable, "Task provider is not running" });
        auto next = _adapter->NextEvent(_configuration.EventPollMilliseconds, cancellationEvent);
        if (!next.Succeeded())
        {
            if (next.Status == TaskProviderStatus::TransportClosed ||
                next.Status == TaskProviderStatus::OutputLimitExceeded ||
                next.Status == TaskProviderStatus::MalformedResponse ||
                next.Status == TaskProviderStatus::ResourceFailure)
            {
                return _reconcileProviderFailure(next.Status, "runtime_recovery", std::move(next.Error));
            }
            return _from(next);
        }
        TaskProviderCoordinatorResult result;
        result.Status = TaskProviderStatus::Succeeded;
        if (next.NoEvent || !next.Value) return result;
        const auto task = _tasksByProviderSession.find(next.Value->SessionId);
        if (task == _tasksByProviderSession.end())
        {
            if (next.Value->Kind == TaskProviderEventKind::UnknownOptional) return result;
            return _from({ TaskProviderStatus::NotFound, "Task provider event session is not registered" });
        }
        auto session = _sessionsByTask.find(task->second);
        if (session == _sessionsByTask.end()) return _from({ TaskProviderStatus::ResourceFailure, "Task provider session registry is inconsistent" });
        std::optional<TaskProviderRunBinding> run;
        if (!next.Value->RunId.empty())
        {
            const auto taskRunId = _taskRunsByProviderRun.find(next.Value->RunId);
            if (taskRunId != _taskRunsByProviderRun.end())
            {
                auto found = _runsByTaskRun.find(taskRunId->second);
                if (found != _runsByTaskRun.end()) run = found->second;
            }
        }
        if (next.Value->Kind == TaskProviderEventKind::RunCompleted ||
            next.Value->Kind == TaskProviderEventKind::RunFailed ||
            next.Value->Kind == TaskProviderEventKind::RunInterrupted)
        {
            if (!run) return _from({ TaskProviderStatus::MalformedResponse, "Task provider terminal event has no registered Run" });
            auto& registered = _runsByTaskRun[run->TaskRunId];
            registered.State = next.Value->Kind == TaskProviderEventKind::RunCompleted ? TaskProviderRunState::Completed :
                               next.Value->Kind == TaskProviderEventKind::RunInterrupted ? TaskProviderRunState::Interrupted :
                               TaskProviderRunState::Failed;
            run = registered;
            const auto persistedRun = _publishRun(registered);
            if (!persistedRun.Succeeded()) return _from(persistedRun);
            if (session->second.State == TaskProviderSessionState::ClosePending ||
                session->second.State == TaskProviderSessionState::Closing)
            {
                const auto closed = _adapter->CloseSessionProjection(session->second.ProviderSessionId, cancellationEvent);
                if (closed.Succeeded()) session->second.State = TaskProviderSessionState::Closed;
            }
            else
            {
                session->second.State = TaskProviderSessionState::Ready;
            }
            const auto persistedSession = _publishSession(session->second);
            if (!persistedSession.Succeeded()) return _from(persistedSession);
        }
        try
        {
            const auto persisted = _sink->OnEvent(session->second, run, next.Value.value());
            if (!persisted.Succeeded()) return _from(persisted);
        }
        catch (...)
        {
            return _from({ TaskProviderStatus::ResourceFailure, "Task provider event could not be persisted" });
        }
        result.Session = session->second;
        result.Run = std::move(run);
        return result;
    }

    TaskProviderCoordinatorResult TaskProviderCoordinator::RestartProvider(const HANDLE cancellationEvent)
    {
        const std::scoped_lock lock{ _mutex };
        auto reconciled = _reconcileProviderFailure(
            TaskProviderStatus::TransportClosed, "runtime_recovery", "Task provider was explicitly restarted");
        const auto started = _ensureProvider(cancellationEvent);
        if (!started.Succeeded())
        {
            reconciled.Status = started.Status;
            reconciled.Error = started.Error;
            return reconciled;
        }
        reconciled.Status = TaskProviderStatus::Succeeded;
        reconciled.Error.clear();
        return reconciled;
    }

    std::vector<TaskProviderSessionBinding> TaskProviderCoordinator::Sessions() const
    {
        const std::scoped_lock lock{ _mutex };
        std::vector<TaskProviderSessionBinding> result;
        result.reserve(_sessionsByTask.size());
        for (const auto& [task, session] : _sessionsByTask) result.emplace_back(session);
        std::ranges::sort(result, {}, &TaskProviderSessionBinding::TaskId);
        return result;
    }

    std::vector<TaskProviderRunBinding> TaskProviderCoordinator::Runs() const
    {
        const std::scoped_lock lock{ _mutex };
        std::vector<TaskProviderRunBinding> result;
        result.reserve(_runsByTaskRun.size());
        for (const auto& [id, run] : _runsByTaskRun) result.emplace_back(run);
        std::ranges::sort(result, {}, &TaskProviderRunBinding::TaskRunId);
        return result;
    }

    void TaskProviderCoordinator::Stop() noexcept
    {
        const std::scoped_lock lock{ _mutex };
        if (_adapter) _adapter->Stop();
        _adapter.reset();
    }
}
