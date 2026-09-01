// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskProviderService.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskProvider;

    RuntimeTaskProviderService::RuntimeTaskProviderService(
        TaskProviderAdapterFactory adapterFactory,
        std::shared_ptr<ITaskProviderCoordinatorSink> sink,
        TaskProviderCoordinatorConfiguration configuration) :
        _coordinator{ std::make_shared<TaskProviderCoordinator>(
            std::move(adapterFactory), std::move(sink), std::move(configuration)) },
        _stopEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) }
    {
    }

    RuntimeTaskProviderService::~RuntimeTaskProviderService()
    {
        Stop();
    }

    bool RuntimeTaskProviderService::Valid() const noexcept
    {
        return _coordinator && _coordinator->Valid() && static_cast<bool>(_stopEvent);
    }

    bool RuntimeTaskProviderService::ProviderRunning() const noexcept
    {
        return _coordinator && _coordinator->ProviderRunning();
    }

    bool RuntimeTaskProviderService::_ensureEventPump() noexcept
    {
        const std::scoped_lock lock{ _mutex };
        if (_stopping || !_stopEvent) return false;
        if (_eventPump.joinable()) return true;
        try
        {
            _eventPump = std::thread{ [this] { _runEventPump(); } };
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void RuntimeTaskProviderService::_runEventPump() noexcept
    {
        while (_stopEvent && WaitForSingleObject(_stopEvent.get(), 0) == WAIT_TIMEOUT)
        {
            if (!_coordinator->ProviderRunning())
            {
                if (WaitForSingleObject(_stopEvent.get(), 100) == WAIT_OBJECT_0) break;
                continue;
            }
            const auto pumped = _coordinator->PumpOneEvent(_stopEvent.get());
            if (pumped.Status == TaskProviderStatus::Cancelled) break;
        }
    }

    TaskProviderCoordinatorResult RuntimeTaskProviderService::StartSession(
        const TaskProviderSessionRequest& request,
        const HANDLE cancellationEvent)
    {
        auto result = _coordinator->StartSession(request, cancellationEvent);
        if (result.Succeeded() && !_ensureEventPump())
        {
            result.Status = TaskProviderStatus::ResourceFailure;
            result.Error = "Runtime provider event pump could not start";
        }
        return result;
    }

    TaskProviderCoordinatorResult RuntimeTaskProviderService::ResumeSession(
        const std::string_view taskId,
        const std::string_view providerSessionId,
        const HANDLE cancellationEvent)
    {
        auto result = _coordinator->ResumeSession(taskId, providerSessionId, cancellationEvent);
        if (result.Succeeded() && !_ensureEventPump())
        {
            result.Status = TaskProviderStatus::ResourceFailure;
            result.Error = "Runtime provider event pump could not start";
        }
        return result;
    }

    TaskProviderCoordinatorResult RuntimeTaskProviderService::StartRun(
        const std::string_view taskId,
        const TaskProviderRunRequest& request,
        const HANDLE cancellationEvent)
    {
        return _coordinator->StartRun(taskId, request, cancellationEvent);
    }

    TaskProviderCoordinatorResult RuntimeTaskProviderService::SteerRun(
        const std::string_view taskId,
        const std::string_view taskRunId,
        const std::string_view message,
        const HANDLE cancellationEvent)
    {
        return _coordinator->SteerRun(taskId, taskRunId, message, cancellationEvent);
    }

    TaskProviderCoordinatorResult RuntimeTaskProviderService::InterruptRun(
        const std::string_view taskId,
        const std::string_view taskRunId,
        const HANDLE cancellationEvent)
    {
        return _coordinator->InterruptRun(taskId, taskRunId, cancellationEvent);
    }

    TaskProviderCoordinatorResult RuntimeTaskProviderService::CloseTask(
        const std::string_view taskId,
        const HANDLE cancellationEvent)
    {
        return _coordinator->CloseTask(taskId, cancellationEvent);
    }

    TaskProviderCoordinatorResult RuntimeTaskProviderService::SuspendTask(
        const std::string_view taskId,
        const HANDLE cancellationEvent)
    {
        return _coordinator->SuspendTask(taskId, cancellationEvent);
    }

    TaskProviderCoordinatorResult RuntimeTaskProviderService::RespondToApproval(
        const std::string_view taskId,
        const std::string_view taskRunId,
        const std::string_view approvalId,
        const TaskProviderApprovalDecision decision,
        const HANDLE cancellationEvent)
    {
        return _coordinator->RespondToApproval(
            taskId, taskRunId, approvalId, decision, cancellationEvent);
    }

    TaskProviderCoordinatorResult RuntimeTaskProviderService::RestartProvider(const HANDLE cancellationEvent)
    {
        auto result = _coordinator->RestartProvider(cancellationEvent);
        if (result.Succeeded()) static_cast<void>(_ensureEventPump());
        return result;
    }

    void RuntimeTaskProviderService::Stop() noexcept
    {
        {
            const std::scoped_lock lock{ _mutex };
            if (_stopping) return;
            _stopping = true;
            if (_stopEvent) SetEvent(_stopEvent.get());
        }
        if (_coordinator) _coordinator->Stop();
        if (_eventPump.joinable()) _eventPump.join();
    }
}
