// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskTerminalCallbackBridge.h"

#include "RuntimeTaskOrchestrator.h"
#include "RuntimeTaskProviderService.h"

#include "ITaskStore.h"
#include "TaskRunStatePolicy.h"

#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    RuntimeTaskTerminalCallbackBridge::RuntimeTaskTerminalCallbackBridge(
        std::shared_ptr<ITaskStore> store) :
        _store{ std::move(store) }
    {
    }

    void RuntimeTaskTerminalCallbackBridge::SetProvider(
        const std::shared_ptr<RuntimeTaskProviderService>& provider) noexcept
    {
        const std::scoped_lock lock{ _mutex };
        _provider = provider;
    }

    void RuntimeTaskTerminalCallbackBridge::SetOrchestrator(
        const std::shared_ptr<RuntimeTaskOrchestrator>& orchestrator) noexcept
    {
        const std::scoped_lock lock{ _mutex };
        _orchestrator = orchestrator;
    }

    RuntimeTerminalBackendResult RuntimeTaskTerminalCallbackBridge::Input(
        const std::string_view taskId,
        const std::span<const uint8_t> input)
    {
        if (input.empty() || input.size() > MaximumTaskMessageBytes || !IsValidUtf8(input))
        {
            return { RuntimeTerminalBackendStatus::Rejected, "Task terminal input is invalid" };
        }
        const auto task = ParseId<TaskId>(taskId);
        const auto aggregate = task && _store ? _store->LoadTask(*task) : std::nullopt;
        if (!aggregate)
        {
            return { RuntimeTerminalBackendStatus::Closed, "Task is unavailable" };
        }
        const auto run = std::ranges::find_if(aggregate->Runs, [](const auto& candidate) {
            return IsTaskRunActive(candidate.State);
        });
        if (run == aggregate->Runs.end() || run->State != TaskRunState::Running)
        {
            std::shared_ptr<RuntimeTaskOrchestrator> orchestrator;
            {
                const std::scoped_lock lock{ _mutex };
                orchestrator = _orchestrator.lock();
            }
            return orchestrator && orchestrator->SubmitTerminalMessage(taskId, input) ?
                       RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} } :
                       RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Rejected, "Task has no running Agent turn" };
        }
        std::shared_ptr<RuntimeTaskProviderService> provider;
        {
            const std::scoped_lock lock{ _mutex };
            provider = _provider.lock();
        }
        if (!provider)
        {
            return { RuntimeTerminalBackendStatus::SystemError, "Task provider is unavailable" };
        }
        const std::string message{
            reinterpret_cast<const char*>(input.data()), input.size()
        };
        const auto steered = provider->SteerRun(taskId, run->Id.Value(), message, nullptr);
        return steered.Succeeded() ?
                   RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} } :
                   RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Rejected, steered.Error };
    }

    RuntimeTerminalBackendResult RuntimeTaskTerminalCallbackBridge::Interrupt(
        const std::string_view taskId)
    {
        const auto task = ParseId<TaskId>(taskId);
        const auto aggregate = task && _store ? _store->LoadTask(*task) : std::nullopt;
        if (!aggregate)
        {
            return { RuntimeTerminalBackendStatus::Closed, "Task is unavailable" };
        }
        const auto run = std::ranges::find_if(aggregate->Runs, [](const auto& candidate) {
            return IsTaskRunActive(candidate.State);
        });
        if (run == aggregate->Runs.end() || run->State != TaskRunState::Running)
        {
            std::shared_ptr<RuntimeTaskOrchestrator> orchestrator;
            {
                const std::scoped_lock lock{ _mutex };
                orchestrator = _orchestrator.lock();
            }
            return orchestrator && orchestrator->RequestStop(taskId) ?
                       RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} } :
                       RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Rejected, "Task has no running Agent turn" };
        }
        std::shared_ptr<RuntimeTaskProviderService> provider;
        {
            const std::scoped_lock lock{ _mutex };
            provider = _provider.lock();
        }
        if (!provider)
        {
            return { RuntimeTerminalBackendStatus::SystemError, "Task provider is unavailable" };
        }
        const auto interrupted = provider->InterruptRun(taskId, run->Id.Value(), nullptr);
        return interrupted.Succeeded() ?
                   RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} } :
                   RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Rejected, interrupted.Error };
    }
}
