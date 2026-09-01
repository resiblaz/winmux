// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskSchedulingPolicy.h"

#include "TaskRunStatePolicy.h"
#include "TaskProviderTypes.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;

    namespace
    {
        std::pair<uint32_t, uint32_t> _retryPolicy(const std::string_view reason) noexcept
        {
            if (reason == "runtime_recovery")
                return { 2, 1000 };
            if (reason == "provider_network" || reason == "provider-network")
                return { 3, 1000 };
            if (reason == "inactivity_timeout" || reason == "inactivity-timeout")
                return { 2, 2000 };
            return {};
        }

        const TaskRunRecord* _findRun(
            const std::vector<TaskRunRecord>& runs,
            const TaskRunId& id) noexcept
        {
            const auto found = std::ranges::find(runs, id, &TaskRunRecord::Id);
            return found == runs.end() ? nullptr : &*found;
        }

        const TaskRunRecord* _rootRun(
            const TaskRunRecord& run,
            const std::vector<TaskRunRecord>& runs) noexcept
        {
            auto current = &run;
            for (size_t depth = 0; current->RetryOfRunId && depth < runs.size(); ++depth)
            {
                const auto parent = _findRun(runs, *current->RetryOfRunId);
                if (!parent)
                    break;
                current = parent;
            }
            return current;
        }
    }

    RuntimeTaskRetryDecision EvaluateRuntimeTaskAutomaticRetry(
        const TaskRunRecord& failedRun,
        const std::vector<TaskRunRecord>& runs) noexcept
    {
        RuntimeTaskRetryDecision result;
        if (!IsTaskRunTerminal(failedRun.State) ||
            (failedRun.State != TaskRunState::Failed && failedRun.State != TaskRunState::Interrupted))
        {
            return result;
        }
        const auto [maximum, baseBackoff] = _retryPolicy(failedRun.ReasonCode);
        if (maximum == 0)
        {
            return result;
        }
        const auto root = _rootRun(failedRun, runs);
        uint32_t executions{};
        for (const auto& candidate : runs)
        {
            if (_rootRun(candidate, runs)->Id == root->Id)
            {
                ++executions;
            }
        }
        result.ExecutionNumber = executions + 1;
        result.MaximumExecutions = maximum;
        result.Retry = executions < maximum;
        if (result.Retry)
        {
            const auto exponent = (std::min)(executions > 0 ? executions - 1 : 0, uint32_t{ 4 });
            result.BackoffMilliseconds = baseBackoff << exponent;
        }
        return result;
    }

    bool ShouldClaimRuntimeTaskMessages(const TaskAggregateSnapshot& aggregate) noexcept
    {
        return !aggregate.Task.ManualHold &&
               aggregate.Task.Status != TaskStatus::Done &&
               aggregate.Task.Status != TaskStatus::Cancelled &&
               !std::ranges::any_of(aggregate.Runs, [](const auto& run) { return IsTaskRunActive(run.State); }) &&
               (!aggregate.Intent || aggregate.Intent->Stage == TaskIntentStage::Completed) &&
               std::ranges::any_of(aggregate.Messages, [](const auto& message) {
                   return message.State == TaskMessageState::Pending || message.State == TaskMessageState::Deferred;
               });
    }

    std::optional<std::string> BuildRuntimeTaskPrompt(
        const TaskAggregateSnapshot& aggregate,
        const TaskRunRecord& run)
    {
        try
        {
            std::string prompt = aggregate.Task.Description.empty() ? aggregate.Task.Title : aggregate.Task.Description;
            if (!aggregate.Task.AcceptanceCriteria.empty())
            {
                prompt.append("\n\nAcceptance criteria:\n");
                for (const auto& criterion : aggregate.Task.AcceptanceCriteria)
                {
                    prompt.append("- ");
                    prompt.append(criterion.Text);
                    prompt.push_back('\n');
                }
            }
            std::vector<const TaskRunMessageRecord*> messages;
            for (const auto& message : aggregate.RunMessages)
            {
                if (message.Run == run.Id)
                {
                    messages.push_back(&message);
                }
            }
            std::ranges::sort(messages, {}, [](const auto* value) { return value->Position; });
            if (!messages.empty())
            {
                prompt.append("\nFollow-up messages, in order:\n");
                for (const auto* message : messages)
                {
                    prompt.append("- ");
                    prompt.append(message->CanonicalContent);
                    prompt.push_back('\n');
                }
            }
            if (prompt.empty() || prompt.size() > Microsoft::Terminal::TaskProvider::MaximumProviderTextBytes)
            {
                return std::nullopt;
            }
            return prompt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}
