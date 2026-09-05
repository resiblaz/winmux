// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskOrchestratorInternal.h"

#include "RuntimeGitWorktreeFinalize.h"
#include "RuntimeTaskCommandMetadata.h"
#include "RuntimeCrypto.h"

#include "TaskRunStatePolicy.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;

    namespace
    {
        const TaskRunRecord* _active(const TaskAggregateSnapshot& aggregate) noexcept
        {
            const auto found = std::ranges::find_if(aggregate.Runs, [](const auto& run) {
                return IsTaskRunActive(run.State);
            });
            return found == aggregate.Runs.end() ? nullptr : &*found;
        }

        const TaskRunRecord* _latestTerminal(const TaskAggregateSnapshot& aggregate) noexcept
        {
            const TaskRunRecord* latest{};
            for (const auto& run : aggregate.Runs)
            {
                if (IsTaskRunTerminal(run.State) && (!latest || run.Sequence > latest->Sequence))
                    latest = &run;
            }
            return latest;
        }

        bool _cancelRequested(const TaskIntentRecord& intent) noexcept
        {
            return intent.DetailJson.find("\"cancel\":true") != std::string::npos;
        }
    }

    void RuntimeTaskOrchestrator::Impl::HandleIntent(const TaskIntentRecord& observed) noexcept
    try
    {
        auto aggregate = Store->LoadTask(observed.Task);
        if (!aggregate || !aggregate->Intent || aggregate->Intent->Id != observed.Id ||
            aggregate->Intent->Stage == TaskIntentStage::Completed ||
            aggregate->Intent->Stage == TaskIntentStage::Failed)
        {
            return;
        }
        auto intent = *aggregate->Intent;
        if (intent.Kind == TaskIntentKind::Stop || intent.Kind == TaskIntentKind::Close || intent.Kind == TaskIntentKind::Delete)
        {
            if (const auto run = _active(*aggregate))
            {
                if (run->State == TaskRunState::Running)
                {
                    if (intent.Kind == TaskIntentKind::Stop)
                    {
                        if (intent.Stage == TaskIntentStage::WaitingForProvider && intent.ReasonCode == "provider-interrupt-pending")
                            return;
                        const auto interrupted = Provider->InterruptRun(
                            aggregate->Task.Id.Value(), run->Id.Value(), StopEvent.get());
                        static_cast<void>(AdvanceIntent(
                            aggregate->Task.Id, intent.Id, TaskIntentStage::WaitingForProvider,
                            interrupted.Succeeded() ? "provider-interrupt-pending" : "provider-interrupt-failed", intent.DetailJson));
                    }
                    else
                    {
                        const auto closed = Provider->CloseTask(aggregate->Task.Id.Value(), StopEvent.get());
                        static_cast<void>(AdvanceIntent(
                            aggregate->Task.Id, intent.Id, TaskIntentStage::WaitingForProvider,
                            closed.Succeeded() ? "provider-close-pending" : "provider-close-failed", intent.DetailJson));
                    }
                    // An interrupt reply only acknowledges the request. The provider
                    // terminal event owns the Run transition and releases the lease.
                    return;
                }
                if (WorkerBusy(run->Id))
                {
                    if (run->State != TaskRunState::Starting)
                        CancelPreparation(run->Id);
                    // A Starting worker may already be dispatching a provider turn.
                    // Let it publish its outcome before attempting interruption.
                    return;
                }
                static_cast<void>(TransitionRun(
                    aggregate->Task.Id, run->Id, TaskRunState::Cancelled, "user-stop"));
                return;
            }
            if (std::ranges::any_of(aggregate->Runs, [&](const auto& run) { return WorkerBusy(run.Id); }))
                return;
        }

        if (intent.Kind == TaskIntentKind::Stop)
        {
            static_cast<void>(AdvanceIntent(aggregate->Task.Id, intent.Id, TaskIntentStage::Completed));
            return;
        }

        if (intent.Kind == TaskIntentKind::Close || intent.Kind == TaskIntentKind::Delete)
        {
            const auto closed = Provider->CloseTask(aggregate->Task.Id.Value(), StopEvent.get());
            if (closed.ClosePending ||
                (!closed.Succeeded() && closed.Status != Microsoft::Terminal::TaskProvider::TaskProviderStatus::NotFound))
            {
                static_cast<void>(AdvanceIntent(
                    aggregate->Task.Id, intent.Id, TaskIntentStage::WaitingForProvider, "provider-close-pending", intent.DetailJson));
                return;
            }

            if (const auto terminalRun = _latestTerminal(*aggregate);
                terminalRun && terminalRun->StartedAtUnixMilliseconds && aggregate->GitWorktree &&
                aggregate->GitWorktree->State == TaskGitWorktreeState::Ready &&
                !std::ranges::any_of(aggregate->GitCheckpoints, [&](const auto& checkpoint) {
                    return checkpoint.Run == terminalRun->Id &&
                           (checkpoint.State == TaskGitCheckpointState::Succeeded ||
                            checkpoint.State == TaskGitCheckpointState::NotNeeded);
                }))
            {
                FinalizeRun(*aggregate, *terminalRun);
                return;
            }

            if (intent.Kind == TaskIntentKind::Close)
            {
                static_cast<void>(AdvanceIntent(
                    aggregate->Task.Id, intent.Id, TaskIntentStage::RemovingSurface, {}, intent.DetailJson));
                aggregate = Store->LoadTask(observed.Task);
                if (!aggregate || !aggregate->Intent)
                    return;
                static_cast<void>(Terminals->Close(
                    aggregate->Task.Id.Value(), aggregate->Terminal.Generation, true));
                if (aggregate->Terminal.State != TaskTerminalState::Closed)
                {
                    static_cast<void>(TransitionTerminal(
                        aggregate->Task.Id, TaskTerminalState::Closed, aggregate->Terminal.Generation));
                }
                if (_cancelRequested(*aggregate->Intent))
                {
                    static_cast<void>(ChangeStatus(aggregate->Task.Id, TaskStatus::Cancelled));
                }
                static_cast<void>(AdvanceIntent(
                    aggregate->Task.Id, aggregate->Intent->Id, TaskIntentStage::Completed, {}, aggregate->Intent->DetailJson));
                return;
            }
        }

        aggregate = Store->LoadTask(observed.Task);
        if (!aggregate || !aggregate->Intent)
            return;
        intent = *aggregate->Intent;
        if (intent.Kind == TaskIntentKind::WorktreeCleanup || intent.Kind == TaskIntentKind::Delete)
        {
            if (aggregate->GitWorktree && aggregate->GitWorktree->State != TaskGitWorktreeState::Finalized)
            {
                static_cast<void>(AdvanceIntent(
                    aggregate->Task.Id, intent.Id, TaskIntentStage::FinalizingWorktree, {}, intent.DetailJson));
                RuntimeGitWorktreeFinalizeService finalize{ Store, GitExecutable, RuntimeDataRoot };
                for (size_t step = 0; step < 3; ++step)
                {
                    const auto command = NewRuntimeUuid();
                    const auto event = NewRuntimeUuid();
                    if (!command || !event)
                        return;
                    const auto result = finalize.Step(
                        aggregate->Task.Id,
                        { CommandId{ *command }, TaskEventId{ *event }, RuntimeUnixMilliseconds(), true },
                        StopEvent.get());
                    if (result.Status == RuntimeGitWorktreeFinalizeStatus::Completed)
                        break;
                    if (!result.Succeeded())
                    {
                        static_cast<void>(AdvanceIntent(
                            aggregate->Task.Id, intent.Id, TaskIntentStage::Failed, result.ReasonCode.empty() ? "worktree-finalize-failed" : result.ReasonCode, intent.DetailJson));
                        return;
                    }
                }
                aggregate = Store->LoadTask(observed.Task);
                if (!aggregate || !aggregate->Intent ||
                    (aggregate->GitWorktree && aggregate->GitWorktree->State != TaskGitWorktreeState::Finalized))
                {
                    return;
                }
                intent = *aggregate->Intent;
            }
            if (intent.Kind == TaskIntentKind::WorktreeCleanup)
            {
                static_cast<void>(AdvanceIntent(
                    aggregate->Task.Id, intent.Id, TaskIntentStage::Completed, {}, intent.DetailJson));
                return;
            }
        }

        if (intent.Kind == TaskIntentKind::Delete)
        {
            static_cast<void>(Terminals->Close(
                aggregate->Task.Id.Value(), aggregate->Terminal.Generation, true));
            if (!AdvanceIntent(
                    aggregate->Task.Id, intent.Id, TaskIntentStage::DeletingAggregate, {}, intent.DetailJson))
            {
                return;
            }
            aggregate = Store->LoadTask(observed.Task);
            if (!aggregate || !aggregate->Intent)
                return;
            DeleteTaskCommand command;
            command.Task = aggregate->Task.Id;
            command.Intent = aggregate->Intent->Id;
            command.ExpectedIntentRevision = aggregate->Intent->Revision;
            const auto metadata = MakeRuntimeTaskCommandMetadata(
                "orchestrator-delete-v1", aggregate->Task.Id, aggregate->Intent->Id.Value(), aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
            if (!metadata)
                return;
            command.Metadata = *metadata;
            static_cast<void>(Store->DeleteTask(command));
        }
    }
    catch (...)
    {
    }

    bool RuntimeTaskOrchestrator::Impl::AdvanceIntent(
        const TaskId& task,
        const TaskIntentId& intentId,
        const TaskIntentStage stage,
        std::string reason,
        std::string detailJson) noexcept
    try
    {
        for (size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto aggregate = Store->LoadTask(task);
            if (!aggregate || !aggregate->Intent || aggregate->Intent->Id != intentId)
                return false;
            if (aggregate->Intent->Stage == stage && aggregate->Intent->ReasonCode == reason &&
                aggregate->Intent->DetailJson == detailJson)
                return true;
            AdvanceTaskIntentCommand command;
            command.Task = task;
            command.Intent = intentId;
            command.ExpectedIntentRevision = aggregate->Intent->Revision;
            command.RequestedStage = stage;
            command.DetailJson = std::move(detailJson);
            command.ReasonCode = std::move(reason);
            if (stage == TaskIntentStage::WaitingForProvider)
            {
                command.DeadlineAtUnixMilliseconds = RuntimeUnixMilliseconds() + 5000;
            }
            const auto metadata = MakeRuntimeTaskCommandMetadata(
                "orchestrator-intent-v1", task, intentId.Value() + ":" + std::to_string(static_cast<uint32_t>(stage)), aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
            if (!metadata)
                return false;
            command.Metadata = *metadata;
            const auto stored = Store->AdvanceTaskIntent(command);
            if (stored.Status == TaskStoreCommandStatus::VersionConflict)
            {
                reason = command.ReasonCode;
                detailJson = command.DetailJson;
                continue;
            }
            return stored.Succeeded();
        }
        return false;
    }
    catch (...)
    {
        return false;
    }

    bool RuntimeTaskOrchestrator::Impl::ChangeStatus(
        const TaskId& task,
        const TaskStatus status) noexcept
    try
    {
        for (size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto aggregate = Store->LoadTask(task);
            if (!aggregate)
                return false;
            if (aggregate->Task.Status == status)
                return true;
            ChangeTaskStatusCommand command;
            command.Task = task;
            command.RequestedStatus = status;
            command.Authority = TaskStatusAuthority::User;
            const auto metadata = MakeRuntimeTaskCommandMetadata(
                "orchestrator-status-v1", task, std::to_string(static_cast<uint32_t>(status)), aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
            if (!metadata)
                return false;
            command.Metadata = *metadata;
            const auto stored = Store->ChangeTaskStatus(command);
            if (stored.Status == TaskStoreCommandStatus::VersionConflict)
                continue;
            return stored.Succeeded();
        }
        return false;
    }
    catch (...)
    {
        return false;
    }
}
